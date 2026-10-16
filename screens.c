#include "screens.h"

#include <string.h>

static const uint8_t *asset(const ScreenState *s, unsigned bank,
                            unsigned address)
{
    return s->rom + bank * 0x4000u + address - 0x4000u;
}

static void tiles(ScreenState *s, unsigned bank, unsigned source,
                  unsigned destination, size_t count)
{
    memcpy(s->tile_data + destination - 0x8000, asset(s, bank, source), count);
}

/* 00:2914: an empty save has five lives, no cleared levels, and the original
 * checksum seed and signature. The record stride is 0x50, not 0x40.
 * The ROM really writes checksum 2 although these new bytes sum to 5;
 * original execution confirms this. Do not silently "correct" that quirk. */
static void empty_record(uint8_t record[0x50])
{
    memset(record, 0, 0x43);
    record[0x43] = 5;
    record[0x44] = 0;
    record[0x45] = 2;
    record[0x46] = 0x12;
    record[0x47] = 0x34;
    record[0x48] = 0x56;
    record[0x49] = 0x78;
}

static void validate_record(uint8_t record[0x50])
{
    unsigned i;
    uint8_t sum = 0;
    for (i = 0; i < 0x45; ++i) sum = (uint8_t)(sum + record[i]);
    if (sum != record[0x45] || record[0x46] != 0x12 ||
        record[0x47] != 0x34 || record[0x48] != 0x56 || record[0x49] != 0x78)
        empty_record(record);
}

int screens_import_save(ScreenState *s, const uint8_t *save, size_t size)
{
    unsigned i;
    if (!save || size != 0x2000) return 0;
    memcpy(s->file_records, save, sizeof s->file_records);
    for (i = 0; i < 3; ++i) validate_record(s->file_records[i]);
    return 1;
}

void screens_export_records(const ScreenState *s, uint8_t records[0xf0])
{
    memcpy(records, s->file_records, 0xf0);
}

int screens_init(ScreenState *s, const uint8_t *rom, size_t rom_size)
{
    unsigned i;
    if (!s || !rom || rom_size != 0x80000) return 0;
    memset(s, 0, sizeof *s);
    s->rom = rom;
    s->rom_size = rom_size;
    s->phase = SCREEN_TITLE;
    s->music_request = 0xff; /* 05:407D title loader stops the prior song. */
    /* 00:2CC4 -> 05:4043 -> 00:2ABE: title tiles and 32x32 map. */
    tiles(s, 5, 0x41e5, 0x8000, 0x1800);
    memcpy(s->background_map, asset(s, 5, 0x59e5), 0x400);
    s->background_palette = 0xe4;
    s->sprite_palette[0] = 0xd0;
    s->sprite_palette[1] = 0x38;
    s->title_timer = 0x0540;
    for (i = 0; i < 3; ++i) empty_record(s->file_records[i]);
    return 1;
}

static void file_select_init(ScreenState *s)
{
    /* 0C:4000 -> 00:2B8B. Retain the loaded title bytes where this routine
     * does not overwrite them; the ROM likewise does not clear VRAM here. */
    tiles(s, 7, 0x5e00, 0x9200, 0x600);
    tiles(s, 7, 0x6a00, 0x8e80, 0x380);
    tiles(s, 6, 0x4000, 0x8000, 0x800);
    tiles(s, 0x1b, 0x7000, 0x8800, 0x300);
    tiles(s, 0x0c, 0x6aca, 0x8b00, 0x380);
    memcpy(s->background_map, asset(s, 0x0c, 0x4a52), 0x400);
    s->phase = SCREEN_FILE_SELECT;
    s->phase_frames = 0;
    s->mario_x = 0x28; /* FFC2 */
    s->mario_y = 0x10; /* FFC0 */
    s->menu_action = 0; /* A277 */
    s->animation_timer = 0; /* A222 */
    s->erase_mode = 0; /* A2C6 */
    s->easy_mode = 0; /* A2E4 */
    s->sound_request = 5; /* A460 */
    s->music_request = 13; /* 0C:4000, file-selection music requestA468. */
    s->sprite_count = 0;
}

/* 01:5297: table 01:4000 contains pointers to terminated metasprites.
 * Entries are y offset, x offset, tile, attributes; 0x80 ends the list. */
