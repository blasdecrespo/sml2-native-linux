#include "level.h"

#include <string.h>

/* This is a readable translation of selected game routines, not a CPU
 * interpreter. Addresses below refer to SHA256 5450dce1...ce1392e.
 * Implements original level/graphics formats, player movement modes, tile
 * collisions, block effects, pickups and player damage/death/exit states.
 * Actors own block contents and boss AI. Pipe travel, room scripts and
 * arena loading retain their original frame and graphics-transfer timing.
 */

enum { RIGHT = 1, LEFT = 2, UP = 4, DOWN = 8, JUMP = 16, RUN = 32, START = 128 };

static unsigned word(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }

static const uint8_t *bank_bytes(const Level *g, unsigned bank, unsigned address,
                                 size_t count) {
    if (address < 0x4000 || address >= 0x8000 || count > 0x8000 - address)
        return NULL;
    size_t offset = (size_t)bank * 0x4000 + address - 0x4000;
    if (offset > g->rom_size || count > g->rom_size - offset) return NULL;
    return g->rom + offset;
}

static bool copy_tiles(Level *g, unsigned bank, unsigned source,
                       unsigned destination, size_t count) {
    const uint8_t *p = bank_bytes(g, bank, source, count);
    if (!p || destination < 0x8000 || count > 0x9800 - destination) return false;
    memcpy(g->tiles + destination - 0x8000, p, count);
    return true;
}

/* 00:0361. Runs stop at each 256-byte row, discarding their remainder.
 * A high-bit token in column 255 does not consume a length byte. A zero
 * length means 256 repetitions, reflecting the original 8-bit decrement. */
static bool decode_map(Level *g, unsigned bank, unsigned address) {
    unsigned out = 0;
    while (out < sizeof(g->map)) {
        const uint8_t *p = bank_bytes(g, bank, address++, 1);
        if (!p) return false;
        uint8_t token = *p;
        ((uint8_t *)g->map)[out++] = token & 0x7f;
        if ((token & 0x80) && (out & 255)) {
            p = bank_bytes(g, bank, address++, 1);
            if (!p) return false;
            unsigned count = *p ? *p : 256;
            unsigned remaining = 256 - (out & 255);
            if (count > remaining) count = remaining;
            memset((uint8_t *)g->map + out, token & 0x7f, count);
            out += count;
        }
    }
    return true;
}

bool level_load_checkpoint(Level *g, unsigned id, bool checkpoint) {
    if (id >= 32 || g->rom_size != 0x80000) return false;
    const uint8_t *p = bank_bytes(g, 1, 0x55cb + id * 2, 2);
    if (!p) return false;
    p = bank_bytes(g, 1, word(p), sizeof(g->descriptor));
    if (!p) return false;
    memcpy(g->descriptor, p, sizeof(g->descriptor));
    if (checkpoint) memcpy(g->descriptor, g->rom + 0x377b + id * 8, 8);
    g->checkpoint = checkpoint;
    memcpy(g->palettes, g->descriptor + 15, 3);
    g->id = id;
    unsigned bank = g->descriptor[13], sublevel = g->descriptor[18] & 15;
    p = bank_bytes(g, bank, 0x42f0 + sublevel * 2, 2);
    if (!p || !decode_map(g, bank, word(p))) return false;
    p = bank_bytes(g, 8, word(g->descriptor + 11), sizeof(g->metatiles));
    if (!p) return false;
    memcpy(g->metatiles, p, sizeof(g->metatiles));
    p = bank_bytes(g, bank, 0x4000, 2);
    if (!p) return false;
    p = bank_bytes(g, bank, word(p) + 48 * sublevel, sizeof(g->camera_rules));
    if (!p) return false;
    memcpy(g->camera_rules, p, sizeof(g->camera_rules));
    p = bank_bytes(g, bank, 0x4002, 2);
    if (!p) return false;
    p = bank_bytes(g, bank, word(p) + 48 * sublevel, sizeof(g->room_scripts));
    if (!p) return false;
    memcpy(g->room_scripts, p, sizeof(g->room_scripts));

    /* 00:0669: the graphics selector's two nibbles select independent sets. */
    unsigned selector = g->rom[0x360a + id];
    unsigned sprite_set = selector & 15;
    unsigned set = selector >> 4;
    memset(g->tiles, 0, sizeof(g->tiles));
    if (!copy_tiles(g, 6, 0x4000 + (sprite_set == 0 ? 0 : sprite_set == 1 ? 0x800 : 0x1000),
                    0x8000, 0x800)) return false;
    unsigned graphics_address = word(g->rom + 0x763 + 2 * set);
    if (!copy_tiles(g, 7, graphics_address, 0x8e80, 0x380) ||
        !copy_tiles(g, 0x1b, 0x7000 + set * 0x300, 0x8800, 0x300)) return false;
    p = g->rom + 0x362a + id * 4;
    if (!copy_tiles(g, p[0], word(p + 2), 0x8b00, 0x380)) return false;
    p = g->rom + 0x36aa + id * 4;
    if (!copy_tiles(g, p[0], word(p + 2), 0x9200, 0x600)) return false;

    g->y = (int)word(g->descriptor);
    g->x = (int)word(g->descriptor + 2);
    g->camera_y = (int)word(g->descriptor + 4);
    g->camera_x = (int)word(g->descriptor + 6);
    g->camera_y_origin = g->y;
    g->background_transfer_pending = false;
    g->auto_scroll = g->rom[0x1f71 + id] != 0;
    g->camera_rise_early = g->rom[0x1f51 + id] != 0; /* A2D9, upward threshold. */
    g->background_shake = 0;
    /* 00:0F2A and scenes2/3 reset only their named work. Animation,
     * camera latches and movement/effect records outside that list survive
     * a stage handoff. level_init supplies the initial zeroed state. */
    g->previous_screen_x = g->previous_screen_y = 0x40;
    g->speed_index = 0x80;
    g->jump_index = 0x18;
    g->facing = 1;
    g->ticks = 0;
    g->asset_animation_selector = (uint8_t)(g->rom[0x35ea + id] + 1);
    g->time = g->descriptor[19] * 100;
    g->timer_countdown = 48; /* A24B, initialized at 00:04B5 */
    g->paused = g->complete = false;
    g->stage_cleared = g->return_requested = false;
    g->life_state = LEVEL_ALIVE;
    g->goal_walk = false;
    g->game_over = false;
    g->pending_damage = false;
    g->room_transition = 0;
    g->room_script_started_this_frame = g->room_script_frame = g->room_script_unsupported = false;
    g->room_script_address = g->room_transfer_source = g->room_transfer_destination = g->room_transfer_remaining = 0;
    g->room_transfer_bank = g->room_enemy_count = g->scene_loading_frames = 0;
    g->boss_mode = g->descriptor[18] & 0xf0;
    g->input_wait = 0;
    g->effective_held = g->effective_pressed = 0;
    g->boss_started_this_frame = g->boss_defeated = false;
    g->ending_requested = false;
    g->movement_mode = g->rom[0x1f91 + id] & (uint8_t)~4;
    g->bubble = false;
    g->swim_stroke_active = false;
    g->invulnerability = g->death_wait = 0;
    g->star_frames &= 255; /* Only A271 is cleared by00:0F2A. */
    g->star_chain = 0;
    g->star_chain_visible = false;
    g->invulnerability_active = false;
    g->death_started_this_frame = false;
    g->death_arc_frame = false;
    g->exit = LEVEL_EXIT_NONE;
    g->exit_wait = 0;
    g->exit_started_this_frame = false;
    g->sound_request = 0;
    g->sound_secondary = g->sound_noise = g->pause_request = 0;
    g->default_music = g->music_request = g->descriptor[14];
    g->volume_duck = false;
    g->fire_pose_active = false;
    g->event_count = 0;
    memset(g->effects, 0, sizeof(g->effects));
    return true;
}

bool level_load(Level *g, unsigned id) { return level_load_checkpoint(g, id, false); }

bool level_init(Level *g, const uint8_t *rom, size_t rom_size) {
    memset(g, 0, sizeof(*g));
    g->rom = rom;
    g->rom_size = rom_size;
    g->lives = 5;
    return rom && level_load(g, 0);
}

