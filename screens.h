#ifndef SML2_NATIVE_SCREENS_H
#define SML2_NATIVE_SCREENS_H

#include <stddef.h>
#include <stdint.h>

#define SCREEN_WIDTH 160
#define SCREEN_HEIGHT 144
#define SCREEN_PIXELS (SCREEN_WIDTH * SCREEN_HEIGHT)

enum ScreenButton {
    SCREEN_RIGHT = 1, SCREEN_LEFT = 2, SCREEN_UP = 4, SCREEN_DOWN = 8,
    SCREEN_A = 16, SCREEN_B = 32, SCREEN_SELECT = 64, SCREEN_START = 128
};

enum ScreenPhase {
    SCREEN_TITLE,
    SCREEN_FILE_LOADING, SCREEN_FILE_SELECT, SCREEN_GAMEPLAY_REQUESTED,
    SCREEN_DEMO_REQUESTED
};

/* These are named game fields and decoded graphics, not a CPU or bus model.
 * Original state/graphics addresses are documented at their producing code. */
typedef struct ScreenState {
    const uint8_t *rom;
    size_t rom_size;
    enum ScreenPhase phase;
    uint64_t frames;
    uint32_t phase_frames;
    uint16_t title_timer;
    uint8_t previous_buttons, frame_counter;
    uint8_t selected_file, gameplay_requested, demo_requested;
    uint8_t menu_action, action_timer, animation_timer;
    uint8_t mario_x, mario_y, facing, sprite_frame;
    uint8_t erase_mode, erase_cloud, easy_mode, falling_y;
    uint8_t sprite_in_front, sound_request, music_request;
    uint8_t file_records[3][0x50];
    uint8_t tile_data[0x1800], background_map[0x400];
    uint8_t sprites[160], sprite_count;
    uint8_t background_palette, sprite_palette[2];
} ScreenState;

int screens_init(ScreenState *s, const uint8_t *rom, size_t rom_size);
void screens_tick(ScreenState *s, uint8_t held_buttons);
void screens_render(const ScreenState *s, uint8_t shades[SCREEN_PIXELS]);
const char *screens_phase_name(const ScreenState *s);

/* Load the game's 3 records from an existing 8 KiB MBC1 save. The original
 * 00:28EC checksum/signature validation repairs invalid records identically. */
int screens_import_save(ScreenState *s, const uint8_t *save, size_t size);
void screens_export_records(const ScreenState *s, uint8_t records[0xf0]);

#endif