static void metasprite(ScreenState *s, uint8_t frame, uint8_t y, uint8_t x,
                       int foreground)
{
    const uint8_t *table = asset(s, 1, 0x4000 + frame * 2u);
    unsigned address = table[0] | (unsigned)table[1] << 8;
    unsigned offset;
    const uint8_t *p;
    if (address < 0x4000 || address >= 0x8000) return;
    p = asset(s, 1, address);
    for (offset = 0; address + offset + 4 <= 0x8000 &&
         p[offset] != 0x80 && s->sprite_count < 40; offset += 4) {
        uint8_t *o = s->sprites + 4u * s->sprite_count++;
        o[0] = (uint8_t)(y + p[offset]);
        o[1] = (uint8_t)(x + p[offset + 1]);
        o[2] = p[offset + 2];
        o[3] = p[offset + 3] | (foreground ? 0 : 0x80);
    }
}

/* 01:5267 / 527C: two BCD digits, using unchanged ROM tiles 0x80..0x8F. */
static void number_sprite(ScreenState *s, uint8_t value, uint8_t x)
{
    unsigned i;
    for (i = 0; i < 2 && s->sprite_count < 40; ++i) {
        uint8_t *o = s->sprites + 4u * s->sprite_count++;
        o[0] = 0x58;
        o[1] = (uint8_t)(x + i * 8);
        o[2] = (uint8_t)(0x80 + (i ? value & 15 : value >> 4));
        o[3] = 0;
    }
}

static uint8_t original_buttons(uint8_t input)
{
    /* The host groups directions first. 00:1FD2 stores A/B/Select/Start
     * in bits 0..3 and Right/Left/Up/Down in bits 4..7. */
    return (uint8_t)((input << 4) | (input >> 4));
}

static void idle_frame(ScreenState *s)
{
    /* 0C:431A: 1 faces right (frame 0), 0xFF left (frame 4).
     * The original 8-bit add wraps 0xFE + 2 to zero. */
    s->sprite_frame = (uint8_t)(((s->facing ^ 0xff) + 2) * 2);
}

static void tick_idle(ScreenState *s, uint8_t held, uint8_t pressed)
{
    /* 0C:4326: Select toggles easy mode; Left/Right move between the three
     * files and erase pipe; Start/Down enters a file; A enters erase pipe. */
    if (pressed == 4) {
        if (!s->erase_mode) {
            s->easy_mode ^= 1;
            idle_frame(s);
        }
        return;
    }
    if (held == 0x10 && s->mario_x < 0x88) {
        s->menu_action = 2;
        s->action_timer = 0x20;
        s->sprite_frame = (uint8_t)(1 + ((s->animation_timer & 12) >> 2));
        return;
    }
    if (held == 0x20 && s->mario_x >= 0x30) {
        s->menu_action = 3;
        s->action_timer = 0x20;
        s->sprite_frame = (uint8_t)(5 + ((s->animation_timer & 12) >> 2));
        return;
    }
    if ((pressed == 0x80 || pressed == 8) && s->mario_x < 0x80) {
        s->menu_action = 5;
        s->action_timer = 0x40;
        s->sound_request = 5;
        return;
    }
    if (pressed == 1 && s->mario_x == 0x88) {
        s->menu_action = 4;
        s->action_timer = 0x37;
        s->sound_request = 1;
        return;
    }
    idle_frame(s);
}

