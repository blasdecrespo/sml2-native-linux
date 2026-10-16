#ifndef SML2_FIREBALLS_H
#define SML2_FIREBALLS_H
#include "actors.h"
#include "level.h"

/* Original A880/A890 are two separate projectile slots, not normal actors. */
typedef struct Fireball {
    bool active;
    uint16_t x,y;
    int8_t direction;
    uint8_t bounce;
} Fireball;
typedef struct Fireballs {
    Fireball slots[2];
    SpriteList sprites;
    uint8_t sound_requests[3];
    bool stepped,error;
} Fireballs;

void fireballs_step(Fireballs *fireballs,Level *level,Actor targets[ACTOR_SLOT_COUNT],uint8_t pressed);
void fireballs_encode(const Fireballs *fireballs,uint8_t bytes[32]);
#endif