void level_reset(Level *g) { (void)level_load_checkpoint(g, g->id, g->checkpoint); }

/* 00:1EFA / 1F2F form a map address in B000-DFFF. Logical rooms
 * above row47 address echoRAM E000-FDFF, which aliases C000-DDFF. */
static int map_row(int map_y) {
    unsigned row = ((uint16_t)map_y & 0x0fff) >> 4;
    if (row >= 48 && row < 78) row -= 32;
    return row < 48 ? (int)row : -1;
}

uint8_t level_map_block(const Level *g, int map_x, int map_y) {
    int row = map_row(map_y);
    unsigned col = ((uint16_t)map_x & 0x0fff) >> 4;
    return row >= 0 ? g->map[row][col] : 0x60;
}

/* Player probes use OAM-style coordinates and subtract (8,16). */
static uint8_t block_at(const Level *g, int x, int y) {
    return level_map_block(g, x - 8, y - 16);
}

/* Probe order is observable: the first solid cell owns pipe/conveyor kind. */
static uint8_t floor_block(const Level *g, bool aligned, int *probe_x) {
    if (aligned && ((g->y - 4) & 15) >= 5) return 0x60;
    int x = g->x + 12;
    uint8_t block = block_at(g, x, g->y + 44);
    if (block >= 0x48) { x = g->x + 20; block = block_at(g, x, g->y + 44); }
    if (probe_x) *probe_x = x;
    return block;
}

static void collect_coin(Level *g, int x, int y);

static uint8_t wall_block(Level *g, bool right) {
    int x = g->x + (right ? 20 : 11);
    /* 00:1E7C/1E8D own coin collection while probing this side, from the
     * head downward. A solid block stops the remaining probes. */
    static const unsigned heights[]={20,32,40};
    unsigned first=g->power!=LEVEL_SMALL&&!g->crouching?0:1;
    uint8_t block=0x60;
    for(unsigned i=first;i<3;++i) {
        int y=g->y+(int)heights[i];
        block=block_at(g,x,y);
        if(block<0x38) return block;
        collect_coin(g,x,y);
    }
    return block;
}

static bool wall_contact(Level *g, bool right) {
    return wall_block(g, right) < 0x38;
}

bool level_head_blocked(const Level *g) {
    uint8_t block = block_at(g, g->x + 16, g->y + (g->power == LEVEL_SMALL ? 28 : 18));
    return block < 0x38 || block == 0x7e;
}

static void event(Level *g, LevelEventKind kind, int x, int y, unsigned value) {
    if (g->event_count < sizeof(g->events) / sizeof(g->events[0]))
        g->events[g->event_count++] = (LevelEvent){kind, x, y, value};
}

static void add_coin(Level *g) {
    if (g->coins < 999) ++g->coins;
    g->sound_request = 7;
    event(g, LEVEL_EVENT_COIN, g->x, g->y, 1);
}

/* 01:5960 allocates one of eight original block/effect slots. Coordinates
 * keep its OAM-style origin so 01:5CF1 writes the same map cell. */
static LevelEffect *start_effect(Level *g, unsigned kind, int x, int y) {
    for (unsigned i = 0; i < 8; ++i) if (!g->effects[i].kind) {
        LevelEffect *effect = &g->effects[i];
        *effect = (LevelEffect){(uint8_t)kind, 1, (x + 8) & ~15, (y & ~15) + 8};
        return effect;
    }
    return NULL;
}

/* 00:341C breaks ID06 using the projectile's existing collision probe. */
bool level_fire_break_block(Level *g, int x, int y) {
    if (block_at(g, x, y) != 6) return false;
    LevelEffect *effect = start_effect(g, 1, x, y);
    if (effect) event(g, LEVEL_EVENT_BLOCK_BROKEN, effect->x - 8, effect->y - 16, 6);
    g->sound_request = 2;
    return true;
}

/* 00:1993 selects effects using the original block IDs. The synchronous
 * actor query is 00:2F72: hidden contents suppress the default coin. */
static void hit_block(Level *g, int x, int y, uint8_t block) {
    if (block >= 4 && block != 0x7e) return;
    unsigned kind = 3;
    if (block == 0) kind = g->power == LEVEL_SMALL ? 2 : 1;
    else if (block == 3) kind = g->power == LEVEL_SMALL ? 6 : 5;
    LevelEffect *effect = start_effect(g, kind, x, y);
    if (!effect) return;
    int center_x = effect->x - 8, center_y = effect->y - 16;
    LevelEvent hit = {LEVEL_EVENT_BLOCK_HIT, center_x, center_y, block};
    event(g, hit.kind, hit.x, hit.y, hit.value);
    bool contents = g->block_hit && g->block_hit(g->block_context, &hit);
    g->sound_request = 2;
    if (kind == 1 || kind == 5)
        event(g, LEVEL_EVENT_BLOCK_BROKEN, center_x, center_y, block);
    if (block == 1 && !contents) {
        add_coin(g);
        /* 01:591F uses a point above the player rather than the hit probe. */
        (void)start_effect(g, 4, g->x + 16, g->y + (g->power == LEVEL_SMALL ? 18 : 8));
    }
}

/* 01:5997: update once after player/camera movement. Bumps temporarily
 * replace the block by ID18; at age8 they restore/consume it. */
static void update_effects(Level *g) {
    for (unsigned i = 0; i < 8; ++i) {
        LevelEffect *e = &g->effects[i];
        if (!e->kind) continue;
        ++e->age;
        int col = ((uint16_t)(e->x - 8) & 0x0fff) >> 4, row = map_row(e->y - 16);
        uint8_t *block = row >= 0 && row < 48 && col >= 0 && col < 256 ? &g->map[row][col] : NULL;
        if (e->kind == 4) {
            if (e->age == 50) e->kind = 0;
        } else if (e->kind == 1 || e->kind == 5) {
            if (e->age == 2 && block) *block = e->kind == 1 ? 0x60 : 0x7f;
            if (e->age == 40) e->kind = 0;
        } else {
            if (e->age == 2 && block) *block = 0x18;
            if (e->age == 8) {
                if (block) *block = e->kind == 3 ? 7 : e->kind == 6 ? 3 : 0;
                e->kind = 0;
            }
        }
    }
}

/* 00:3451 receives the original collectible kind through A26C. */
void level_collect(Level *g, LevelPickup pickup) {
    switch (pickup) {
        case LEVEL_PICKUP_MUSHROOM: g->power = LEVEL_BIG; break;
        case LEVEL_PICKUP_FLOWER: g->power = LEVEL_FIRE; break;
        case LEVEL_PICKUP_CARROT: g->power = LEVEL_BUNNY; break;
        case LEVEL_PICKUP_STAR:
            if (g->star_frames == 0xff) g->star_chain = 0;
            g->star_frames = 0x440; g->music_request = 4; return;
        case LEVEL_PICKUP_HEART:
            if (g->lives < 99) ++g->lives;
            g->sound_request = 9;
            return;
        case LEVEL_PICKUP_GOAL:
            g->goal_walk = true; /* A24F; completion is a later state transition */
            g->volume_duck = false;
            g->sound_request = 12;
            if (g->jump_index < 24) g->jump_index = 24;
            event(g, LEVEL_EVENT_GOAL, g->x, g->y, 0);
            return;
        case LEVEL_PICKUP_CHECKPOINT: g->checkpoint = true; g->sound_request = 21; return;
        case LEVEL_PICKUP_BUBBLE: g->bubble = true; g->sound_secondary = 20; return;
        case LEVEL_PICKUP_MONEYBAG:
            g->coins = g->coins > 949 ? 999 : g->coins + 50;
            g->sound_request = 7;
            return;
        default: return;
    }
    g->invulnerability = 0x40;
    g->sound_secondary = 20;
    event(g, LEVEL_EVENT_POWER_CHANGED, g->x, g->y, g->power);
}

