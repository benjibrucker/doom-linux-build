/*
 * doomgeneric_headless.c — v2 "control build" for turn-based DOOM in a sandbox.
 *
 * Deterministic replay interface (DOOM-demo style): the game is driven by a
 * per-tic key script, so re-running the same script reproduces the same game
 * byte-for-byte. Turn-based play = append this chunk's keys to the cumulative
 * script, re-run from tic 0, and only record frames from where the last chunk
 * ended (--record-from).
 *
 * Usage:
 *   ./doomgeneric_headless -iwad freedoom1.wad [options]
 *     --input FILE        per-tic key script (see format below); no file = no input
 *     --tics N            total game tics to simulate (default: script length + 105)
 *     --outdir DIR        PNG output dir (default /tmp/doom_frames)
 *     --record-from T     don't write frames before tic T (default 0)
 *     --every K           write every Kth frame (default 1)
 *   All other args pass through to DOOM (-iwad, -skill, -warp, ...).
 *   -singletics is forced: exactly one game tic per rendered frame, no wall clock.
 *
 * Key script format — one line per step, applied for one tic unless "xN":
 *   FWD x35            hold forward for 35 tics (1 second)
 *   FWD+TURNR x10      hold forward and turn-right together
 *   ENTER x2           tap enter (keydown fires on first tic of a hold)
 *   . x20              nothing held for 20 tics
 *   # comment / blank lines ignored
 * Tokens: ENTER ESC FWD BACK TURNL TURNR STRAFEL STRAFER FIRE USE RUN TAB
 *         Y N 1-7 (weapons), K<decimal> for a raw DOOM keycode.
 * Press/release events are derived from the diff of consecutive tics' held
 * sets, so holding a key across lines sends exactly one keydown.
 *
 * Menu cheat-sheet (vanilla flow): at the title screen any key opens the menu;
 * then ENTER selects New Game -> episode -> skill (default Hurt Me Plenty).
 * So a new game is:  ENTER x2 / . x10  repeated 4 times.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <png.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomtype.h"

extern boolean singletics;   /* d_loop.c: one game tic per TryRunTics() call */

#define MAX_TICS      (35 * 60 * 30)   /* 30 minutes of game time */
#define MAX_HELD      8                /* simultaneous keys per tic */
#define EVQ_SIZE      64

/* ---- config from argv ------------------------------------------------- */
static const char *opt_input = NULL;
static const char *opt_outdir = "/tmp/doom_frames";
static long opt_tics = -1;
static long opt_record_from = 0;
static long opt_every = 1;

/* ---- per-tic held-key script ------------------------------------------ */
static unsigned char (*script)[MAX_HELD];  /* script[tic][slot], 0 = empty */
static long script_len = 0;

static int token_to_key(const char *t) {
    if (!strcmp(t, "ENTER"))   return KEY_ENTER;
    if (!strcmp(t, "ESC"))     return KEY_ESCAPE;
    if (!strcmp(t, "FWD") || !strcmp(t, "UP"))    return KEY_UPARROW;
    if (!strcmp(t, "BACK") || !strcmp(t, "DOWN")) return KEY_DOWNARROW;
    if (!strcmp(t, "TURNL") || !strcmp(t, "LEFT"))  return KEY_LEFTARROW;
    if (!strcmp(t, "TURNR") || !strcmp(t, "RIGHT")) return KEY_RIGHTARROW;
    if (!strcmp(t, "STRAFEL")) return KEY_STRAFE_L;
    if (!strcmp(t, "STRAFER")) return KEY_STRAFE_R;
    if (!strcmp(t, "FIRE"))    return KEY_FIRE;
    if (!strcmp(t, "USE"))     return KEY_USE;
    if (!strcmp(t, "RUN"))     return KEY_RSHIFT;
    if (!strcmp(t, "TAB"))     return KEY_TAB;
    if (!strcmp(t, "Y"))       return 'y';
    if (!strcmp(t, "N"))       return 'n';
    if (t[0] == 'K' && t[1])   return atoi(t + 1) & 0xFF;
    if (t[0] >= '1' && t[0] <= '7' && !t[1]) return t[0];
    fprintf(stderr, "[DG] unknown key token '%s'\n", t);
    exit(2);
}

static void load_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[DG] cannot open input script %s\n", path); exit(2); }
    script = calloc(MAX_TICS, MAX_HELD);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char keypart[128] = "";
        long repeat = 1;
        char xpart[64] = "";
        int n = sscanf(line, " %127s %63s", keypart, xpart);
        if (n < 1 || !keypart[0]) continue;
        if (n == 2 && (xpart[0] == 'x' || xpart[0] == 'X'))
            repeat = atol(xpart + 1);
        if (repeat < 1) repeat = 1;

        unsigned char held[MAX_HELD] = {0};
        int nh = 0;
        if (strcmp(keypart, ".") != 0) {
            char *tok = strtok(keypart, "+");
            while (tok && nh < MAX_HELD) {
                held[nh++] = (unsigned char)token_to_key(tok);
                tok = strtok(NULL, "+");
            }
        }
        for (long r = 0; r < repeat && script_len < MAX_TICS; r++, script_len++)
            memcpy(script[script_len], held, MAX_HELD);
    }
    fclose(f);
    printf("[DG] input script: %ld tics (%.1f s of game time)\n",
           script_len, script_len / 35.0);
}

