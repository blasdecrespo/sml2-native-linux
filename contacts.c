#include "contacts.h"

bool contacts_fireball(Actor slots[ACTOR_SLOT_COUNT],uint8_t x,uint8_t y,uint8_t *sound_request) {
    bool hit=false;
    for(unsigned i=0;i<ACTOR_SLOT_COUNT;++i) {
        Actor *a=&slots[i];
        if(a->contact_result||!a->contact_capability||(a->contact_capability&0x80)) continue;
        uint8_t top=a->screen_y+a->bounds[0]-6;
        uint8_t height=a->screen_y+a->bounds[1]+6-top;
        uint8_t left=a->screen_x+a->bounds[2]-6;
        uint8_t width=a->screen_x+a->bounds[3]+6-left;
        if((uint8_t)(y-top)>=height||(uint8_t)(x-left)>=width) continue;
        if(a->contact_capability&4) a->contact_result|=2;
        else *sound_request=2;
        hit=true;
    }
    return hit;
}

static bool overlaps(const Actor *actor, const Level *level, uint8_t player_x,
                     uint8_t player_y, uint8_t *vertical) {
    uint8_t capability = actor->contact_capability;
    if (capability & 0x80) {
        if (!(capability & 0x7f)) return false;
    } else if (!(capability & 0x63)) return false;
    if (actor->contact_result & 0xf7) return false;
    /* 00:2EDD compares modulo-256 OAM origins and source bounding extents. */
    uint8_t top = actor->bounds[0] + actor->screen_y - 20;
    int player_height = level->power == LEVEL_SMALL ? 0 : 8;
    if (level->crouching) player_height -= 8;
    uint8_t height = actor->bounds[1] + actor->screen_y + player_height - top;
    uint8_t delta_y = player_y - top;
    if (delta_y >= height) return false;
    uint8_t left = actor->bounds[2] + actor->screen_x - 5;
    uint8_t width = actor->bounds[3] + actor->screen_x + 4 - left;
    uint8_t delta_x = player_x - left;
    if (delta_x >= width) return false;
    *vertical = delta_y;
    return true;
}

bool contacts_step(Actor slots[ACTOR_SLOT_COUNT], Level *level, uint8_t held) {
    bool bounced = false;
    level->platform_grounded = false;
    if (level->pending_damage || level->life_state != LEVEL_ALIVE || level->pipe >= 0x10) return false;
    /* A23B/C were latched before this routine, even if a contact moves Mario. */
    uint8_t player_y = level->y - level->camera_y + 98;
    uint8_t player_x = level->x - level->camera_x + 96;
    for (unsigned i = 0; i < ACTOR_SLOT_COUNT; ++i) {
        Actor *actor = &slots[i];
        uint8_t vertical;
        if (!overlaps(actor, level, player_x, player_y, &vertical)) continue;
        uint8_t capability = actor->contact_capability;
        level->grounded = false;
        actor->contact_result |= 0x10;
        if (capability & 0x80) {
            level->pending_pickup = capability & 0x7f;
            continue;
        }
        if (vertical < 12 && (level->bunny_float || level->jump_index < 24) && (capability & 0x23)) {
            if (capability & 0x20) {
                level->platform_grounded = true;
                actor->contact_result = 8;
                if (vertical >= 3) --level->y;
                continue;
            }
            /* 00:2E23 ends bunny hovering before the star/stomp branch.
             * Keeping it set advances the next flap's cadence too early. */
            level->bunny_hover = false;
            if (!(level->star_frames >> 8)) {
                actor->contact_result = 1;
                int previous_y = level->y;
                level->y = (uint16_t)(level->y - vertical - 6);
                bounced = true;
                if (level_head_blocked(level)) level->y = previous_y;
                /* 00:2E5C uses the same release-proof jump flag as water
                 * exits. The common epilogue below owns the final index. */
                if(level->boss_mode) level->preserve_jump=true;
                continue;
            }
        }
        if (level->star_frames >> 8) {
            if (capability & 0x20) continue;
            actor->contact_result = 4;
            level->star_chain_ticks = 32;
            level->star_chain = (level->star_chain + 1) % 100;
            if (level->star_chain >= 5) {
                level->sound_request = 9;
                if (level->lives < 99) ++level->lives;
            }
        } else if (capability & 0x40) {
            if (level->bubble) {
                level->bubble = false;
                level->invulnerability = 128;
            } else if (!level->invulnerability) {
                level->pending_damage = true;
                level->sound_request = level->power == LEVEL_SMALL ? 15 : 20;
            }
        }
    }
    if (bounced) {
        level->jumped = true;
        level->jump_index = held & 16 ? 75 : 48;
    }
    return bounced;
}