/* 00:319F resolves damage already authorized by a producer. */
static void resolve_damage(Level *g, bool fatal) {
    if (g->life_state != LEVEL_ALIVE) return;
    if (!fatal) {
        if (g->power != LEVEL_SMALL) {
            g->power = g->power == LEVEL_BIG ? LEVEL_SMALL : LEVEL_BIG;
            g->invulnerability = 128;
            event(g, LEVEL_EVENT_POWER_CHANGED, g->x, g->y, g->power);
            return;
        }
    }
    g->life_state = LEVEL_DYING;
    g->death_started_this_frame = true;
    g->animation = 0;
    g->jump_index = 0x5f;
    event(g, LEVEL_EVENT_DEATH, g->x, g->y, 0);
}

/* 00:2E6F: callers without a prior collision decision use these gates. The
 * high byte alone grants star protection; its expired sentinel is 00FF. */
void level_damage(Level *g, bool fatal) {
    if (!fatal) {
        if (g->star_frames >> 8) return;
        if (g->bubble) { g->bubble = false; g->invulnerability = 128; return; }
        if (g->invulnerability) return;
    }
    g->sound_request=fatal||g->power==LEVEL_SMALL?15:20;
    resolve_damage(g, fatal);
}

void level_bounce(Level *g, bool jump_held) {
    if (g->life_state != LEVEL_ALIVE) return;
    g->jumped = true;
    g->grounded = false;
    g->jump_index = jump_held ? 0x4b : 0x30; /* 00:2D7D */
}

/* 00:3CC0 / 3CF1: the actor producer has already applied its speed gates
 * and owns the exact camera counts. Several platforms can run in one pass. */
void level_apply_actor_movement(Level *g, int dx, int dy,
                                uint8_t camera_right, uint8_t camera_left) {
    g->x = (uint16_t)(g->x + dx);
    g->y = (uint16_t)(g->y + dy);
    g->pending_camera_right += camera_right;
    g->pending_camera_left += camera_left;
}

/* 01:54E9/551E are shared by normal Mario and visible death-arc sprites. */
static void finish_sprite_tail(Level *g) {
    if (g->shake_frames) --g->shake_frames;
    g->star_chain_visible = g->star_chain_ticks != 0;
    if (g->star_chain_ticks) --g->star_chain_ticks;
}

/* 01:5447 distinguishes a counter that reached zero this frame from one
 * that was already zero. The renderer consumes this fact without advancing
 * state itself. Called once by level_step, before actor contact results. */
void level_finish_frame(Level *g) {
    g->previous_screen_x = (uint8_t)(g->x - g->camera_x + 96);
    g->previous_screen_y = (uint8_t)(g->y - g->camera_y + 98);
    g->invulnerability_active = g->invulnerability != 0;
    if (g->invulnerability) --g->invulnerability;
    /* 01:5354 / 53CF decrement only while selecting an airborne swim pose. */
    g->swim_stroke_active = (g->movement_mode & 2) && !g->grounded && g->swim_stroke;
    if (g->swim_stroke_active) --g->swim_stroke;
    g->fire_pose_active = g->power == LEVEL_FIRE && g->fire_pose &&
        !(g->invulnerability_active && (g->invulnerability & 4));
    if (g->fire_pose_active) --g->fire_pose;
    finish_sprite_tail(g);
}

static void collect_coin(Level *g, int x, int y) {
    int col = ((uint16_t)(x - 8) & 0x0fff) >> 4, row = map_row(y - 16);
    if (row < 0 || row >= 48 || col < 0 || col >= 256) return;
    uint8_t *block = &g->map[row][col];
    /* 00:1D75 / 1DA5: the two coin variants restore different backgrounds. */
    if ((*block & 0xfe) == 0x48) {
        *block = *block == 0x48 ? 0x60 : 0x7f;
        add_coin(g);
    }
}

/* A287 combines the level's gravity flag with water and bubble state. Keeping
 * the bubble bit here prevents item/damage consumers from reinterpreting it. */
static uint8_t movement_flags(const Level *g) {
    return g->movement_mode | (g->bubble ? 4 : 0);
}

/* 00:0FF2 is before terrain sampling: entering/leaving water therefore uses
 * the preceding frame's mode for the swim/flight input response. */
static void mode_input(Level *g, uint8_t held, uint8_t pressed) {
    unsigned mode = movement_flags(g);
    if (g->goal_walk || !mode) return;
    if (mode & 1) {
        if (!g->grounded && (held & JUMP)) {
            if (pressed & JUMP) g->sound_request = 18;
            if (g->jump_index < 0x48) g->jump_index += 4;
        }
    } else if (mode & 2) {
        if (pressed & JUMP) {
            g->jump_index = 0x34;
            if (!g->swim_stroke) { g->swim_stroke = 8; g->sound_request = 3; }
        }
    } else if (mode & 8) {
        if (g->grounded) g->flight_fuel = 50;
        else if (!(held & JUMP)) g->flight_fuel = 0;
        else if (g->flight_fuel) {
            --g->flight_fuel;
            if (g->jump_index < 0x4a) g->jump_index += 2;
        }
    } else if (!g->grounded && (held & JUMP)) {
        if (pressed & JUMP) g->sound_request = 18;
        if (g->jump_index < 0x4a) g->jump_index += 2;
    }
}

/* 00:1090 pauses vertical movement for eight frames on each bunny flap. */
static void bunny_input(Level *g, uint8_t held, uint8_t pressed) {
    if (g->goal_walk || g->shake_frames || g->power != LEVEL_BUNNY || movement_flags(g) ||
        g->jump_index >= 0x48) return;
    bool flap = (pressed & JUMP) != 0;
    if (flap) g->bunny_hover = true;
    else if (g->bunny_hover && !g->grounded && (held & JUMP)) {
        if (++g->bunny_cadence >= 12) { g->bunny_cadence = 0; flap = true; }
    }
    if (flap) { g->bunny_float = 8; g->jump_index = 0x18; }
}

/* 00:307D pans only with the exact RUN+UP or RUN+DOWN combination.
 * The previous sprite pass owns the on-screen limit, including platforms. */
static void pan_camera(Level *g, uint8_t held) {
    if (g->boss_mode || !(g->grounded || g->platform_grounded)) return;
    unsigned room = ((unsigned)g->camera_y >> 8) * 16 + ((unsigned)g->camera_x >> 8);
    unsigned rules = room < sizeof(g->camera_rules) ? g->camera_rules[room] : 0;
    if (held == (RUN | DOWN) && g->previous_screen_y >= 0x20) {
        g->camera_dirty |= 0x80;
        if (!(rules & 8) || (g->camera_y & 255) < 0xc0) g->camera_y = (uint16_t)(g->camera_y + 1);
    } else if (held == (RUN | UP) && g->previous_screen_y < 0x70) {
        g->camera_dirty |= 0x40;
        if (!(rules & 4) || (g->camera_y & 255) >= 0x49) g->camera_y = (uint16_t)(g->camera_y - 1);
    }
}

