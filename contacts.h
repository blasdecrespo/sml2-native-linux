#ifndef SML2_CONTACTS_H
#define SML2_CONTACTS_H

#include "actors.h"
#include "level.h"

/* 00:2D7D runs after Mario's sprite origin is selected, before actor AI.
 * Actor bounds/screen positions therefore come from the preceding actor step.
 * Damage and pickups are queued for the next frame's 00:319F/3451 handlers. */
bool contacts_step(Actor slots[ACTOR_SLOT_COUNT], Level *level, uint8_t held);
/* 00:2FED tests all actors using their preceding sprite pass's bounds. */
bool contacts_fireball(Actor slots[ACTOR_SLOT_COUNT],uint8_t screen_x,uint8_t screen_y,uint8_t *sound_request);

#endif
