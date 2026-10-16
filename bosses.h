#ifndef SML2_NATIVE_BOSSES_H
#define SML2_NATIVE_BOSSES_H

#include "actors.h"

/* Bosses share the source's32-byte record layout with ordinary actors, but
 * type zero marks an empty record and movement uses screen_x/screen_y. */
typedef struct Bosses {
    const uint8_t *rom;
    size_t rom_size;
    Actor slots[ACTOR_SLOT_COUNT];
    uint8_t tick, configuration, last_slot;
    uint8_t defeat_phase, saved_frame, saved_state, saved_capability;
    uint8_t health, aggressive, projectile_variant, field_c8, side;
    uint8_t defeat_frame, field_cb, hit_wait;
    uint8_t field_ce, field_cf, field_d0, field_d1;
    uint8_t spawn_type, spawn_direction;
    bool unsupported;
} Bosses;

typedef struct BossWorld {
    void *context;
    uint8_t (*divider_byte)(void *context);
    uint8_t player_x, player_y; /* A23C/A23B, the preceding player display origin. */
    uint8_t player_transition, player_power, timer_bcd; /* A254, timer low digits. */
    uint16_t input_wait; /* A266/A267 */
    bool easy_mode;
} BossWorld;

typedef enum BossEventKind {
    BOSS_EVENT_SOUND, BOSS_EVENT_INPUT_WAIT, BOSS_EVENT_DEFEATED,
    BOSS_EVENT_SCENE, BOSS_EVENT_SHAKE, BOSS_EVENT_PHASE, BOSS_EVENT_PLAYER_RISE,
    BOSS_EVENT_UNSUPPORTED, BOSS_EVENT_ERROR
} BossEventKind;
typedef struct BossEvent {
    BossEventKind kind;
    unsigned slot;
    uint16_t value;
    uint8_t channel; /* SOUND:0=A460,1=A470,2=A478,3=musicA468. */
    bool has_definition; /* PHASE optionally replaces the ordinary actor list. */
    ActorDefinition definition;
} BossEvent;
typedef struct BossEvents { BossEvent items[64]; unsigned count; bool overflow; } BossEvents;
typedef struct BossVisual { uint8_t frame, x, y, flags_xor; bool hidden; } BossVisual;

bool bosses_load(Bosses *bosses, const uint8_t *rom, size_t rom_size,
                 unsigned level_id, uint8_t wario_phase, uint8_t tick);
void bosses_step(Bosses *bosses, const BossWorld *world, BossEvents *events);
bool bosses_visual(const Bosses *bosses, unsigned slot, BossVisual *visual);
/* 02:6982/6C6A/6ED2: source shared AD/AF state after each Wario form.
 * Final phase3 resumes ordinary slots during the forced ending walk. */
bool bosses_release_actors(const Bosses *bosses, Actors *actors, const BossEvent *phase);

#endif
