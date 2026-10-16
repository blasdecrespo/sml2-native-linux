#ifndef SML2_LEVEL_H
#define SML2_LEVEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sprites.h"

typedef enum LevelPower { LEVEL_SMALL, LEVEL_BIG, LEVEL_BUNNY, LEVEL_FIRE } LevelPower;
typedef enum LevelPickup {
    LEVEL_PICKUP_MUSHROOM = 1, LEVEL_PICKUP_FLOWER = 2, LEVEL_PICKUP_CARROT = 3,
    LEVEL_PICKUP_STAR = 4, LEVEL_PICKUP_HEART = 5, LEVEL_PICKUP_GOAL = 6,
    LEVEL_PICKUP_CHECKPOINT = 7, LEVEL_PICKUP_BUBBLE = 8, LEVEL_PICKUP_MONEYBAG = 9
} LevelPickup;
typedef enum LevelEventKind {
    LEVEL_EVENT_BLOCK_HIT, LEVEL_EVENT_BLOCK_BROKEN, LEVEL_EVENT_COIN,
    LEVEL_EVENT_POWER_CHANGED, LEVEL_EVENT_DEATH, LEVEL_EVENT_DEATH_FINISHED,
    LEVEL_EVENT_GOAL
} LevelEventKind;
typedef struct LevelEvent {
    LevelEventKind kind;
    int x, y; /* map-world pixel coordinates; block events use tile centers */
    unsigned value;
} LevelEvent;
/* The original block routine queries hidden actors synchronously to decide
 * whether this hit awards a coin. Actors retain ownership of item spawning. */
struct Level;
typedef void (*LevelFireballs)(void *context, struct Level *level, uint8_t pressed);
typedef bool (*LevelBlockHit)(void *context, const LevelEvent *event);
typedef struct LevelEffect { uint8_t kind, age; int x, y; } LevelEffect;
typedef enum LevelLifeState { LEVEL_ALIVE, LEVEL_DYING, LEVEL_DEATH_WAIT, LEVEL_DEAD } LevelLifeState;
typedef enum LevelExit { LEVEL_EXIT_NONE, LEVEL_EXIT_NORMAL, LEVEL_EXIT_SECRET } LevelExit;

/* Coordinates retain the ROM's player/camera origins; see level.c. */
typedef struct Level {
    const uint8_t *rom;
    size_t rom_size;
    uint8_t map[48][256];
    uint8_t metatiles[128][4];
    uint8_t tiles[0x1800];
    uint8_t descriptor[20], camera_rules[48], room_scripts[48], palettes[3];
    unsigned id, frame, ticks, coins, lives, time, timer_countdown;
    int x, y, camera_x, camera_y;
    int camera_y_origin;
    uint8_t pending_camera_right, pending_camera_left, camera_dirty;
    bool auto_scroll, camera_rise_early;
    uint8_t shake_frames, background_shake;
    bool background_transfer_pending;
    uint8_t previous_screen_x, previous_screen_y;
    uint8_t speed_index, jump_index, animation;
    int facing;
    bool grounded, jumped, paused, complete;
    bool stage_cleared, return_requested; /* A27D bit7 and pause Select scene20. */
    bool scroll_right, scroll_left;
    unsigned asset_animation;
    uint8_t asset_animation_clock, asset_animation_selector;
    LevelPower power;
    LevelLifeState life_state;
    unsigned invulnerability, star_frames, death_wait;
    uint8_t star_chain, star_chain_ticks;
    bool star_chain_visible;
    bool invulnerability_active, death_started_this_frame, death_arc_frame;
    LevelExit exit;
    unsigned exit_wait;
    bool exit_started_this_frame;
    bool bubble, checkpoint, goal_walk, crouching, spinning, game_over;
    bool platform_grounded, pending_damage;
    uint8_t pending_pickup, bunny_float;
    uint8_t pipe, room_transition, room_script, room_script_wait;
    bool room_script_started_this_frame, room_script_frame, room_script_unsupported;
    uint16_t room_script_address, room_transfer_source, room_transfer_destination, room_transfer_remaining;
    uint8_t room_transfer_bank, room_enemy_count, boss_mode, boss_kind, scene_loading_frames;
    unsigned input_wait;
    uint8_t effective_held, effective_pressed;
    bool boss_started_this_frame, boss_defeated;
    uint8_t wario_phase;
    bool ending_requested;
    int8_t conveyor;
    /* Original movement flags A287 excluding bit2 (owned by bubble). */
    uint8_t movement_mode, bunny_cadence, swim_stroke, flight_fuel;
    bool slow_terrain, bunny_hover, swim_stroke_active, preserve_jump;
    uint8_t sound_request, fire_pose;
    uint8_t sound_secondary, sound_noise; /* A470/A478 requests. */
    uint8_t music_request, default_music, current_music; /* A468/A292/A469. */
    uint8_t pause_request; /* A45E:1 enters pause,2 resumes. */
    bool volume_duck; /* A53F, shared with the audio driver. */
    bool fire_pose_active;
    uint8_t water_effect_kind, water_effect_age;
    int water_effect_x, water_effect_y;
    LevelEffect effects[8];
    LevelEvent events[16];
    unsigned event_count;
    LevelBlockHit block_hit;
    void *block_context;
    LevelFireballs fireballs;
    void *fireballs_context;
} Level;

bool level_init(Level *level, const uint8_t *rom, size_t rom_size);
bool level_load(Level *level, unsigned id);
/* 01:5579 overlays only the eight initial player/camera coordinate bytes. */
bool level_load_checkpoint(Level *level, unsigned id, bool checkpoint);
void level_reset(Level *level);
void level_step(Level *level, uint8_t held, uint8_t pressed);
/* VBlank-owned background transfer/animated tile work, independent of the
 * main-loop animation counter. level_step supplies the usual preceding tick. */
void level_tick_tiles(Level *level);
/* 01:5D39..5E12: star expiration and decimal timer music. Returns timeout. */
bool level_step_clock(Level *level);
void level_render(const Level *level, uint8_t framebuffer[160 * 144]);
void level_compose_sprites(const Level *level, SpriteList *sprites);
void level_render_scene(const Level *level, const SpriteList *sprites, uint8_t defeated_bcd,
                        const uint8_t palettes[3], uint8_t framebuffer[160 * 144]);
void level_collect(Level *level, LevelPickup pickup);
void level_damage(Level *level, bool fatal);
void level_bounce(Level *level, bool jump_held);
void level_room_enemy_removed(Level *level);
bool level_fire_break_block(Level *level, int projectile_x, int projectile_y);
/* Actor00:3CC0/3CF1 owns these direction counts; camera consumes them on
 * its next update. Actor vertical movement is measured from camera_y_origin. */
void level_apply_actor_movement(Level *level, int dx, int dy,
                                uint8_t camera_right, uint8_t camera_left);
/* level_step invokes this at its original sprite-selection phase. */
void level_finish_frame(Level *level);
/* 00:3753/01:6485 run after player contacts, before actor composition. */
void level_step_water_effect(Level *level);
void level_compose_water_effect(const Level *level, SpriteList *sprites);
bool level_head_blocked(const Level *level);
/* Actual map-pixel coordinates (without the player's OAM-origin offsets).
 * Owns the original12-bitX and E000-FDFF echo-RAM row aliases. */
uint8_t level_map_block(const Level *level, int map_x, int map_y);

#endif