static void tick_action(ScreenState *s, uint8_t held, uint8_t pressed)
{
    switch (s->menu_action) {
    case 0: /* 0C:43D6: descend from the pipe at the upper left. */
        s->sprite_frame = 0x10;
        s->sprite_in_front = 0;
        if (s->mario_y >= 0x30) {
            s->sprite_frame = 0x0a;
            s->sprite_in_front = 1;
        }
        if (s->mario_y >= 0x7c) {
            s->menu_action = 1;
            s->facing = 1;
            s->mario_y = 0x7c;
        } else {
            s->mario_y = (uint8_t)(s->mario_y + 2);
            if (s->mario_y >= 0x40) ++s->mario_y;
        }
        break;
    case 1:
        tick_idle(s, held, pressed);
        break;
    case 2: /* 0C:42F1 */
    case 3: /* 0C:42C8 */
        s->facing = s->menu_action == 2 ? 1 : 0xff;
        if (++s->animation_timer >= 12) s->animation_timer = 0;
        s->sprite_frame = (uint8_t)((s->menu_action == 2 ? 1 : 5) +
                                    ((s->animation_timer & 12) >> 2));
        s->mario_x = (uint8_t)(s->mario_x + (s->menu_action == 2 ? 1 : -1));
        if (--s->action_timer == 0) s->menu_action = 1;
        break;
    case 4: /* 0C:426D: enter the erase pipe, toggle mode, come back out. */
        s->sprite_frame = 9;
        --s->action_timer;
        if (s->action_timer == 0x2b) {
            s->erase_mode ^= 0xff;
            s->facing = 0xff;
            s->sound_request = 8;
            if (s->erase_mode) s->easy_mode = 0;
        }
        if (s->action_timer >= 0x2b) s->mario_y -= 2;
        else if (s->action_timer >= 0x0b) s->erase_cloud = 1;
        else {
            s->mario_y += 2;
            if (!s->action_timer) {
                s->menu_action = 1;
                s->mario_y = 0x7c;
            }
        }
        break;
    case 5: /* 0C:41C6: 64-frame pipe entry, then load or erase a save. */
        s->sprite_in_front = 0;
        s->sprite_frame = 0x10;
        ++s->mario_y;
        if (--s->action_timer) break;
        s->selected_file = (uint8_t)(((s->mario_x & 0x60) >> 5) - 1);
        if (!s->erase_mode) {
            s->phase = SCREEN_GAMEPLAY_REQUESTED;
            s->gameplay_requested = 1;
        } else {
            empty_record(s->file_records[s->selected_file]);
            s->action_timer = 0x80;
            s->menu_action = 6;
            s->erase_mode = 0;
            s->falling_y = 0x90;
            s->sound_request = 5;
        }
        break;
    case 6: /* 0C:4140: erase effect and Mario's return from the top. */
        --s->action_timer;
        if (s->action_timer == 0) {
            s->mario_y = 0x7c;
            s->menu_action = 1;
            s->erase_mode = 0;
            s->sprite_in_front = 0;
            break;
        }
        s->sprite_frame = (uint8_t)(0xd3 + ((s->action_timer >> 5) & 7));
        s->sprite_in_front = 0;
        if (s->action_timer >= 0x40)
            metasprite(s, s->sprite_frame,
                       (uint8_t)(0x70 + (s->action_timer >> 2)), s->mario_x, 0);
        s->falling_y = (uint8_t)(s->falling_y - ((s->action_timer >> 4) - 4));
        if (s->action_timer < 0x40 && s->falling_y >= 0x7c) {
            s->falling_y = s->mario_y = 0x7c;
            s->sprite_frame = 0x10;
            s->menu_action = 1;
            s->erase_mode = 0;
            break;
        }
        {
            static const uint8_t frames[4] = {0x10, 0, 0x11, 4};
            s->sprite_frame = frames[(s->action_timer & 0x18) >> 3];
            metasprite(s, s->sprite_frame, s->falling_y, s->mario_x, 0);
        }
        break;
    }
}

static void file_select_tick(ScreenState *s, uint8_t held, uint8_t pressed)
{
    unsigned i;
    uint8_t draw_y, draw_frame;
    s->sprite_count = 0;
    s->sprite_in_front = 1;
    s->erase_cloud = 0;
    if (s->easy_mode) metasprite(s, 0xf1, 0x30, 0x44, 1);
    tick_action(s, held, pressed);
    draw_y = s->mario_y;
    draw_frame = s->sprite_frame;
    if (s->erase_cloud) draw_frame = (uint8_t)(0xcf + ((s->frame_counter & 12) >> 2));
    else {
        if (s->easy_mode) draw_frame += 0x20;
        if (s->erase_mode) {
            static const uint8_t walk[4] = {0, 1, 2, 1};
            uint8_t step = walk[(s->frame_counter & 12) >> 2];
            draw_frame = (uint8_t)((s->facing == 1 ? 0xc9 : 0xcc) + step);
            draw_y = (uint8_t)(draw_y + 1 - (step & 1));
        }
    }
    metasprite(s, draw_frame, draw_y, s->mario_x, s->sprite_in_front);
    for (i = 0; i < 3; ++i)
        number_sprite(s, s->file_records[i][0x42], (uint8_t)(0x20 + i * 0x20));
}