static void step_player(Level *g, uint8_t held, uint8_t pressed, bool timer_expired) {
    pan_camera(g, held);
    if (g->pipe >= 0x10) {
        int dx = 0;
        if (g->pipe < 0x30) {
            g->y += g->pipe == 0x10 ? 2 : -2;
            if (!level_head_blocked(g) && floor_block(g, false, NULL) >= 0x48) g->pipe = 0;
        } else {
            dx = g->pipe == 0x30 ? 1 : -1;
            g->x += dx * 2; /* A235/A236 is one although movement is two. */
            if (dx > 0) g->pending_camera_right = 1;
            else g->pending_camera_left = 1;
            if (++g->animation >= 12) g->animation = 0;
            if (!wall_contact(g, true) && !wall_contact(g, false)) g->pipe = 0;
        }
        return;
    }
    g->pipe = 0;
    int old_y = g->y;
    mode_input(g, held, pressed);
    /* 00:1BFF samples the center before this frame's movement. Crossing a
     * goal tile, rather than touching the goal actor, starts state18. */
    uint8_t center_block = block_at(g, g->x + 16, g->y + 28);
    bool was_slow = g->slow_terrain;
    g->slow_terrain = false;
    bool hazard = (center_block & 0xfe) == 0x50 ||
        (g->power != LEVEL_SMALL && !g->crouching &&
         (block_at(g, g->x + 16, g->y + 22) & 0xfe) == 0x50);
    if (hazard) {
        if (!g->invulnerability && !(g->star_frames >> 8)) {
            g->pending_damage = true;
            g->sound_request=g->power==LEVEL_SMALL?15:20;
        }
        if (g->previous_screen_y >= 0x80) {timer_expired=true;g->sound_request=15;}
    } else if ((center_block & 0xfc) == 0x4c) {
        if (g->jump_index < 0x18) {
            if (!was_slow) g->jump_index = 0x18;
            else g->jumped = false;
        }
        g->slow_terrain = true;
        g->movement_mode &= (uint8_t)~2;
    } else if ((center_block & 0xfe) == 0x4a) {
        g->exit = center_block == 0x4a ? LEVEL_EXIT_NORMAL : LEVEL_EXIT_SECRET;
        g->exit_wait = 192;
        g->exit_started_this_frame = true;
        g->music_request = 3;
        g->sound_request = g->sound_secondary = g->sound_noise = 0;
        g->checkpoint = false;
        g->asset_animation_selector = 0;
    } else {
        bool was_water = (g->movement_mode & 2) != 0;
        g->movement_mode &= (uint8_t)~2;
        if (center_block >= 0x52 && center_block < 0x58) {
            g->movement_mode |= 2;
            g->bunny_hover = false;
            g->bubble = false;
        }
        if (was_water != ((g->movement_mode & 2) != 0)) {
            /* 00:3735 latches the position before this frame's movement. */
            g->water_effect_kind = 1;
            g->water_effect_age = 0;
            g->water_effect_x = g->x;
            g->water_effect_y = g->y;
            if (g->speed_index >= 0xc0) g->speed_index = 0xbf;
            if (g->speed_index < 0x48) g->speed_index = 0x48;
            if (!(g->movement_mode & 2)) {
                g->jump_index = 0x46;
                g->jumped = true;
                if (held & UP) g->preserve_jump = true;
            }
        }
    }

    /* 00:129F: ground check and one-pixel floor alignment. */
    if (g->jump_index < 25) {
        int floor_x = 0;
        uint8_t floor = floor_block(g, true, &floor_x);
        g->preserve_jump = false;
        if (g->platform_grounded) {
            g->grounded = true; /* 00:1378; platform owner already resolved Y */
        } else if (floor >= 0x48) {
            if (g->grounded && (uint8_t)(g->speed_index - 0x48) >= 0x78)
                g->jump_index = 0x1c;
            g->grounded = false;
        } else {
            if ((floor & 0xfe) == 4 && (g->frame & 1)) {
                g->conveyor = floor == 5 ? -1 : 1;
                g->x += g->conveyor;
                if (g->conveyor > 0 && g->speed_index >= 0x80) ++g->pending_camera_right;
                else if (g->conveyor < 0 && g->speed_index <= 0x80) ++g->pending_camera_left;
            }
            if (g->spinning && floor < 4) {
                hit_block(g, floor_x, g->y + 44, floor);
                g->jumped = true;
                g->jump_index = held & JUMP ? 0x4b : 0x30;
                if (held & JUMP) g->grounded = false;
            } else {
                g->grounded = true;
                if ((floor & 0xfe) == 8) g->pipe = 1;
                if ((g->y - 4) & 15) --g->y;
            }
        }
    }
    /* 00:1413 resolves overlap before jumping/vertical movement. Keep its
     * signed correction separate from the subsequent table-driven step. */
    int wall_push = 0;
    uint8_t wall = wall_block(g, false);
    if (wall < 0x38) wall_push = 1;
    else { wall = wall_block(g, true); if (wall < 0x38) wall_push = -1; }
    if (wall_push && (wall & 0xfc) == 0x0c) g->pipe = 3;
    bool blocked = wall_push != 0;
    /* 00:1464: an autoscrolling wall crush bypasses power protection. */
    if (wall_push < 0 && g->auto_scroll && g->previous_screen_x < 0x12) {
        timer_expired=true;g->sound_request=15;g->volume_duck=false;
    }
    g->x += wall_push;

    /* 00:126F enables the descending spin for big/fire forms. The next
     * ground pass consumes this state before deciding whether to land. */
    bool spin = (held & DOWN) && ((unsigned)g->power & 1) &&
                !g->grounded && !movement_flags(g);
    if (spin && !g->spinning) g->sound_request = 14;
    g->spinning = spin;
    /* 00:160A: short/long jump uses original trajectory indices. */
    if (g->grounded && !g->shake_frames) g->jumped = false;
    g->crouching = !g->goal_walk && g->grounded && (held & DOWN);
    if ((g->grounded || g->slow_terrain) && !g->goal_walk && !g->shake_frames &&
        ((g->power == LEVEL_BUNNY ? held : pressed) & JUMP)) {
        /* 00:1637 emits here, including landing and jumping in one pass. */
        unsigned mode=movement_flags(g)&15;
        g->sound_request=mode?(mode&2?3:18):g->slow_terrain?3:1;
        if(g->power!=LEVEL_BUNNY&&(held&DOWN))g->sound_request=0;
        g->grounded = false;
        g->jumped = true;
        g->bunny_hover = false;
        g->jump_index = 0x4b;
        if (g->power != LEVEL_BUNNY && g->speed_index >= 0x48 && g->speed_index < 0xc0 && !(held & UP))
            g->jump_index = g->slow_terrain ? 0x48 : 0x30;
    }
    bunny_input(g, held, pressed);

    /* 00:14FA: release truncates rising motion, tables encode gravity. */
    if (!(g->movement_mode & 2) && g->bunny_float) --g->bunny_float;
    else if (!g->grounded) {
        if (g->jump_index >= 0x21 && !g->preserve_jump && !(held & JUMP)) g->jump_index = 0x21;
        if (g->jump_index) {
            --g->jump_index;
            if (g->jump_index == 0x30)
                g->jump_index = movement_flags(g) ? 0x2f : 0x18;
        }
        unsigned trajectory = movement_flags(g) ? 0x1947 : g->slow_terrain ? 0x18fb : 0x18af;
        g->y += (int8_t)g->rom[trajectory + g->jump_index];
        if (g->jump_index >= 25) {
            int head_y = g->y + (g->power == LEVEL_SMALL ? 28 : 18);
            uint8_t ceiling = block_at(g, g->x + 16, head_y);
            if (ceiling < 0x38 || ceiling == 0x7e) {
                g->y = old_y;
                g->jump_index = 24;
                g->flight_fuel = 0;
                hit_block(g, g->x + 16, head_y, ceiling);
                g->pipe = (ceiling & 0xfe) == 0x0a ? 2 : 0;
            }
            if (g->y < 0) g->y = 0;
        }
    }

    /* 00:1701 then 16CC: input acceleration precedes ground friction. */
    if (g->goal_walk) {
        g->facing = 1;
        g->speed_index = g->grounded ? 0xb8 : 0x80;
    } else if (!g->shake_frames && !blocked && !g->crouching) {
        if (held & RIGHT) {
            g->facing = 1;
            g->speed_index = (uint8_t)(g->speed_index + 8);
            if (!g->grounded && !g->slow_terrain && (g->speed_index & 0xf0) == 0xc0)
                g->speed_index = 0xb8;
            else if ((g->grounded || g->slow_terrain) && (!(held & RUN) || movement_flags(g)) && g->speed_index >= 0xc0)
                g->speed_index = 0xbf;
            else if (g->speed_index >= 0xf4) g->speed_index = 0xf3;
        } else if (held & LEFT) {
            g->facing = -1;
            g->speed_index = (uint8_t)(g->speed_index - 8);
            if (!g->grounded && !g->slow_terrain && (g->speed_index & 0xf0) == 0x40)
                g->speed_index = 0x48;
            else if ((g->grounded || g->slow_terrain) && (!(held & RUN) || movement_flags(g)) && g->speed_index < 0x48)
                g->speed_index = 0x48;
            else if (g->speed_index < 8) g->speed_index = 8;
        }
    }
    if (g->grounded || g->slow_terrain || (g->movement_mode & 2)) {
        if ((g->speed_index & 0xfc) == 0x80) g->speed_index = 0x80;
        else if (g->speed_index < 0x80) g->speed_index += 4;
        else g->speed_index -= 4;
    }
    /* 00:181C: table performs original frame-phased fractional movement. */
    unsigned horizontal_table = !g->goal_walk && (g->slow_terrain ||
        ((g->movement_mode & 2) && g->grounded)) ? 0x1aff : 0x19ff;
    int dx = wall_push + (int8_t)g->rom[horizontal_table + g->speed_index + (g->frame & 7)];
    int before_horizontal_step = g->x;
    g->x += dx;
    uint8_t hit_wall = wall_block(g, dx >= 0);
    if (hit_wall < 0x38) {
        g->x = before_horizontal_step;
        dx = wall_push;
        g->speed_index = 0x80;
        if ((hit_wall & 0xfc) == 0x0c) g->pipe = 3;
    }
    /* 00:0FBF clamps the on-screen low-byte position, not map-world X. */
    unsigned screen_x = (uint8_t)(g->x - g->camera_x + 96);
    if (screen_x < 16) g->x += 16 - (int)screen_x;
    else if (screen_x >= 160) g->x -= (int)screen_x - 160;
    g->x = (uint16_t)g->x;
    g->animation = (uint8_t)(g->animation + ((!g->goal_walk && !movement_flags(g) && (held & RUN)) ? 2 : 1));
    if (g->animation >= 12) g->animation = 0;
    /* 00:14BF writes only the moving direction. These A235/A236 counters
     * survive stationary frames and boss camera freezes until00:0C76. */
    if (dx > 0) g->pending_camera_right = (uint8_t)dx;
    else if (dx < 0) g->pending_camera_left = (uint8_t)-dx;
    /* 00:1189 requires the narrow center alignment for vertical pipes. */
    if (g->pipe && g->pipe < 0x10) {
        uint8_t direction = 0;
        if (g->pipe <= 2) {
            bool aligned = (g->x & 16) && (unsigned)((g->x & 15) - 5) < 7;
            if (aligned && (held & (g->pipe == 1 ? DOWN : UP))) {
                direction = g->pipe == 1 ? 0x10 : 0x20;
                if (direction == 0x20) g->y -= 2;
            }
        } else {
            bool right_enabled = !g->auto_scroll || (g->camera_x & 255) >= 0xb0;
            if (right_enabled && (g->conveyor > 0 || ((held & RIGHT) && g->grounded && !g->room_enemy_count))) direction = 0x30;
            else if (g->conveyor < 0 || ((held & LEFT) && g->grounded)) direction = 0x40;
        }
        if (direction) {
            g->pipe = direction;
            g->crouching = false;
            g->speed_index = 0x80;
            if (direction != 0x30 || !g->wario_phase) g->sound_request = 5;
        }
    }
    /* 00:319F / 3451 consume the preceding actor-contact pass. Damage is
     * resolved first; simultaneous pickups still run the original handler. */
    if (timer_expired) resolve_damage(g, true);
    if (g->pending_damage) {
        g->pending_damage = false;
        resolve_damage(g, false);
    }
    if (g->pending_pickup) {
        LevelPickup pickup = (LevelPickup)g->pending_pickup;
        g->pending_pickup = 0;
        level_collect(g, pickup);
    }

    if (g->fireballs) g->fireballs(g->fireballs_context, g, pressed);
}

