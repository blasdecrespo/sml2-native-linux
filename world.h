#ifndef SML2_WORLD_H
#define SML2_WORLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { WORLD_NODE_COUNT=0x80, WORLD_PROGRESS_COUNT=0x2a };
enum WorldReturn { WORLD_EXISTING, WORLD_CLEAR, WORLD_SECRET, WORLD_FAILED, WORLD_FAILED_CHECKPOINT };
enum WorldScene { WORLD_MAP, WORLD_CASTLE_GATE, WORLD_ZONE_ENTRY, WORLD_ZONE_EXIT, WORLD_BONUS, WORLD_GAME_OVER, WORLD_BELL_CLAW, WORLD_BELL_LADDER, WORLD_COIN_AWARD, WORLD_ENDING, WORLD_FADE };
enum WorldAudioClock { WORLD_AUDIO_MAIN, WORLD_AUDIO_SKIP, WORLD_AUDIO_BEFORE_REQUESTS, WORLD_AUDIO_WAIT_THEN_MAIN };
enum WorldFade { WORLD_FADE_STAGE, WORLD_FADE_BELL_RETURN, WORLD_FADE_REGION_ENTRY, WORLD_FADE_MAP_READY };

/* Paid bonus machine state, translated from 18:4A81. The reel is driven by
 * player timing, not host randomness. Prize identities are the ROM's 0..11. */
typedef struct WorldBonus {
    uint8_t action,timer,prize,reel_phase,reel_ticks,arrow_cycle;
    uint8_t lever_phase,lever_distance,jump_step,loss_row;
    uint8_t previous_power,awarded_power,flash;
    uint8_t player_x,player_y,player_sprite;
    bool player_front;
} WorldBonus;
typedef struct WorldBellItem { uint8_t state,x,y,delay,prize; } WorldBellItem;
typedef struct WorldBell {
    enum WorldReturn result;
    uint16_t sequence,sequence_start;
    uint8_t walk_phase,grabbed,item_cursor;
    uint8_t gates[2],ball_direction,ball_distance;
    WorldBellItem items[5];
} WorldBell;
typedef struct WorldDecoration {
    uint8_t scheduler,random_pose,animation_clock,flight_step,flight_command;
    uint8_t orbit_step,bob_timer,bob_phase;
    uint8_t tile_timer,tile_phase,cloud_scroll[3];
    uint8_t blink_timer,horizontal_x,horizontal_direction;
    uint8_t secret_reveal,secret_action,secret_timer,shake_x,shake_y;
    uint8_t space_coin_phase,lightning_phase,lightning_position;
    uint8_t satellite_clock,satellite_direction,satellite_timer;
} WorldDecoration;
typedef struct WorldCoinAward {
    uint16_t timer;
    uint8_t stage,x,animation,coins,door_step,pending_door;
} WorldCoinAward;
typedef struct WorldEnding {
    uint16_t timer,copy_source,copy_destination,copy_remaining,text_source;
    uint8_t phase,copy_bank,text_rows,text_finished,patch_castle;
    uint8_t sprite_clock,y,wave_bottom,scroll_y,render_scroll_y;
    uint8_t wave_first,wave_last,wave_hold;
    bool sprite_front,unsigned_lower_tiles,finished;
} WorldEnding;

/* Original identities remain distinct: node is A840, stage is A269,
 * progress[slot] is A848+slot, and region is A68B. The ROM's FF stage identity
 * opens the castle gate; its verified handoff requests stage24 (0x18). FE is no stage.
 * The host dispatches stage_requested through the original scene16 owner. After
 * resolving gameplay, world_return retains the node selected for that stage.
 */
typedef struct {
    uint8_t music_request,sound_requests[3];
    bool after_tick; /* 69EC audio precedes a resumed fade continuation. */
} WorldAudioEvent;

typedef uint8_t (*WorldClockRead)(void *context);

