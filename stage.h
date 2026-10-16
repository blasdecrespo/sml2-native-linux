#ifndef SML2_STAGE_H
#define SML2_STAGE_H

#include "actors.h"
#include "bosses.h"
#include "contacts.h"
#include "fireballs.h"
#include "level.h"

/* Owns the gameplay frame order, while each system retains its parsed data. */
typedef struct Stage {
    Level level;
    Actors actors;
    Bosses bosses;
    Fireballs fireballs;
    SpriteList sprites;
    uint8_t sound_requests[3]; /* Original A460, A470, A478 queues. */
    uint8_t boss_music, scene_request;
    bool bosses_ready;
    bool unsupported_types[256];
    bool error;
    uint8_t (*divider_reader)(void *context);
    void *divider_context;
} Stage;

bool stage_init(Stage *stage, const uint8_t *assets, size_t size, bool easy_mode);
bool stage_load(Stage *stage, unsigned id, bool easy_mode, bool checkpoint);
void stage_step(Stage *stage, uint8_t held, uint8_t pressed);
void stage_render(const Stage *stage, uint8_t framebuffer[160 * 144]);

#endif