/* 00:0F0B chooses the script using the player's destination room, independent
 * of the camera's still-moving room. Script zero needs no asset changes. */
static void enter_room(Level *g, uint8_t direction) {
    g->room_transition = direction;
    unsigned room = ((unsigned)g->y >> 8) * 16 + (((unsigned)g->x >> 8) & 15);
    g->room_script = room < sizeof(g->room_scripts) ? g->room_scripts[room] : 0;
    g->room_script_wait = 0;
    const uint8_t *p = bank_bytes(g, 5, 0x4125 + g->room_script * 2, 2);
    g->room_script_address = p ? (uint16_t)word(p) : 0;
    g->room_script_started_this_frame = g->room_script != 0;
}

/* 16:41E2/6C2A remove one special room enemy, saturating at zero. */
void level_room_enemy_removed(Level *g) {
    if (g->room_enemy_count) --g->room_enemy_count;
}

/* 00:0DED moves the camera four pixels per update while the player moves
 * through the pipe. The input/player pass remains suspended throughout. */
static void move_room(Level *g) {
    bool complete = false;
    if (g->room_transition & 1) {
        g->camera_dirty = 0x10;
        g->camera_x = (g->camera_x + 4) & 0x0fff;
        ++g->x;
        complete = (g->camera_x & 255) == 0x50;
    } else if (g->room_transition & 2) {
        g->camera_dirty = 0x20;
        g->camera_x = (g->camera_x - 4) & 0x0fff;
        --g->x;
        complete = (g->camera_x & 255) == 0xb0;
    } else if (g->room_transition & 4) {
        g->camera_dirty = 0x40;
        g->camera_y = (g->camera_y - 4) & 0x0fff;
        --g->y;
        complete = (g->camera_y & 255) == 0xb8;
    } else if (g->room_transition & 8) {
        g->camera_dirty = 0x80;
        g->camera_y = (g->camera_y + 4) & 0x0fff;
        g->y += 1 + (g->frame & 1);
        complete = (g->camera_y & 255) == 0x48;
    }
    g->x = (uint16_t)g->x;
    g->y = (uint16_t)g->y;
    if (++g->animation >= 12) g->animation = 0;
    if (complete) {
        g->room_transition = 0;
        g->sound_request = 5;
        if (g->boss_mode & 15) {
            g->boss_mode = 0xf0;
            /* 00:0EB0 calls0348 before boss initialization. Restoring IE
             * after the LY145 wait services the queued0154 VBlank: its
             * A266/A267 countdown advances without another02FF update. */
            if (g->input_wait) --g->input_wait;
            g->boss_started_this_frame = true;
        }
        if (g->room_enemy_count & 0xf0) g->room_enemy_count &= 15;
        memcpy(g->palettes, g->descriptor + 15, 3);
    }
}

/* 00:25AF changes the map/graphics to the boss arena. Boss AI remains
 * owned by its separate actor handler, selected by boss_kind and boss_mode. */
static bool load_boss_arena(Level *g, unsigned kind) {
    if (kind > 6) return false;
    const uint8_t *p = bank_bytes(g, 0x13, 0x42f0 + 3 * 2, 2);
    if (!p || !decode_map(g, 0x13, word(p))) return false;
    if (!copy_tiles(g, 7, 0x7800, 0x8e80, 0x50)) return false;
    unsigned bank = g->rom[0x268b + kind * 2];
    unsigned source = word(g->rom + 0x2699 + kind * 4);
    if (!copy_tiles(g, bank, source, g->id == 24 ? 0x9000 : 0x9200,
                    g->id == 24 ? 0x800 : 0x600)) return false;
    bank = g->rom[0x268c + kind * 2];
    source = word(g->rom + 0x269b + kind * 4);
    if (!copy_tiles(g, bank, source, g->id == 24 ? 0x8900 : 0x8b00,
                    g->id == 24 ? 0x700 : 0x400)) return false;
    p = bank_bytes(g, 8, word(g->rom + 0x26b5 + kind * 2), sizeof(g->metatiles));
    if (!p) return false;
    memcpy(g->metatiles, p, sizeof(g->metatiles));
    g->boss_kind = (uint8_t)kind;
    memset(g->palettes, 0, sizeof(g->palettes));
    return true;
}

/* 00:20A4 data commands. Each2728 wait advances FF97 without advancing
 * gameplay/actors/A298. Graphics transfers copy64bytes per VBlank (26D7). */