/* ---- event queue: press/release diffs, drained per tic ----------------- */
static struct { int pressed; unsigned char key; } evq[EVQ_SIZE];
static int evq_head = 0, evq_tail = 0;
static long input_tic = 0;      /* which script tic we're on */
static int draining = 0;

static int was_held(long tic, unsigned char key) {
    if (tic < 0 || tic >= script_len) return 0;
    for (int i = 0; i < MAX_HELD; i++)
        if (script[tic][i] == key) return 1;
    return 0;
}

static void queue_tic_events(long tic) {
    /* releases first, then presses */
    if (tic - 1 >= 0 && tic - 1 < script_len) {
        for (int i = 0; i < MAX_HELD; i++) {
            unsigned char k = script[tic - 1][i];
            if (k && !was_held(tic, k)) {
                evq[evq_tail].pressed = 0; evq[evq_tail].key = k;
                evq_tail = (evq_tail + 1) % EVQ_SIZE;
            }
        }
    }
    if (tic < script_len) {
        for (int i = 0; i < MAX_HELD; i++) {
            unsigned char k = script[tic][i];
            if (k && !was_held(tic - 1, k)) {
                evq[evq_tail].pressed = 1; evq[evq_tail].key = k;
                evq_tail = (evq_tail + 1) % EVQ_SIZE;
            }
        }
    }
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if (!draining) {                 /* first call this tic */
        queue_tic_events(input_tic);
        input_tic++;
        draining = 1;
    }
    if (evq_head == evq_tail) {      /* queue empty -> end of this tic's events */
        draining = 0;
        return 0;
    }
    *pressed = evq[evq_head].pressed;
    *key = evq[evq_head].key;
    evq_head = (evq_head + 1) % EVQ_SIZE;
    return 1;
}

/* ---- frame output ------------------------------------------------------ */
static long frames_written = 0;
static long tics_done = 0;

void DG_Init(void) {
    singletics = true;   /* deterministic: no wall clock, 1 tic per Tick() */
    mkdir(opt_outdir, 0755);
    printf("[DG] headless control build v2 | %dx%d | frames -> %s\n",
           DOOMGENERIC_RESX, DOOMGENERIC_RESY, opt_outdir);
}

void DG_DrawFrame(void) {
    tics_done++;
    long tic = tics_done - 1;
    if (tic < opt_record_from || (tic - opt_record_from) % opt_every) return;

    char filename[512];
    snprintf(filename, sizeof(filename), "%s/frame_%05ld.png", opt_outdir, frames_written);
    FILE *fp = fopen(filename, "wb");
    if (!fp) { fprintf(stderr, "[DG] cannot write %s\n", filename); exit(2); }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_init_io(png, fp);
    png_set_IHDR(png, info, DOOMGENERIC_RESX, DOOMGENERIC_RESY, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);

    static png_byte *row = NULL;
    if (!row) row = malloc(DOOMGENERIC_RESX * 3);
    uint32_t *src = (uint32_t *)DG_ScreenBuffer;
    for (int y = 0; y < DOOMGENERIC_RESY; y++) {
        for (int x = 0; x < DOOMGENERIC_RESX; x++) {
            uint32_t px = src[y * DOOMGENERIC_RESX + x];
            row[x * 3 + 0] = (px >> 16) & 0xFF;
            row[x * 3 + 1] = (px >> 8) & 0xFF;
            row[x * 3 + 2] = px & 0xFF;
        }
        png_write_row(png, row);
    }
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    frames_written++;

    if (tic % 175 == 0)
        printf("[DG] tic %ld / frame %ld\n", tic, frames_written);
}

/* ---- time: synthetic, deterministic (singletics ignores it anyway) ----- */
static uint32_t fake_ms = 0;
uint32_t DG_GetTicksMs(void) { fake_ms += 10; return fake_ms; }
void DG_SleepMs(uint32_t ms) { (void)ms; }
void DG_SetWindowTitle(const char *title) { (void)title; }

/* ---- main --------------------------------------------------------------- */
int main(int argc, char **argv) {
    /* pull out our --flags, pass the rest (plus -singletics) to DOOM */
    static char *dargv[64];
    int dargc = 0;
    dargv[dargc++] = argv[0];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--input"))            opt_input = argv[++i];
        else if (!strcmp(argv[i], "--outdir"))      opt_outdir = argv[++i];
        else if (!strcmp(argv[i], "--tics"))        opt_tics = atol(argv[++i]);
        else if (!strcmp(argv[i], "--record-from")) opt_record_from = atol(argv[++i]);
        else if (!strcmp(argv[i], "--every"))       opt_every = atol(argv[++i]);
        else if (dargc < 62)                        dargv[dargc++] = argv[i];
    }
    dargv[dargc] = NULL;
    if (opt_every < 1) opt_every = 1;

    if (opt_input) load_script(opt_input);
    else { script = calloc(1, MAX_HELD); printf("[DG] no input script (attract mode)\n"); }
    if (opt_tics < 0) opt_tics = script_len + 105;  /* script + 3 s tail */

    printf("[DG] simulating %ld tics, recording from tic %ld (every %ld)\n",
           opt_tics, opt_record_from, opt_every);

    doomgeneric_Create(dargc, dargv);
    while (tics_done < opt_tics)
        doomgeneric_Tick();

    printf("[DG] done: %ld tics, %ld frames -> %s\n",
           tics_done, frames_written, opt_outdir);
    return 0;
}