void screens_tick(ScreenState *s, uint8_t held_buttons)
{
    uint8_t held = original_buttons(held_buttons);
    uint8_t pressed = (uint8_t)(held & ~s->previous_buttons);
    s->previous_buttons = held;
    s->sound_request = 0;
    ++s->frames;
    ++s->phase_frames;
    ++s->frame_counter;
    if (s->title_timer) --s->title_timer;
    if (s->phase == SCREEN_TITLE) {
        /* 05:407C: the first 65 title frames do not accept Start. The
         * original attract-mode transition is exposed to the host. */
        if (s->title_timer < 0x500 && s->title_timer >= 0x100 && pressed == 8)
            s->phase = SCREEN_FILE_LOADING;
        else if (s->title_timer < 0x100) {
            s->phase = SCREEN_DEMO_REQUESTED;
            s->demo_requested = 1;
        }
    } else if (s->phase == SCREEN_FILE_LOADING) file_select_init(s);
    else if (s->phase == SCREEN_FILE_SELECT) file_select_tick(s, held, pressed);
}

static uint8_t tile_pixel(const uint8_t *tile, unsigned x, unsigned y)
{
    unsigned bit = 7 - x;
    return (uint8_t)(((tile[y * 2] >> bit) & 1) |
                    (((tile[y * 2 + 1] >> bit) & 1) << 1));
}

void screens_render(const ScreenState *s, uint8_t shades[SCREEN_PIXELS])
{
    unsigned x, y, i;
    /* Both screens use the signed $8800 tile mode (LCDC bit 4 clear).
     * Map byte 0 is at $9000; byte 0x80 is at $8800. */
    for (y = 0; y < SCREEN_HEIGHT; ++y) {
        uint8_t scanline[10];
        unsigned count = 0;
        for (i = 0; i < s->sprite_count && count < 10; ++i) {
            int sprite_y = (int)s->sprites[i * 4] - 16;
            if ((int)y >= sprite_y && (int)y < sprite_y + 8) scanline[count++] = (uint8_t)i;
        }
        for (x = 0; x < SCREEN_WIDTH; ++x) {
            uint8_t index = s->background_map[(y / 8) * 32 + x / 8];
            int tile_number = index < 128 ? index : (int)index - 256;
            const uint8_t *tile = s->tile_data + 0x1000 + tile_number * 16;
            uint8_t bg = tile_pixel(tile, x & 7, y & 7);
            uint8_t shade = (uint8_t)((s->background_palette >> (bg * 2)) & 3);
            unsigned best_x = 256;
            for (i = 0; i < count; ++i) {
                const uint8_t *o = s->sprites + scanline[i] * 4;
                int sx = (int)o[1] - 8, sy = (int)o[0] - 16;
                unsigned tx, ty;
                uint8_t color;
                if ((int)x < sx || (int)x >= sx + 8 || o[1] >= best_x) continue;
                tx = (unsigned)((int)x - sx);
                ty = (unsigned)((int)y - sy);
                if (o[3] & 0x20) tx = 7 - tx;
                if (o[3] & 0x40) ty = 7 - ty;
                color = tile_pixel(s->tile_data + o[2] * 16u, tx, ty);
                if (!color) continue;
                best_x = o[1];
                shade = (o[3] & 0x80) && bg ?
                    (uint8_t)((s->background_palette >> (bg * 2)) & 3) :
                    (uint8_t)((s->sprite_palette[(o[3] >> 4) & 1] >> (color * 2)) & 3);
            }
            shades[y * SCREEN_WIDTH + x] = shade;
        }
    }
}

const char *screens_phase_name(const ScreenState *s)
{
    switch (s->phase) {
    case SCREEN_TITLE: return "title";
    case SCREEN_FILE_LOADING: return "file-loading";
    case SCREEN_FILE_SELECT: return "file-select";
    case SCREEN_GAMEPLAY_REQUESTED: return "gameplay-requested";
    case SCREEN_DEMO_REQUESTED: return "demo-requested";
    }
    return "unknown";
}