static void step_room_script(Level *g) {
    if (g->room_transfer_remaining) {
        unsigned count = g->room_transfer_remaining < 64 ? g->room_transfer_remaining : 64;
        if (!copy_tiles(g, g->room_transfer_bank, g->room_transfer_source,
                        g->room_transfer_destination, count)) {
            g->room_script_unsupported = true;
            return;
        }
        g->room_transfer_source += count;
        g->room_transfer_destination += count;
        g->room_transfer_remaining -= count;
        return;
    }
    if (g->room_script_wait && --g->room_script_wait) return;
    const uint8_t *p = bank_bytes(g, 5, g->room_script_address, 8);
    if (!p) { g->room_script_unsupported = true; return; }
    if (*p == 0xff) {
        g->room_script = 0;
        g->background_transfer_pending = false;
        return;
    }
    unsigned command = *p & 0xf0;
    switch (command) {
        case 0x40: {
            unsigned x = (p[1] & 15) << 8, y = (p[1] >> 4) << 8;
            g->x = (g->x & 255) | (int)x;
            g->y = (g->y & 255) | (int)y;
            g->camera_x = (g->camera_x & 255) | (int)x;
            g->camera_y = (g->camera_y & 255) | (int)y;
            g->room_script_wait = g->room_transition == 8 ? 5 : 4;
            g->camera_dirty &= g->room_transition & 3 ? 0xcf : 0x3f;
            g->room_script_address += 2;
            break;
        }
        case 0x80:
            g->room_enemy_count = 0xf0 | (*p & 15);
            ++g->room_script_address;
            break;
        case 0xb0: {
            unsigned kind = *p & 15;
            g->room_transfer_bank = p[1];
            g->room_transfer_source = (uint16_t)word(p + 2);
            g->room_transfer_destination = kind == 1 ? 0x9200 : 0x8b00;
            g->room_transfer_remaining = kind == 1 ? 0x600 : kind == 3 ? 0x400 : 0x380;
            g->room_script_address += 4;
            break;
        }
        case 0x60:
            g->boss_kind = *p & 15;
            /* 0348 synchronizes toLY145 before disablingLCD. The fixed
             * arena decoder/copy work takes13 display intervals,14 for the
             * castle's additional0x500 graphics bytes; original-ROM timing
             * is verified for every arena. FF97/A298 stop during this load. */
            g->scene_loading_frames = g->id == 24 ? 14 : 13;
            ++g->room_script_address;
            break;
        case 0x20:
            g->boss_mode = 15;
            g->input_wait = 0x160;
            g->asset_animation_selector = 0;
            g->star_frames = 0;
            ++g->room_script_address;
            break;
        case 0x30:
            if (*p & 15) {
                g->camera_y = (uint16_t)(g->camera_y + p[1]);
                g->y = (uint16_t)(g->y + p[2]);
            } else {
                g->camera_y = (g->camera_y & ~255) | p[1];
                g->y = (g->y & ~255) | p[2];
            }
            g->room_script_address += 3;
            break;
        case 0x10:
            g->descriptor[15] = p[1];
            g->room_script_address += 2;
            break;
        case 0x50:
            memset(g->palettes, 255, sizeof(g->palettes));
            ++g->room_script_address;
            break;
        default:
            g->room_script_unsupported = true;
            break;
    }
}

/* 00:0B2A keeps camera gates separate from player motion. Bosses own
 * camera movement once active; special room enemies hold it until cleared. */
static void step_camera(Level *g) {
    if (g->boss_mode & 0xf0) return;
    if (g->room_enemy_count && !(g->room_enemy_count & 0xf0)) return;
    if (g->room_transition) { move_room(g); return; }
    /* 00:0B2A uses direction latches set by the previous sprite pass.
     * They remain set until movement in the other direction clears them. */
    unsigned room = ((unsigned)g->camera_y >> 8) * 16 + ((unsigned)g->camera_x >> 8);
    unsigned rules = room < sizeof(g->camera_rules) ? g->camera_rules[room] : 0;
    if (g->auto_scroll) {
        /* 00:0DAF: one pixel every other frame, until a right room seam. */
        if ((rules & 1) && (g->camera_x & 255) >= 0xb0) {
            if (g->previous_screen_x >= 0xa1) enter_room(g, 1);
        } else if (!(g->frame & 1)) {
            g->scroll_left = false;
            g->camera_x = (g->camera_x + 1) & 0x0fff;
            g->camera_dirty |= 0x10;
        }
    } else {
        if ((rules & 1) && (g->camera_x & 255) == 0xb0 && g->previous_screen_x >= 0xa1)
            enter_room(g, 1);
        bool right_bound = (rules & 1) && ((g->camera_x & 255) >= 0xb0);
        if (right_bound && (g->camera_x & 255) > 0xb0) --g->camera_x;
        unsigned right_scroll = g->pending_camera_right;
        unsigned left_scroll = g->pending_camera_left;
        if (g->scroll_right && right_scroll && !right_bound) {
            g->camera_dirty |= 0x10;
            g->scroll_left = false;
            g->camera_x += (int)right_scroll;
            if ((uint8_t)(g->x - g->camera_x + 96) >= 64) ++g->camera_x;
            else if ((uint8_t)(g->x - g->camera_x + 96) < 63) --g->camera_x;
        }
        /* Both boundary passes run, even when travelling in the other direction.
         * At a 256-pixel seam the second pass still uses the old room flags. */
        if ((rules & 2) && (g->camera_x & 255) == 0x50 && g->previous_screen_x < 0x0f) {
            g->x = (g->x + 256) & 0x0f00;
            enter_room(g, 2);
        }
        bool left_bound = (rules & 2) && ((g->camera_x & 255) <= 0x50);
        if (left_bound && (g->camera_x & 255) < 0x50) ++g->camera_x;
        if (g->scroll_left && left_scroll && !left_bound) {
            g->camera_dirty |= 0x20;
            g->scroll_right = false;
            g->camera_x -= (int)left_scroll;
            if ((uint8_t)(g->x - g->camera_x + 96) < 112) --g->camera_x;
            else if ((uint8_t)(g->x - g->camera_x + 96) >= 113) ++g->camera_x;
        }
        g->camera_x &= 0x0fff; /* 00:0BA9 / 0C3A retains a 12-bit map X. */
        g->pending_camera_right = g->pending_camera_left = 0;
    }
    int camera_dy = (int8_t)(g->y - g->camera_y_origin);
    int relative_y = (uint8_t)(g->y - g->camera_y + 96);
    if (camera_dy > 0) {
        g->camera_dirty |= 0x80;
        if (rules & 8) {
            if ((g->camera_y & 255) == 0xc0 && g->previous_screen_y >= 0x90) {
                enter_room(g, 8);
                g->camera_dirty |= 0x40; /* 00:0CBD enters the upward flag pass. */
            }
            if ((g->camera_y & 255) > 0xc0) --g->camera_y;
            else if ((g->camera_y & 255) < 0xc0 && relative_y >= 0x58)
                g->camera_y += camera_dy;
        } else if (relative_y >= 0x50) g->camera_y += camera_dy;
    } else if (camera_dy < 0) {
        g->camera_dirty |= 0x40;
        if (rules & 4) {
            if ((g->camera_y & 255) == 0x48 && g->previous_screen_y < 0x1b) {
                g->y = g->camera_y & ~255;
                enter_room(g, 4);
            }
            if ((g->camera_y & 255) < 0x48) ++g->camera_y;
            else if ((g->camera_y & 255) > 0x48 && relative_y < (g->camera_rise_early ? 0x40 : 0x38))
                g->camera_y += camera_dy;
        } else if (relative_y < (g->camera_rise_early ? 0x40 : 0x28)) g->camera_y += camera_dy;
    }
    g->camera_y_origin = g->y;
    if ((uint8_t)(g->x - g->camera_x + 96) >= 96) g->scroll_right = true;
    else g->scroll_left = true;
}

/* 00:0879 queues one map strip after player movement and before camera
 * movement. VBlank prioritizes this transfer over animated tiles (018B).
 * The renderer reads the map directly, but its producer still owns the
 * transfer schedule because it determines the visible animation phase. */
static void queue_background_strip(Level *g) {
    static const uint8_t directions[4] = {0x40, 0x80, 0x20, 0x10};
    uint8_t direction = directions[g->frame & 3];
    if (g->camera_dirty & direction) {
        g->background_transfer_pending = true;
        g->camera_dirty &= direction & 0xc0 ? 0x3f : 0xcf;
    }
}

/* 01:5E12 drives Mario after Wario's final escape. It compares coordinate
 * low bytes, requests a jump at X=A0/A1, then hands off to scene23 at Y=E0. */
