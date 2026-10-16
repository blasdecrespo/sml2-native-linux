#ifndef SML2_NATIVE_ACTORS_H
#define SML2_NATIVE_ACTORS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { ACTOR_SLOT_COUNT = 16, ACTOR_DEFINITION_CAPACITY = 84 };

typedef struct ActorDefinition {
    uint16_t x, y;
    uint8_t flags, type;
    uint32_t asset_offset;
} ActorDefinition;

/* Original AD00+32*n / temporary FFD0 layout. Fields whose interpretation
 * has not yet been established retain their explicit offset in the name. */
typedef struct Actor {
    uint16_t x, y, source_flags_address;
    uint8_t definition_flags, type, lifecycle;
    uint8_t screen_y, screen_x, frame;
    uint8_t base_attributes, direction_attributes, extra_attributes;
    uint8_t ai_state, timer, inactive_contact_capability, contact_capability;
    uint8_t bounds[4], contact_result;
    uint8_t field_18, field_19, animation, alternate_ai;
    uint8_t fields_1c_1f[4];
    bool hidden;
} Actor;

typedef struct ActorWorld {
    void *context;
    /* 00:1F2F: actual map-world pixels, WITHOUT player 00:1EFA offsets. */
    uint8_t (*block_at)(void *context, uint16_t x, uint16_t y);
    /* Source FF04 input for random-choice routines; called at each read. */
    uint8_t (*divider_byte)(void *context);
    uint16_t camera_x, camera_y, player_x, player_y;
    uint8_t scroll_x, scroll_y;
    uint8_t player_transition; /* A224; FF freezes ordinary actor AI. */
    bool player_goal_walk; /* A24F; goal exit disables actor contact. */
    bool player_spin; /* A268 */
    bool player_powered; /* A216 */
    bool player_crouching; /* A291; carried-shell vertical attachment. */
    uint8_t player_state; /* A221; high nibble removes pipe-associated actors. */
    bool checkpoint_collected; /* A2A0 */
    bool run_held;
    uint8_t player_facing; /* A22B signed direction:01 right,FF left. */
    uint8_t current_sound; /* A460; defeated actors preserve request09. */
    uint8_t player_speed_index; /* A200; platform camera carry eligibility. */
} ActorWorld;

typedef struct ActorVisual {
    unsigned slot;
    uint8_t lifecycle, frame, flags_xor, x, y;
    bool alternate_tileset, hidden;
} ActorVisual;

enum ActorEventKind { ACTOR_EVENT_SOUND, ACTOR_EVENT_DEFEATED,
    ACTOR_EVENT_UNSUPPORTED, ACTOR_EVENT_ERROR, ACTOR_EVENT_PLAYER_MOVE,
    ACTOR_EVENT_SPECIAL_REMOVED }; /* Consumer decrements A2E0 iff nonzero. */
typedef struct ActorEvent {
    enum ActorEventKind kind;
    unsigned slot;
    uint16_t x, y;
    uint8_t value;
    int16_t delta_x, delta_y; /* PLAYER_MOVE: platform motion while standing. */
    uint8_t camera_right, camera_left, camera_up, camera_down; /* A235..A238 */
    uint8_t sound_channel; /* SOUND:0=A460,1=A470,2=A478. */
} ActorEvent;
typedef struct ActorEvents {
    ActorEvent items[ACTOR_SLOT_COUNT * 4];
    unsigned count;
    bool overflow;
} ActorEvents;

typedef struct Actors {
    const uint8_t *assets;
    size_t asset_size;
    ActorDefinition definitions[ACTOR_DEFINITION_CAPACITY];
    unsigned definition_count;
    Actor slots[ACTOR_SLOT_COUNT];
    int spawn_cursor;
    uint16_t previous_camera_x;
    uint16_t camera_x, camera_y; /* Current actor-pass FFCA/FFC8 reward anchor. */
    uint8_t scroll_x, scroll_y;
    uint8_t alternating_tick, slow_tick, dormant_count, active_count;
    uint8_t defeated_count; /* A28D packed BCD; preserve explicitly across loads. */
    bool easy_mode, alternate_tileset, carrying; /* carrying = AF3A */
    unsigned level_id;
    /* A shorter spawn list can retain old records beyond its first sentinel.
     * Wario writes only AB00..AB0C; those later typed records survive. */
    uint8_t stored_definition_count, definition_terminator_bytes;
    uint64_t frames;
} Actors;

bool actors_load(Actors *actors, const uint8_t *assets, size_t asset_size,
                 unsigned level_id, bool easy_mode);
void actors_step(Actors *actors, const ActorWorld *world, ActorEvents *events);
bool actors_visual(const Actors *actors, unsigned slot, ActorVisual *visual);
/* Verification/save-state representation; the runtime uses named fields. */
void actors_encode_slot(const Actor *actor, uint8_t bytes[32]);
void actors_decode_slot(Actor *actor, const uint8_t bytes[32]);
void actors_encode_definitions(const Actors *actors, uint8_t bytes[512]);
/* Original block-center occupancy query; marks actor contact bit5 and returns
 * whether a hidden item suppressed the ordinary question-block coin. */
bool actors_hit_block(Actors *actors, uint16_t center_x, uint16_t center_y);

#endif
