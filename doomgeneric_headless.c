/*
 * doomgeneric_headless_interactive.c
 * Headless platform layer — injects key presses to actually play Doom.
 * Sends START key to get past title screen, then walks forward + turns.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <png.h>
#include <time.h>
#include "doomgeneric.h"

static int frame_count = 0;
static int game_started = 0;
static int tick_count = 0;
#define KEY_ENTER       13
#define KEY_UPARROW     172
#define KEY_DOWNARROW   173
#define KEY_LEFTARROW   174
#define KEY_RIGHTARROW  175
#define KEY_FIRE        157
#define KEY_USE         ' '
#define KEY_ESCAPE      27
#define KEY_UP          'w'
#define KEY_DOWN        's'
#define KEY_LEFT        'a'
#define KEY_RIGHT       'd'

void DG_Init(void) {
    system("mkdir -p /tmp/doom_frames");
    printf("[DG] Headless interactive mode. Frames -> /tmp/doom_frames\n");
    printf("[DG] Will auto-press ENTER to start game\n");
    srand(time(NULL));
}

void DG_DrawFrame(void) {
    char filename[512];
    snprintf(filename, sizeof(filename), "/tmp/doom_frames/frame_%04d.png", frame_count);
    
    FILE *fp = fopen(filename, "wb");
    if (!fp) return;
    
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    png_init_io(png, fp);
    png_set_IHDR(png, info, 320, 200, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);
    
    uint32_t *src = (uint32_t *)DG_ScreenBuffer;
    for (int y = 0; y < 200; y++) {
        png_bytep row = (png_bytep)malloc(320 * 3);
        for (int x = 0; x < 320; x++) {
            uint32_t px = src[y * 320 + x];
            row[x*3+0] = (px >> 16) & 0xFF;
            row[x*3+1] = (px >> 8) & 0xFF;
            row[x*3+2] = px & 0xFF;
        }
        png_write_row(png, row);
        free(row);
    }
    
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    
    if (frame_count % 35 == 0) {
        printf("[DG] Frame %d captured\n", frame_count);
    }
    frame_count++;
}

void DG_SleepMs(uint32_t ms) {
    // Minimal sleep — we want max speed
    if (ms > 1) ms = 1;
    struct timespec ts = {0, ms * 1000000};
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void) {
    tick_count++;
    return tick_count * 28;  // ~35 ticks per second
}

// Key injection state machine
static int current_key = 0;
static int key_held = 0;
static int key_release_at = 0;

int DG_GetKey(int *pressed, unsigned char *key) {
    tick_count++;
    
    if (!game_started) {
        // Press ENTER to start the game from title screen
        *pressed = 1;
        *key = KEY_ENTER;
        game_started = 1;
        printf("[DG] Pressed ENTER to start game\n");
        return 1;
    }
    
    // Release ENTER after a few ticks
    if (tick_count < 10) {
        *pressed = 0;
        *key = KEY_ENTER;
        return 1;
    }
    
    // Simple gameplay: cycle through actions
    int phase = (tick_count / 35) % 6;  // Change action every ~1 second
    
    switch (phase) {
        case 0: // Walk forward
            *pressed = 1;
            *key = KEY_UPARROW;
            break;
        case 1: // Turn right
            *pressed = 1;
            *key = KEY_RIGHTARROW;
            break;
        case 2: // Walk forward
            *pressed = 1;
            *key = KEY_UPARROW;
            break;
        case 3: // Fire
            *pressed = 1;
            *key = KEY_FIRE;
            break;
        case 4: // Walk forward
            *pressed = 1;
            *key = KEY_UPARROW;
            break;
        case 5: // Use (open doors)
            *pressed = 1;
            *key = KEY_USE;
            break;
    }
    return 1;
}

void DG_SetWindowTitle(const char * title) {
    // No-op
}

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    int frame_limit = 700;  // ~20 seconds at 35fps
    if (argc > 1) frame_limit = atoi(argv[1]);
    
    printf("[DG] Running for %d frames\n", frame_limit);
    int ticks = 0;
    int max_ticks = frame_limit * 3;  // Doom runs at 35 ticks/sec, draw is slower
    while (frame_count < frame_limit && ticks < max_ticks) {
        doomgeneric_Tick();
        ticks++;
    }
    printf("[DG] Done. %d frames captured in %d ticks.\n", frame_count, ticks);
    return 0;
}