static void finale_input(Level *g, uint8_t *held, uint8_t *pressed) {
    if (g->wario_phase != 3) return;
    *pressed = 0;
    *held = RIGHT | RUN;
    unsigned x = (uint8_t)g->x & 0xfe;
    if (x < 0xa0) return;
    *held = RIGHT | JUMP;
    if (x == 0xa0) *pressed = JUMP;
    else if ((uint8_t)g->y >= 0xe0) g->ending_requested = true;
}

bool level_step_clock(Level *g) {
    /* 01:5D39 runs before the death/goal guards. Expiration may replace
     * music even while dying, but it preserves the100-second warning. */
    if(g->star_frames>>8) {
        --g->star_frames;
        if(!(g->star_frames>>8)&&g->current_music!=9)g->music_request=g->default_music;
    }
    if(g->life_state!=LEVEL_ALIVE||g->pending_damage||g->boss_defeated||g->goal_walk)return false;
    if(--g->timer_countdown)return false;
    g->timer_countdown=48;
    if(!g->time) {g->sound_request=15;g->volume_duck=false;return true;}
    --g->time;
    if(g->time<=10) {g->sound_secondary=14;g->volume_duck=true;}
    if(g->time<100) {
        if(!g->current_music) {
            if((g->default_music&0xfc)==0x20)g->music_request=g->default_music+4;
            else g->music_request=g->star_frames>>8?4:g->default_music;
        }
    } else if(g->time==100)g->music_request=9;
    return false;
}

/* 00:273E is the final state4 handler. The whole player/actor pass still
 * completes on entry; state8 resumes without running another player pass. */
static void enter_pause(Level *g, uint8_t pressed) {
    if (pressed == START && !g->room_transition) {
        g->paused = true;
        g->pause_request = 1;
    }
}

void level_tick_tiles(Level *g) {
    /* 00:018B prioritizes a queued background strip over 00:3873. The
     * counter itself advances in the main loop, not in this interrupt. */
    unsigned selector = g->asset_animation_selector;
    unsigned period = (selector >> 4) * 2;
    if (!g->background_transfer_pending && g->exit == LEVEL_EXIT_NONE && selector &&
        g->asset_animation_clock >= period) {
        g->asset_animation_clock = 0;
        g->asset_animation = (g->asset_animation + 1) & 3;
        unsigned address = 0x6680 + ((selector - 1) & 15) * 256 +
                           g->asset_animation * 64;
        (void)copy_tiles(g, 0x1d, address, 0x9200, 64);
    }
    g->background_transfer_pending = false;
}

void level_step(Level *g, uint8_t held, uint8_t pressed) {
    if (g->complete) return;
    g->boss_started_this_frame = false;
    if (g->scene_loading_frames) {
        if (--g->scene_loading_frames == 0 && !load_boss_arena(g, g->boss_kind))
            g->room_script_unsupported = true;
        return;
    }
    if (g->input_wait) --g->input_wait;
    g->room_script_started_this_frame = false;
    g->room_script_frame = g->room_script != 0;
    if (g->room_script_frame) {
        ++g->frame;
        g->sound_request = 0;
        g->event_count = 0;
        step_room_script(g);
        return;
    }
    /* 00:3873 runs in VBlank using the old A298 value; 00:02FF then
     * increments that byte even in pause, death and exit-wait states. */
    level_tick_tiles(g);
    ++g->asset_animation_clock;
    ++g->frame;
    g->event_count = 0;
    g->sound_request = 0;
    g->sound_secondary = g->sound_noise = 0;
    if (g->paused) {
        /* 00:2758 flashes both background and object palette0. Unlike
         * entry, resume accepts Start together with other new buttons. */
        g->palettes[0] = g->palettes[1] = g->frame & 16 ? 0x93 : 0xe7;
        g->effective_held = held;
        g->effective_pressed = pressed;
        if (pressed == 64 && g->stage_cleared) {
            /* 00:276D permits abandoning an already-cleared stage without
             * losing a life; A224=2 enters the ordinary failed return. */
            g->paused = false;
            g->pause_request = 2;
            g->return_requested = true;
            g->auto_scroll = false;
            g->asset_animation_selector = 0;
        } else if (pressed & START) {
            memcpy(g->palettes, g->descriptor + 15, 3);
            g->paused = false;
            g->pause_request = 2;
        }
        return;
    }
    g->death_started_this_frame = false;
    g->death_arc_frame = false;
    g->exit_started_this_frame = false;
    /* State18 (00:3547) waits on the VBlank-owned A267:A266 counter. */
    if (g->exit != LEVEL_EXIT_NONE) {
        if (g->exit_wait && --g->exit_wait == 0) g->complete = true;
        return;
    }
    /* 00:1FD2 compares against the previous effective FF80. A wait frame
     * clears FF80, so buttons still physically held become newly pressed
     * on the first released frame. Retain caller-supplied scripted presses. */
    pressed |= held & (uint8_t)~g->effective_held;
    if (g->input_wait) held = pressed = 0;
    finale_input(g, &held, &pressed);
    g->effective_held = held;
    g->effective_pressed = pressed;
    /* 01:5D39 runs before the state4 death branch; state9 no longer calls it. */
    bool timer_expired = (g->life_state == LEVEL_ALIVE || g->life_state == LEVEL_DYING) && level_step_clock(g);
    if (g->life_state != LEVEL_ALIVE) {
        /* 00:315C deliberately updates only FFC0, leaving the high byte. */
        if (g->life_state == LEVEL_DYING) {
            g->death_arc_frame = true;
            int low = (uint8_t)(g->y + (int8_t)g->rom[0x31ef + g->jump_index]);
            g->y = (g->y & ~255) | low;
            unsigned screen_y = (uint8_t)(g->y - g->camera_y + 98);
            g->previous_screen_y = (uint8_t)screen_y;
            g->star_chain_visible = false;
            if ((screen_y & 0xe0) != 0xe0) {
                g->previous_screen_x = (uint8_t)(g->x - g->camera_x + 96);
                finish_sprite_tail(g);
            }
            if ((screen_y & 0xf0) == 0xb0) {
                g->power = LEVEL_SMALL;
                g->invulnerability = 0;
                g->death_wait = 192;
                g->life_state = LEVEL_DEATH_WAIT;
                g->music_request = 2;
            } else if (g->jump_index > 1) --g->jump_index;
        } else if (g->life_state == LEVEL_DEATH_WAIT && --g->death_wait == 0) {
            /* 00:3127 clears these before requesting the world loader. */
            g->asset_animation_selector = g->wario_phase = 0;
            if (g->lives) --g->lives;
            else g->game_over = true;
            g->life_state = LEVEL_DEAD;
            event(g, LEVEL_EVENT_DEATH_FINISHED, g->x, g->y, g->lives);
        }
        if (g->death_arc_frame) enter_pause(g, pressed);
        return;
    }
    ++g->ticks;
    g->conveyor = 0; /* 01:5D27 precedes the player pass. */
    if (!g->room_transition) step_player(g, held, pressed, timer_expired);
    queue_background_strip(g);

    step_camera(g);
    g->background_shake = g->shake_frames & 3; /* 00:2062 precedes Mario. */
    update_effects(g);
    level_finish_frame(g);
    enter_pause(g, pressed);
}

/* 00:3753 and01:6485 own one water splash/rising-bubble record. */
void level_step_water_effect(Level *g) {
    if ((g->movement_mode & 2) && !(g->frame & 127) && !g->water_effect_kind) {
        g->water_effect_kind = 2;
        g->water_effect_age = 0;
        g->water_effect_x = g->x;
        g->water_effect_y = g->y;
    }
    if (g->water_effect_kind == 1) {
        if (++g->water_effect_age >= 16) g->water_effect_kind = 0;
    } else if (g->water_effect_kind == 2) {
        if (!(g->frame & 1)) {
            g->water_effect_y = (uint16_t)(g->water_effect_y - 2);
            uint8_t block = block_at(g, g->water_effect_x + 16, g->water_effect_y + 24);
            if (block < 0x52 || block >= 0x58) g->water_effect_kind = 0;
        }
        if ((uint8_t)(g->water_effect_y - g->camera_y + 98) < 16) g->water_effect_kind = 0;
    }
}