typedef struct World {
    const uint8_t *rom;
    size_t rom_size;
    /* Host oscillator's DIV byte. NULL supplies deterministic test entropy;
     * gameplay installs its monotonic 4194304 Hz oscillator after init. */
    uint8_t (*read_divider)(void *context);
    uint8_t (*read_timer)(void *context);
    void *divider_context;
    uint8_t record[0x50],save_record[0x50],progress[WORLD_PROGRESS_COUNT];
    uint8_t node,region,x,y,segment,segment_timer,animation,pose;
    uint16_t camera_x,camera_y,path;
    uint16_t object_camera_x,object_camera_y; /* 0F:6D4C latched A680..683. */
    uint8_t movement_cadence,frame_counter,global_frame;
    uint8_t random_state;
    unsigned entropy_samples;
    uint8_t water_counter,cloud_counter,coin_animation,fish_counter;
    uint8_t boat_counter,scene_action,scene_timer,scene_y,scene_sprite;
    uint8_t scene_direction,scene_destination,scene_segment,lost_coin_mask;
    enum WorldScene scene;
    enum WorldFade fade;
    uint8_t fade_region;
    WorldBonus bonus;
    WorldBell bell;
    WorldDecoration decoration;
    WorldCoinAward award;
    WorldEnding ending;
    bool moving,stage_requested,return_processed,asset_error,save_requested,title_requested,peeking,return_requested;
    enum WorldReturn return_result;
    uint8_t stage,stage_flags,music_request,sound_requests[3];
    WorldAudioEvent audio_events[2]; /* Maximum: region stop, then initializer song. */
    uint8_t audio_event_count;
    enum WorldAudioClock audio_clock; /* Main2089, blocked69EC, or ending2A96. */
    uint8_t tiles[0x1800],map[64*64],sprites[160];
    uint8_t objects[18][4],overlays[16][3];
    uint8_t background_palette,sprite_palette[2];
    unsigned sprite_count,map_stride,lives,coins;
    uint8_t powerup; /* transient A216; not part of the original save record */
    uint64_t frames;
} World;

bool world_init(World *world,const uint8_t *rom,size_t size,const uint8_t record[0x50]);
bool world_init_with_clock(World *world,const uint8_t *rom,size_t size,const uint8_t record[0x50],WorldClockRead divider,WorldClockRead timer,void *context);
/* 00:03FF and boss00:266D reuse A600..A7FF for the stage metatile table. */
void world_inherit_metatiles(World *world,const uint8_t metatiles[512]);
bool world_return(World *world,enum WorldReturn result);
/* A24F selects a bell game before 0F:6F2B's normal map return. A255 is the
 * hundreds digit of remaining seconds and selects the claw prize sequence. */
bool world_finish_stage(World *world,enum WorldReturn result,bool goal_bell,unsigned remaining_seconds);
/* 00:27AF owns game-over progress loss. Sprite palettes are inherited from
 * gameplay: the caller supplies its last palettes in sprite_palette first. */
bool world_game_over(World *world);
/* Global1B/0C45B6 prepares the gate after the selected FF passes through16. */
bool world_castle_prepare(World *world);
/* Global state 1D: the defeated zone boss's coin returns to the castle. */
/* Separate global1D assets, global1E coin initialization, and global1F ticks.
 * The shared A266/267 timer survives both initializers. */
bool world_boss_prepare(World *world,uint8_t stage);
bool world_boss_clear(World *world,uint8_t stage);
/* Global 23 fade, then bank 1A's castle restoration and credits. Preserve
 * gameplay art/palettes and seed ending.timer=A266/267, sprite_clock=A222,
 * global_frame=FF97 after this call. finished retains the final screen. */
bool world_ending(World *world);
void world_step(World *world,uint8_t held,uint8_t pressed);
void world_render(const World *world,uint8_t shades[160*144]);
void world_export_record(const World *world,uint8_t record[0x50]);
bool world_node_stage(const World *world,uint8_t node,uint8_t *stage);
bool world_node_progress_slot(const World *world,uint8_t node,uint8_t *slot);
unsigned world_golden_coins(const World *world);

#endif