void level_compose_water_effect(const Level *g, SpriteList *sprites) {
    if (!g->water_effect_kind) return;
    (void)sprites_mario(sprites, g->rom, g->rom_size,
                        g->water_effect_kind == 1 ? 0xc7 : 0xc8,
                        (uint8_t)(g->water_effect_y - g->camera_y + 98),
                        (uint8_t)(g->water_effect_x - g->camera_x + 96), false, false);
}

static uint8_t tile_pixel(const Level *g, unsigned address, int x, int y) {
    unsigned offset = address - 0x8000 + (unsigned)y * 2;
    unsigned bit = 7 - (unsigned)x;
    return (uint8_t)(((g->tiles[offset] >> bit) & 1) |
                     (((g->tiles[offset + 1] >> bit) & 1) << 1));
}

static uint8_t background_pixel(const Level *g, uint8_t tile, int x, int y) {
    unsigned address = (unsigned)(0x9000 + (int8_t)tile * 16);
    return tile_pixel(g, address, x, y);
}

void level_compose_sprites(const Level *g, SpriteList *list) {
    /* 01:530D frame selection. The simulation owns counters; rendering is pure. */
    unsigned frame = g->facing > 0 ? 0 : 4;
    if (g->pipe >= 0x10)
        frame = g->pipe < 0x30 ? 0x10 : (g->pipe == 0x30 ? 1 : 5) + ((g->animation & 12) >> 2);
    else if (g->spinning) frame = g->rom[0x5348 + ((g->frame & 12) >> 2)];
    else if ((g->movement_mode & 2) && !g->grounded)
        frame = (g->facing > 0 ? 0x12 : 0x14) + (g->swim_stroke_active ? 1 : 0);
    /* 01:5377/53F2 tests the latched crouch before airborne state. A jump
     * can clear grounded after the crouch producer ran in the same update. */
    else if (g->crouching) frame = g->facing > 0 ? 14 : 15;
    else if (!g->grounded && !g->platform_grounded) frame = g->facing > 0 ? (g->jumped ? 8 : 10) : (g->jumped ? 9 : 11);
    else if ((g->facing > 0 && g->speed_index < 0x80) ||
             (g->facing < 0 && g->speed_index > 0x80))
        frame = g->facing > 0 ? 13 : 12;
    else if (g->speed_index != 0x80) frame += 1 + ((g->animation & 12) >> 2);
    bool small_frame = g->invulnerability_active ? (g->invulnerability & 4) != 0 : g->power == LEVEL_SMALL;
    if (small_frame) frame += 0x20;
    else if (g->power == LEVEL_BUNNY) frame += 0x45 + ((g->bunny_float & 4) ? 0x1a : 0);
    else if (g->power == LEVEL_FIRE) frame += 0x70 + (g->fire_pose_active ? 0x20 : 0);
    uint8_t origin_y = (uint8_t)(g->y - g->camera_y + 98);
    uint8_t y = (uint8_t)(origin_y - (g->shake_frames & 3));
    uint8_t x = (uint8_t)(g->x - g->camera_x + 96);
    if (g->death_arc_frame) frame = 0x6b;
    if (g->life_state == LEVEL_ALIVE || g->death_started_this_frame ||
        (g->death_arc_frame && (origin_y & 0xe0) != 0xe0)) {
        (void)sprites_mario(list, g->rom, g->rom_size, frame, y, x,
                            (g->star_frames >> 8) && (g->star_frames & 4), g->pipe >= 0x10 && !g->death_arc_frame);
        if (g->bubble)
            (void)sprites_mario(list, g->rom, g->rom_size, 0xdc + ((g->frame & 16) >> 4), y, x, false, false);
    }

    /* 01:551E emits the star-combo number, then the 1UP metasprite after
     * the fifth contact. It follows Mario and precedes block effects. */
    if(g->star_chain_visible) {
        uint8_t count_y=(uint8_t)(y-32+(g->star_chain_ticks>>1));
        uint8_t count_x=(uint8_t)(x-4);
        if(g->star_chain<5) (void)sprites_add(list,count_y,count_x,(uint8_t)(0x80+g->star_chain),0);
        else (void)sprites_mario(list,g->rom,g->rom_size,0xbe,count_y,count_x,false,false);
    }
    /* 00:0640 skips block effects after the shared Mario sprite tail. */
    if (g->life_state != LEVEL_ALIVE && !g->death_started_this_frame) return;
    /* 01:5997: effects follow Mario, before actors, in effect-slot order. */
    for (unsigned i = 0; i < 8; ++i) {
        const LevelEffect *effect = &g->effects[i];
        unsigned age = effect->age;
        int ey = effect->y - g->camera_y + 72;
        int ex = effect->x - g->camera_x + 80;
        if ((effect->kind == 1 || effect->kind == 5) && age >= 3 && age < 40) {
            ey += (int8_t)g->rom[0x5a4a + age];
            (void)sprites_mario(list, g->rom, g->rom_size, 0x65, (uint8_t)ey, (uint8_t)(ex - age), false, false);
            (void)sprites_mario(list, g->rom, g->rom_size, 0x66, (uint8_t)ey, (uint8_t)(ex + age), false, false);
        } else if ((effect->kind == 2 || effect->kind == 3 || effect->kind == 6) && age >= 3 && age < 8) {
            ey += (int8_t)g->rom[0x5bb4 + age];
            (void)sprites_mario(list, g->rom, g->rom_size, 0x42, (uint8_t)ey, (uint8_t)ex, false, false);
        } else if (effect->kind == 4 && age >= 2 && age < 50) {
            ey += (int8_t)g->rom[0x5ae0 + (age >> 1)];
            frame = g->rom[0x5af9 + ((age & 12) >> 2)];
            (void)sprites_mario(list, g->rom, g->rom_size, frame, (uint8_t)ey, (uint8_t)ex, false, false);
        }
    }
}

void level_render_scene(const Level *g, const SpriteList *list, uint8_t defeated_bcd,
                        const uint8_t palettes[3], uint8_t fb[160 * 144]) {
    if(g->scene_loading_frames) {memset(fb,0,160*144);return;}
    uint8_t bg[160 * 144];
    /* 00:2062 + 0969: scroll origin and metatile ordering TL,TR,BL,BR. */
    for (int y = 0; y < 144; ++y) {
        for (int x = 0; x < 160; ++x) {
            int wx = x + g->camera_x - 80, wy = y + g->camera_y - 72 - g->background_shake;
            uint8_t block = level_map_block(g,wx,wy);
            unsigned part = ((unsigned)wx & 8) / 8 + ((unsigned)wy & 8) / 4;
            uint8_t tile = g->metatiles[block][part];
            bg[y * 160 + x] = background_pixel(g, tile, wx & 7, wy & 7);
        }
    }

    /* 00:07DB / 076B; window starts at y=136, x=0 (WY=136,WX=7).
     * It is part of the background and participates in object priority. */
    uint8_t hud[20];
    memcpy(hud, g->rom + 0x7ea, sizeof(hud));
    hud[2] = 0x80 + (g->lives / 10) % 10;
    hud[3] = 0x80 + g->lives % 10;
    hud[7] = 0x80 + (g->coins / 100) % 10;
    hud[8] = 0x80 + (g->coins / 10) % 10;
    hud[9] = 0x80 + g->coins % 10;
    hud[13] = 0x80 + (defeated_bcd >> 4);
    hud[14] = 0x80 + (defeated_bcd & 15);
    hud[17] = 0x80 + (g->time / 100) % 10;
    hud[18] = 0x80 + (g->time / 10) % 10;
    hud[19] = 0x80 + g->time % 10;
    for (int y = 136; y < 144; ++y) for (int x = 0; x < 160; ++x)
        bg[y * 160 + x] = background_pixel(g, hud[x / 8], x & 7, y & 7);
    for (unsigned i = 0; i < 160 * 144; ++i) fb[i] = (palettes[0] >> (bg[i] * 2)) & 3;
    sprites_render(list, g->tiles, bg, fb, palettes[1], palettes[2]);
}

void level_render(const Level *g, uint8_t fb[160 * 144]) {
    SpriteList list;
    sprites_clear(&list);
    level_compose_sprites(g, &list);
    level_render_scene(g, &list, 0, g->palettes, fb);
}
