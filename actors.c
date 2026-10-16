#include "actors.h"

#include <string.h>

static const uint8_t *rom_at(const Actors *a, unsigned bank, unsigned address,
                            size_t size)
{
    size_t offset;
    if (address < 0x4000 || address > 0x8000 || size > 0x8000 - address)
        return NULL;
    offset = bank * 0x4000u + address - 0x4000;
    if (offset > a->rom_size || size > a->rom_size - offset) return NULL;
    return a->rom + offset;
}

static unsigned little_word(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

void actors_decode_slot(Actor *s, const uint8_t b[32])
{
    memset(s, 0, sizeof *s);
    s->x = (uint16_t)((unsigned)b[0] << 8 | b[1]);
    s->definition_flags = b[2];
    s->y = (uint16_t)((unsigned)b[3] << 8 | b[4]);
    s->type = b[5];
    s->source_flags_address = (uint16_t)((unsigned)b[6] << 8 | b[7]);
    s->lifecycle = b[8]; s->screen_y = b[9]; s->screen_x = b[10];
    s->frame = b[11]; s->base_attributes = b[12];
    s->direction_attributes = b[13]; s->extra_attributes = b[14];
    s->ai_state = b[15]; s->timer = b[16]; s->inactive_contact_capability = b[17];
    s->contact_capability = b[18];
    memcpy(s->bounds, b + 19, 4);
    s->contact_result = b[23]; s->field_18 = b[24]; s->field_19 = b[25];
    s->animation = b[26]; s->alternate_ai = b[27];
    memcpy(s->fields_1c_1f, b + 28, 4);
}

void actors_encode_slot(const Actor *s, uint8_t b[32])
{
    b[0] = (uint8_t)(s->x >> 8); b[1] = (uint8_t)s->x;
    b[2] = s->definition_flags;
    b[3] = (uint8_t)(s->y >> 8); b[4] = (uint8_t)s->y;
    b[5] = s->type;
    b[6] = (uint8_t)(s->source_flags_address >> 8);
    b[7] = (uint8_t)s->source_flags_address;
    b[8] = s->lifecycle; b[9] = s->screen_y; b[10] = s->screen_x;
    b[11] = s->frame; b[12] = s->base_attributes;
    b[13] = s->direction_attributes; b[14] = s->extra_attributes;
    b[15] = s->ai_state; b[16] = s->timer; b[17] = s->inactive_contact_capability;
    b[18] = s->contact_capability;
    memcpy(b + 19, s->bounds, 4);
    b[23] = s->contact_result; b[24] = s->field_18; b[25] = s->field_19;
    b[26] = s->animation; b[27] = s->alternate_ai;
    memcpy(b + 28, s->fields_1c_1f, 4);
}

void actors_encode_definitions(const Actors *a, uint8_t b[512])
{
    unsigned i;
    memset(b, 0, 512); memset(b, 0xff, 6);
    for (i = 0; i < a->stored_definition_count; ++i) {
        const ActorDefinition *d = &a->definitions[i];
        unsigned p = 6 + 6 * i;
        b[p] = (uint8_t)(d->x >> 8); b[p + 1] = (uint8_t)d->x;
        b[p + 2] = d->flags;
        b[p + 3] = (uint8_t)(d->y >> 8); b[p + 4] = (uint8_t)d->y;
        b[p + 5] = d->type;
    }
    for(unsigned j=0;j<a->definition_terminator_bytes;++j)b[6+i*6+j]=0xff;
}

bool actors_load(Actors *a, const uint8_t *rom, size_t rom_size,
                 unsigned level_id, bool easy_mode)
{
    const uint8_t *p;
    unsigned source;
    if (!a || !rom || rom_size != 0x80000 || level_id >= 32) return false;
    memset(a, 0, sizeof *a);
    a->rom = rom; a->rom_size = rom_size; a->level_id = level_id;
    a->easy_mode = easy_mode;
    /* 03:6C4B switches object graphics/AI families for level IDs >= 0A. */
    a->alternate_tileset = level_id >= 10;
    p = rom_at(a, 3, 0x6037 + level_id * 2, 2);
    if (!p) return false;
    source = little_word(p);
    for (;;) {
        ActorDefinition *d;
        unsigned type;
        p = rom_at(a, 3, source, 1);
        if (!p) return false;
        if (*p == 0xff) break;
        p = rom_at(a, 3, source, 3);
        if (!p || a->definition_count == ACTOR_DEFINITION_CAPACITY) return false;
        d = &a->definitions[a->definition_count++];
        /* 03:6BD8. The source's bit7 is flags, never part of X. */
        d->x = (uint16_t)(((p[0] & 15) << 8) | ((p[1] & 31) << 3));
        d->y = (uint16_t)((((p[2] >> 5) & 3) << 8) | ((p[2] & 31) << 3));
        d->flags = p[2] >> 7;
        type = (p[0] >> 2) & 0x3c;
        if (type & 4) type = (type | 0x40) & ~4u;
        d->type = (uint8_t)(type + (p[1] >> 5));
        d->rom_offset = 3 * 0x4000u + source - 0x4000;
        source += 3;
    }
    a->stored_definition_count=(uint8_t)a->definition_count;
    a->definition_terminator_bytes=2;
    return true;
}

static void event(ActorEvents *events, enum ActorEventKind kind,
                  unsigned index, const Actor *s, uint8_t value)
{
    ActorEvent *e;
    if (!events) return;
    if (events->count == sizeof events->items / sizeof *events->items) {
        events->overflow = true;
        return;
    }
    e = &events->items[events->count++];
    e->kind = kind; e->slot = index; e->x = s->x; e->y = s->y; e->value = value;
    e->delta_x = e->delta_y = 0;
    e->camera_right = e->camera_left = e->camera_up = e->camera_down = 0;
    e->sound_channel = 0;
}

static void queued_sound(ActorEvents *events, unsigned index, const Actor *s,
                         uint8_t channel, uint8_t value)
{
    unsigned before = events ? events->count : 0;
    event(events, ACTOR_EVENT_SOUND, index, s, value);
    if (events && events->count > before) events->items[before].sound_channel = channel;
}

static void update_bounds(const Actors *a, Actor *s)
{
    const uint8_t *p = rom_at(a, 2,
        (a->alternate_tileset ? 0x4935 : 0x4737) + s->frame * 2u, 2);
    if (!p) return;
    p = rom_at(a, 2, little_word(p), 4);
    if (p) memcpy(s->bounds, p, 4);
}

/* 02:4240 resets behavior fields but retains position, definition link,
 * lifecycle, screen origin and base attributes. */
static void change_type(const Actors *a, Actor *s, uint8_t attributes)
{
    const uint8_t *p;
    if ((s->source_flags_address >> 8) >= 0xad) return;
    p = rom_at(a, 2, (a->alternate_tileset ? 0x4bc1 : 0x4b61) + s->type, 1);
    if (p) s->frame = *p;
    s->direction_attributes = attributes;
    s->extra_attributes = 0; s->ai_state = 0; s->timer = 0;
    s->inactive_contact_capability = 0; s->contact_capability = 0;
    memset(s->bounds, 0, 4);
    s->contact_result = 0; s->field_18 = 0; s->field_19 = 0;
    s->animation = 0; s->alternate_ai = 0;
    /* The17-byte clear ends atFFEE. FFEF is the retained behavior byte. */
    memset(s->fields_1c_1f, 0, 3);
}

static ActorDefinition *source_definition(Actors *a, const Actor *s)
{
    unsigned index;
    if (s->source_flags_address < 0xab08) return NULL;
    index = (s->source_flags_address - 0xab08) / 6;
    if (index >= a->definition_count || s->source_flags_address != 0xab08 + 6 * index)
        return NULL;
    return &a->definitions[index];
}

static void count_defeat(Actors *a, const Actor *s, unsigned index, ActorEvents *events)
{
    /* 16:731D increments packed BCD. At rollover7346 tries the first empty
     * slot before the defeated actor is cleared, preserving slot scheduling. */
    unsigned count = a->defeated_count + 1u;
    if ((a->defeated_count & 15) + 1 > 9) count += 6;
    if (count > 0x99) count += 0x60;
    a->defeated_count = (uint8_t)count;
    event(events, ACTOR_EVENT_DEFEATED, index, s, 1);
    if (a->defeated_count) return;
    for (unsigned i = 0; i < ACTOR_SLOT_COUNT; ++i) {
        Actor *reward = &a->slots[i];
        if (reward->lifecycle) continue;
        reward->x = a->camera_x; reward->y = a->camera_y;
        reward->type = 0x1a; reward->frame = 0x1c;
        reward->source_flags_address = (uint16_t)(0xad00 + index * 32);
        reward->lifecycle = 1; reward->direction_attributes = 0;
        ++a->dormant_count;
        break;
    }
    queued_sound(events, index, s, 0, 8);
}

static void remove_actor(Actors *a, Actor *s, uint8_t reason,
                         unsigned index, ActorEvents *events)
{
    /* 16:72D7. reason0 permits respawn; reason1 counts a defeated actor.
     * Clearing the slot is followed by 02:41CF's frame-zero bbox write. */
    ActorDefinition *d = source_definition(a, s);
    if (!reason && d) d->flags = s->definition_flags == 0x10 ? 0x10 : 0;
    if (s->lifecycle == 1) --a->dormant_count;
    else {
        --a->active_count;
        if (reason == 1) count_defeat(a, s, index, events);
    }
    memset(s, 0, sizeof *s);
    s->hidden = true;
    update_bounds(a, s);
}

static bool in_window(uint16_t coordinate, uint16_t center, unsigned radius)
{
    uint16_t low = center >= radius ? (uint16_t)(center - radius) : 0;
    uint16_t high = (uint16_t)(center + radius);
    /* 02:42E3 includes both lower and upper endpoints. */
    return coordinate >= low && coordinate <= high;
}

static uint8_t block(const ActorWorld *w, uint16_t x, uint16_t y)
{
    return w->block_at ? w->block_at(w->context, x, y) : 0x60;
}

static bool floor_contact(const ActorWorld *w, const Actor *s)
{
    /* 02:4358: only the first four pixel rows of a 16px block contact. */
    return !(s->y & 12) && block(w, s->x, s->y) < 0x48;
}

static bool ceiling_contact(const ActorWorld *w, const Actor *s)
{
    /* 02:43B0 through00:3B7C probes the actor's18-pixel upper anchor. */
    return block(w, s->x, (uint16_t)(s->y - 18)) < 0x38;
}

static bool wall_contact(const ActorWorld *w, const Actor *s, bool right)
{
    /* 02:443C / 44EA: two points, at y-11 and y-5, x+/-7. */
    uint16_t x = (uint16_t)(s->x + (right ? 7 : -7));
    return block(w, x, (uint16_t)(s->y - 11)) < 0x38 ||
           block(w, x, (uint16_t)(s->y - 5)) < 0x38;
}

static void fall(const Actors *a, Actor *s)
{
    /* 16:7451 / 7488: saturating table index, then 00:3D3D moves Y. */
    const uint8_t *p = rom_at(a, 0x16, 0x7488 + s->timer, 1);
    if (p) s->y = (uint16_t)(s->y + *p);
    if (s->timer != 0x15) ++s->timer;
}

static void face_player(const ActorWorld *w, Actor *s)
{
    /* 16:73C6 / 73E0: compare the player's X+8 anchor to the actor. */
    bool right = (uint16_t)(w->player_x + 8) > s->x;
    s->ai_state = right ? 2 : 1;
    s->direction_attributes = right ? 0x20 : 0;
}

static void animate_four(const Actors *a, Actor *s, uint8_t tick_mask)
{
    /* 16:757E/7585 share16:758A and its four-phase delta table. */
    const uint8_t *p;
    if (a->slow_tick & tick_mask) return;
    if (++s->animation == 4) s->animation = 0;
    p = rom_at(a, 0x16, 0x75a2 + s->animation, 1);
    if (p) s->frame = (uint8_t)(s->frame + *p);
}

static void walking_fall(const Actors *a, Actor *s, const ActorWorld *w, bool half_rate)
{
    /* 16:4215 shared body. States1/2 walk;3/4 are falling variants. */
    bool right;
    if (half_rate && (a->slow_tick & 1)) return;
    right = s->ai_state == 2 || s->ai_state == 4;
    if (wall_contact(w, s, right)) {
        s->ai_state = (uint8_t)(s->ai_state + (right ? -1 : 1));
        s->direction_attributes ^= 0x20;
        return;
    }
    s->x = (uint16_t)(s->x + (right ? 1 : -1));
    if (s->ai_state < 3) {
        if (!floor_contact(w, s)) s->ai_state += 2;
    } else {
        fall(a, s);
        if (floor_contact(w, s)) s->ai_state -= 2;
    }
}

static void walker9(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:4201. */
    if (!(a->slow_tick & 3)) s->frame ^= 1;
    if (!s->ai_state) {
        face_player(w, s); s->extra_attributes = 0x10;
        s->contact_capability = 0x4e;
    } else walking_fall(a, s, w, true);
}

static void walking_edges(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:42F5/42FB/431A share movement with several later actor families. */
    if (!(a->slow_tick & 1)) {
        bool right = s->ai_state == 2;
        s->x = (uint16_t)(s->x + (right ? 1 : -1));
        if (!wall_contact(w, s, right)) return;
    } else if (floor_contact(w, s)) return;
    s->ai_state ^= 3;
    s->direction_attributes ^= 0x20;
}

static void walker1(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:42D1. This walker reverses on an edge rather than entering a
     * walking-fall state. Initial placement can still fall onto its floor. */
    animate_four(a, s, 1);
    if (!s->ai_state) {
        s->contact_capability = 0x56;
        if (!floor_contact(w, s)) fall(a, s);
        else { s->timer = 0; face_player(w, s); }
        return;
    }
    walking_edges(a, s, w);
}

static void bouncing_motion(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:4642 shared body; horizontal and vertical phases alternate. */
    if (!(a->slow_tick & 1)) {
        bool right = !(s->ai_state & 1);
        s->x = (uint16_t)(s->x + (right ? 1 : -1));
        if (wall_contact(w, s, right)) {
            s->ai_state = right ? 1 : 2;
            s->direction_attributes ^= 0x20;
        }
        return;
    }
    if (s->field_18 != 1) {
        const uint8_t *p = rom_at(a, 0x16, 0x749e + s->timer, 1);
        uint8_t speed = p ? *p : 0;
        if (s->timer != 0x15) ++s->timer;
        if (s->timer != 0x15) {
            s->y = (uint16_t)(s->y - speed);
            if (!ceiling_contact(w, s)) return;
        }
        s->timer = 0; s->field_18 = 1;
    } else {
        fall(a, s);
        if (!floor_contact(w, s)) return;
        s->field_18 = 0;
        if ((uint8_t)(s->field_19 + 1) != 4) {
            ++s->field_19; s->timer = 0x0a;
            if (s->ai_state < 3) s->ai_state += 2;
        } else {
            s->timer = s->field_18 = s->field_19 = 0;
            face_player(w, s);
        }
    }
}

static void bounce8(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:462E. */
    if (!(a->slow_tick & 1)) s->frame ^= 1;
    if (!s->ai_state) {
        face_player(w, s); s->extra_attributes = 0x10; s->contact_capability = 0x56;
    } else bouncing_motion(a, s, w);
}

static void pipe12(Actors *a, Actor *s, const ActorWorld *w,
                    unsigned index, ActorEvents *events, bool inverted)
{
    /* 16:44E0: distance-triggered 32px emergence, retreat and24tick pause. */
    if (w->player_state & 0xf0) { remove_actor(a, s, 0, index, events); return; }
    if (!s->ai_state) {
        uint8_t distance = (uint8_t)(w->player_x + 8 - s->x);
        s->base_attributes = 0x80;
        if (distance & 0x80) distance = (uint8_t)(-distance);
        if (distance < 0x18) return;
        s->ai_state = 1; s->field_18 = s->field_19 = 0x20;
        s->contact_capability = 0x44;
    } else if (s->ai_state == 1 || s->ai_state == 2) {
        if (!(a->slow_tick & 3)) s->frame ^= 1;
        s->y = (uint16_t)(s->y + ((s->ai_state == 1) != inverted ? -1 : 1));
        if (--s->field_18) return;
        s->field_18 = s->field_19; ++s->ai_state;
        s->fields_1c_1f[0] = 0x18;
    } else if (s->ai_state == 3 && !--s->fields_1c_1f[0]) s->ai_state = 0;
}

static void hidden_walker(Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:4712, used by source types11 and15 (hex). */
    if (!s->ai_state) {
        s->contact_capability = 0x80; s->field_18 = 0;
        s->base_attributes = 0x80; s->hidden = true;
    } else if (s->ai_state == 1) {
        --s->y;
        if (++s->field_18 == 8) { s->ai_state = 3; s->base_attributes = 0; }
    } else {
        bool right = s->ai_state == 3 || s->ai_state == 5;
        if (wall_contact(w, s, right)) {
            s->ai_state = (uint8_t)(s->ai_state + (right ? -1 : 1));
            s->direction_attributes ^= 0x20;
            return;
        }
        s->x = (uint16_t)(s->x + (right ? 1 : -1));
        if (s->ai_state < 4) {
            if (!floor_contact(w, s)) s->ai_state += 2;
        } else {
            fall(a, s);
            if (floor_contact(w, s)) s->ai_state -= 2;
        }
    }
}

static void hidden_stationary(const Actors *a, Actor *s)
{
    /* 16:4755 (type14, flashing) / 4758 (type12). */
    if (s->type == 20 && !(a->slow_tick & 1)) s->extra_attributes ^= 0x10;
    if (s->ai_state == 2) return;
    if (s->ai_state == 1) {
        if (a->slow_tick & 1) return;
        --s->y;
        if (++s->field_18 == 8) { s->ai_state = 2; s->base_attributes = 0; }
        return;
    }
    s->contact_capability = 0x80;
    if (s->definition_flags == 0x10) {
        s->contact_capability = (uint8_t)(s->type + 0x70);
        s->ai_state = 2; s->y += 8;
    } else {
        s->base_attributes = 0x80; s->hidden = true; s->ai_state = 2;
    }
}

static void animate_collectible(const Actors *a, Actor *s, uint8_t tick_mask)
{
    /* 16:7545 / 7551. Frame deltas come from the ROM table, with a flip
     * on phase3 and its removal on wrap. */
    const uint8_t *p;
    if (a->slow_tick & tick_mask) return;
    ++s->animation;
    if (s->animation == 4) {
        s->animation = 0; s->direction_attributes &= (uint8_t)~0x20;
    }
    p = rom_at(a, 0x16, 0x757a + s->animation, 1);
    if (p) s->frame = (uint8_t)(s->frame + *p);
    if (s->animation == 3) s->direction_attributes |= 0x20;
}

static void checkpoint(Actors *a, Actor *s, const ActorWorld *w,
                        unsigned index, ActorEvents *events)
{
    /* 16:47BD(type16) / 47D7(type17). The player's contact producer owns
     * activation; this handler owns the original resulting animation. */
    if (s->ai_state == 1) { animate_collectible(a, s, 3); return; }
    if (s->type == 22) {
        s->contact_capability = 0x86;
    } else if (s->ai_state != 2) {
        if (w->checkpoint_collected) { remove_actor(a, s, 2, index, events); return; }
        s->contact_capability = 0x87; s->ai_state = 2;
        return;
    }
    if (s->contact_result & 0x10) { s->ai_state = 1; s->contact_capability = 0; }
}

static uint8_t vertical_speed(const Actors *a, Actor *s, unsigned table, uint8_t last)
{
    const uint8_t *p = rom_at(a, 0x16, table + s->timer, 1);
    if (s->timer != last) ++s->timer;
    return p ? *p : 0;
}

static void defeated(Actors *a, Actor *s, unsigned index, ActorEvents *events)
{
    /* 16:41B1. Display-origin removal deliberately uses last frame's Y. */
    if (s->ai_state == 1) {
        s->y -= (uint16_t)(2 * vertical_speed(a, s, 0x74ab, 8));
        if (s->timer == 8) { s->ai_state = 2; s->timer = 0; }
    } else if (s->ai_state == 2) {
        s->y += (uint16_t)(2 * vertical_speed(a, s, 0x7488, 0x15));
        if (s->screen_y >= 0xa0) {
            remove_actor(a, s, 2, index, events);
            event(events, ACTOR_EVENT_SPECIAL_REMOVED, index, s, 1);
        }
    } else {
        s->y -= 0x14; s->direction_attributes ^= 0x40;
        s->timer = s->contact_capability = 0; s->ai_state = 1;
    }
}

static void shell(Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:434F: dormant shell, then eight flashes before restoring type01. */
    if (s->ai_state == 1) {
        if (!--s->fields_1c_1f[0]) s->ai_state = 2;
        else if (!floor_contact(w, s)) fall(a, s);
    } else if (s->ai_state == 2) {
        if (a->slow_tick & 3) return;
        s->frame ^= 0x0b;
        if (++s->fields_1c_1f[0] == 8) {
            s->type = 1; change_type(a, s, s->direction_attributes); s->hidden = true;
        }
    } else {
        s->contact_capability = 6; s->ai_state = 1; s->fields_1c_1f[0] = 0xc0;
    }
}

static void flattened(Actors *a, Actor *s, unsigned index, ActorEvents *events)
{
    /* 16:42A9: the flattened type09 persists for32 scheduled updates. */
    if (s->ai_state == 1) {
        if (!--s->timer) remove_actor(a, s, 1, index, events);
    } else {
        s->extra_attributes = 0x10; s->ai_state = 1; s->timer = 0x20;
        s->contact_capability = 0;
        event(events, ACTOR_EVENT_SOUND, index, s, 3);
    }
}

static void collected(Actors *a, Actor *s, unsigned index, ActorEvents *events)
{
    /* 16:46F5. The frame retained by the contact handler identifies the item. */
    s->ai_state = 1;
    if (!(a->slow_tick & 1)) s->extra_attributes ^= 0x10;
    s->y -= 2; s->field_18 += 2;
    if (s->field_18 == 0x20) remove_actor(a, s, 2, index, events);
}

static void spawn_child(Actors *a, const Actor *parent, unsigned parent_index,
                         uint8_t type, uint8_t frame, uint8_t direction)
{
    /* 16:736D writes only the listed fields. Removed slots retain frame-zero
     * bounds until their first ordinary controller update. */
    for (unsigned i = 0; i < ACTOR_SLOT_COUNT; ++i) {
        Actor *s = &a->slots[i];
        if (s->lifecycle) continue;
        s->x = parent->x; s->y = parent->y;
        s->definition_flags = parent->definition_flags; s->type = type;
        s->source_flags_address = (uint16_t)(0xad00 + parent_index * 32);
        s->lifecycle = 1; s->frame = frame; s->direction_attributes = direction;
        ++a->dormant_count;
        return;
    }
}

static void firing_pipe(Actors *a, Actor *s, const ActorWorld *w,
                         unsigned index, ActorEvents *events)
{
    /* 16:4541. It aims the projectile vertically while emerging and retreating. */
    if (w->player_state & 0xf0) { remove_actor(a, s, 0, index, events); return; }
    if (!s->ai_state) {
        s->direction_attributes = (uint16_t)(w->player_x + 8) >= s->x ? 0x20 : 0;
        pipe12(a, s, w, index, events, false);
        return;
    }
    if (s->ai_state == 1 || s->ai_state == 3) {
        s->frame = (uint16_t)(w->player_y + 32) > s->y ? 0x14 : 0x12;
        if (s->ai_state == 1) {
            --s->y;
            if (--s->field_18) return;
            ++s->frame;
            spawn_child(a, s, index, 0x0e, 0x16,
                s->direction_attributes | (s->frame == 0x13 ? 0 : 0x40));
            s->ai_state = 2; s->fields_1c_1f[0] = 0x30;
        } else {
            ++s->y;
            if (--s->field_18) return;
            s->ai_state = s->fields_1c_1f[0] = 4;
        }
    } else if (s->ai_state == 2) {
        if (!--s->fields_1c_1f[0]) {
            s->ai_state = 3; s->field_18 = s->field_19; --s->frame;
        }
    } else if (s->ai_state == 4 && !(a->slow_tick & 7) && !--s->fields_1c_1f[0])
        s->ai_state = 0;
}

static void pipe_projectile(const Actors *a, Actor *s)
{
    /* 16:45ED: fixed diagonal movement; direction derives from spawn flags. */
    if (!(a->slow_tick & 1)) s->frame ^= 1;
    if (!s->ai_state) {
        s->contact_capability = 0x40; s->y -= 12;
        if (s->direction_attributes & 0x40) {
            s->direction_attributes &= (uint8_t)~0x40; s->field_18 = 1;
        }
        s->ai_state = (s->direction_attributes & 0x20) ? 2 : 1;
    } else {
        s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -2 : 2));
        s->y = (uint16_t)(s->y + (s->field_18 ? 1 : -1));
    }
}

static void paused_walker(const Actors *a, Actor *s, const ActorWorld *w, bool four_frames)
{
    /* 16:5CE7 alternates48 movement updates with16 pause updates. */
    if (four_frames) animate_four(a, s, 1);
    else if (!(a->slow_tick & 3)) s->frame ^= 1;
    if (!s->ai_state) {
        s->extra_attributes = 0x10; s->contact_capability = four_frames ? 0x4e : 0x4c;
        face_player(w, s); s->fields_1c_1f[0] = 1;
        return;
    }
    bool right = s->ai_state == 2;
    if (wall_contact(w, s, right)) {
        s->direction_attributes ^= 0x20; s->ai_state ^= 3;
    } else if (s->fields_1c_1f[0]) {
        if (!--s->fields_1c_1f[0]) s->field_19 = 0x30;
    } else {
        s->x = (uint16_t)(s->x + (right ? 1 : -1));
        if (!--s->field_19) s->fields_1c_1f[0] = 0x10;
    }
}

static uint8_t player_distance(const ActorWorld *w, const Actor *s)
{
    /* 16:74B4 intentionally measures the low byte, including its wrap. */
    uint8_t distance = (uint8_t)(w->player_x + 8 - s->x);
    return distance & 0x80 ? (uint8_t)-distance : distance;
}

static void face_player_vertical(const ActorWorld *w, Actor *s)
{
    /* 16:7407 compares the player anchor cached by02:4000. */
    s->ai_state = (uint16_t)(w->player_y + 32) > s->y ? 2 : 1;
}

static void animate_three(const Actors *a, Actor *s)
{
    /* 16:75BB: three sprite phases, using the original signed deltas. */
    const uint8_t *p;
    if (++s->animation == 3) s->animation = 0;
    p = rom_at(a, 0x16, 0x75d3 + s->animation, 1);
    if (p) s->frame = (uint8_t)(s->frame + *p);
}

static void carry_player(const Actor *s, const ActorWorld *w, unsigned index, ActorEvents *events,
                          int dx, int dy)
{
    /* 00:3CC0/3CDF/3CF1/3D10 carry the player only on standing contact. */
    if (s->contact_result & 8) {
        unsigned before = events ? events->count : 0;
        event(events, ACTOR_EVENT_PLAYER_MOVE, index, s, 0);
        if (events && events->count > before) {
            events->items[before].delta_x = (int16_t)dx;
            events->items[before].delta_y = (int16_t)dy;
            events->items[before].camera_right = dx > 0 && w->player_speed_index >= 0x80 ? (uint8_t)dx : 0;
            events->items[before].camera_left = dx < 0 && w->player_speed_index <= 0x80 ? (uint8_t)-dx : 0;
            events->items[before].camera_up = dy < 0 ? (uint8_t)-dy : 0;
            events->items[before].camera_down = dy > 0 ? (uint8_t)dy : 0;
        }
    }
}

static void platform_motion(Actor *s, const ActorWorld *w, unsigned index, ActorEvents *events,
                            int dx, int dy, bool carry)
{
    s->x = (uint16_t)(s->x + dx); s->y = (uint16_t)(s->y + dy);
    if (carry) carry_player(s, w, index, events, dx, dy);
}

static void oscillating_platform(const Actors *a, Actor *s, const ActorWorld *w,
                                 unsigned index, ActorEvents *events, unsigned routine)
{
    /* 16:4BBA/4BCB vertical;4C41/4C52/4C6A horizontal;4D34 diagonal.
     * The initial leg is half the later travel distance. */
    bool vertical = routine == 0x4bba || routine == 0x4bcb;
    bool diagonal = routine == 0x4d34;
    if (routine == 0x4c52 && !(a->slow_tick & 3)) s->frame ^= 1;
    if (!s->ai_state) {
        s->contact_capability = 0x20;
        if (vertical) face_player_vertical(w, s); else face_player(w, s);
        s->field_18 = routine == 0x4bba ? 0x2c : routine == 0x4bcb ? 0x14 :
                      routine == 0x4c41 ? 0x3c : diagonal ? 0x18 : 0x24;
        if (routine == 0x4c52) s->extra_attributes = 0x10;
        return;
    }
    if (diagonal && (a->slow_tick & 1)) return;
    bool negative = s->ai_state == 1 || s->ai_state == 3;
    int motion = negative ? -1 : 1;
    platform_motion(s, w, index, events, vertical ? 0 : motion,
                    vertical || diagonal ? motion : 0, true);
    uint8_t distance = (uint8_t)(s->field_18 * (s->ai_state <= 2 ? 1 : 2));
    if (++s->timer != distance) return;
    s->timer = 0;
    s->ai_state = s->ai_state <= 2 ? (s->ai_state == 1 ? 4 : 3) : s->ai_state ^ 7;
}

static void diagonal_platform(const Actors *a, Actor *s, const ActorWorld *w,
                               unsigned index, ActorEvents *events)
{
    /* 16:4CE0: down-left/up-right,24 initial steps then48 each leg. */
    if (!s->ai_state) {
        s->contact_capability = 0x20; face_player(w, s);
        s->field_18 = 0x18; s->field_19 = 0x30;
        return;
    }
    if (a->slow_tick & 1) return;
    int dx = s->ai_state == 1 ? -1 : 1;
    platform_motion(s, w, index, events, dx, -dx, true);
    if (!--s->field_18) { s->field_18 = s->field_19; s->ai_state ^= 3; }
}

static void falling_platform(Actors *a, Actor *s, const ActorWorld *w,
                             unsigned index, ActorEvents *events)
{
    /* 16:4DD1 starts a28-update delay after the first standing contact. */
    if (!s->ai_state) { s->contact_capability = 0x20; s->ai_state = 2; return; }
    if (s->ai_state == 2) {
        if (s->contact_result & 8) { s->ai_state = 1; s->fields_1c_1f[0] = 0x1c; }
        return;
    }
    if (s->fields_1c_1f[0] && --s->fields_1c_1f[0]) return;
    unsigned speed = vertical_speed(a, s, 0x7488, 0x15);
    platform_motion(s, w, index, events, 0, (int)speed, true);
    if (floor_contact(w, s)) remove_actor(a, s, 0, index, events);
}

static void rising_platform(Actor *s, const ActorWorld *w, unsigned index, ActorEvents *events)
{
    /* 16:4E12 rises while ridden and returns after16 idle updates. The
     * downward return intentionally does not carry the player. */
    if (!s->ai_state) { s->contact_capability = 0x20; s->ai_state = 3; return; }
    if (s->ai_state == 3) {
        if (s->contact_result & 8) {
            s->ai_state = 1; s->field_18 = s->field_19 = 0x40;
        }
    } else if (s->ai_state == 1) {
        if (!(s->contact_result & 8)) {
            s->ai_state = 2; s->fields_1c_1f[0] = 0x10;
        } else if (s->field_18) {
            --s->field_18; platform_motion(s, w, index, events, 0, -1, true);
        }
    } else if (s->contact_result & 8) {
        s->ai_state = 1; s->fields_1c_1f[0] = 0;
    } else {
        if (s->fields_1c_1f[0] && --s->fields_1c_1f[0]) return;
        if (s->field_18 != s->field_19) { ++s->field_18; ++s->y; }
    }
}

static void rectangular_platform(Actor *s, const ActorWorld *w,
                                 unsigned index, ActorEvents *events)
{
    /* 16:4E7E. The direction bit fixes which way the rectangle is traversed. */
    if (!s->ai_state) {
        s->y -= 0x20; face_player(w, s); s->contact_capability = 0x20;
        s->field_19 = 0x28; s->field_18 = s->timer = 0x50;
        return;
    }
    bool right_first = s->direction_attributes != 0;
    if (s->ai_state <= 2) {
        platform_motion(s, w, index, events, s->ai_state == 1 ? -1 : 1, 0, true);
        if (!--s->field_19) { s->field_19 = s->timer; s->ai_state = 3; }
    } else if (s->ai_state == 3 || s->ai_state == 5) {
        bool down = s->ai_state == 3;
        platform_motion(s, w, index, events, 0, down ? 1 : -1, true);
        if (!--s->field_18) {
            s->field_18 = s->timer;
            s->ai_state = (down != right_first) ? 4 : 6;
        }
    } else {
        bool right = s->ai_state == 4;
        platform_motion(s, w, index, events, right ? 1 : -1, 0, true);
        if (!--s->field_19) {
            s->field_19 = s->timer;
            s->ai_state = (right == right_first) ? 3 : 5;
        }
    }
}

static void vertical_patrol(const Actors *a, Actor *s)
{
    /* 16:5E06:48-pixel legs,16-update waits, movement at half AI rate. */
    if (!(a->slow_tick & 3)) s->frame ^= 1;
    if (!s->ai_state) {
        s->extra_attributes = 0x10; s->contact_capability = 0x4c;
        s->ai_state = 1; s->field_18 = 0x30; return;
    }
    if (a->slow_tick & 1) return;
    if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
    s->y = (uint16_t)(s->y + (s->ai_state == 1 ? -1 : 1));
    if (!--s->field_18) {
        s->field_18 = 0x30; s->fields_1c_1f[0] = 0x10; s->ai_state ^= 3;
    }
}

static void floating_enemy(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:51F9: alternating deceleration/acceleration along the vertical axis. */
    if (!(a->slow_tick & 1)) s->frame ^= 1;
    if (!s->ai_state) {
        s->contact_capability = 0x56; s->extra_attributes = 0x10;
        face_player_vertical(w, s); return;
    }
    if (a->slow_tick & 3) return;
    bool initial = s->ai_state <= 2;
    if (!initial && s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
    unsigned speed = vertical_speed(a, s, initial ? 0x74a4 : 0x7488, 0x0f);
    bool down = s->ai_state == 2 || s->ai_state == 3;
    s->y = (uint16_t)(s->y + (down ? (int)speed : -(int)speed));
    if (s->timer == 0x0f) {
        s->timer = 0;
        s->ai_state = initial ? s->ai_state + 2 : s->ai_state == 4 ? 1 : 2;
    }
}

static void emerging_bubble(Actors *a, Actor *s, const ActorWorld *w,
                             unsigned index, ActorEvents *events)
{
    /* 16:4804: nearby player reveals the bubble, then it drifts upward. */
    if (!s->ai_state) { s->ai_state = 1; return; }
    if (s->ai_state == 1) {
        if (player_distance(w, s) < 0x30) {
            s->ai_state = 2; s->contact_capability = 0x88;
        }
    } else if (s->ai_state == 2) {
        if (!(a->slow_tick & 3) && ++s->frame == 0x88) s->ai_state = 3;
    } else if (s->contact_result & 0x10) {
        remove_actor(a, s, 0, index, events);
    } else {
        if (!(a->slow_tick & 7)) s->frame ^= 1;
        --s->y;
    }
}

static void flying_acceleration(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:4B43: horizontal acceleration with one vertical pixel per update;
     * each21-step curve ends by aiming again and pausing40 updates. */
    if (!s->ai_state) { face_player(w, s); s->contact_capability = 0x4d; return; }
    if (s->fields_1c_1f[0]) {
        if (!(a->slow_tick & 1)) animate_three(a, s);
        if (--s->fields_1c_1f[0]) return;
    }
    animate_three(a, s);
    if (a->slow_tick & 1) return;
    unsigned speed = vertical_speed(a, s, 0x7488, 0x15);
    s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -(int)speed : (int)speed));
    s->y = (uint16_t)(s->y + (s->field_18 ? -1 : 1));
    if (s->timer == 0x15) {
        s->timer = 0; face_player(w, s);
        s->field_18 = (uint16_t)(w->player_y + 32) > s->y ? 0 : 1;
        s->fields_1c_1f[0] = 0x28;
    }
}

static void charging_enemy(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:5D5A uses wider19-pixel wall probes while charging. */
    if (!(a->slow_tick & 3)) s->frame ^= 1;
    if (!s->ai_state) {
        s->extra_attributes = 0x10; s->contact_capability = 0x48;
        face_player(w, s); return;
    }
    bool right = s->ai_state == 2 || s->ai_state == 4;
    uint16_t probe = (uint16_t)(s->x + (right ? 19 : -19));
    if (block(w, probe, (uint16_t)(s->y - 11)) < 0x38 ||
        block(w, probe, (uint16_t)(s->y - 5)) < 0x38) {
        s->ai_state ^= s->ai_state < 3 ? 3 : 7;
        s->direction_attributes ^= 0x20;
        return;
    }
    if (s->ai_state < 3) {
        s->x = (uint16_t)(s->x + (right ? 1 : -1));
        if (player_distance(w, s) < 0x24) s->ai_state += 2;
    } else {
        unsigned speed = vertical_speed(a, s, s->field_19 ? 0x74a4 : 0x7488, 15);
        s->x = (uint16_t)(s->x + (right ? (int)speed : -(int)speed));
        if (s->timer == 15) {
            s->timer = 0; s->field_19 ^= 1;
            if (!s->field_19) s->ai_state -= 2;
        }
    }
}

static void rising_walker(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:5E5B walks48 pixels, rises32 pixels, then returns to walking. */
    if (!s->ai_state) {
        s->extra_attributes = 0x10; s->contact_capability = 0x4c;
        face_player(w, s); s->field_19 = 0x30; return;
    }
    if (s->ai_state <= 2) {
        if (!(a->slow_tick & 3)) s->frame ^= 1;
        bool right = s->ai_state == 2;
        if (wall_contact(w, s, right)) {
            s->ai_state ^= 3; s->direction_attributes ^= 0x20; return;
        }
        s->x = (uint16_t)(s->x + (right ? 1 : -1));
        if (!--s->field_19) {
            s->field_19 = 0x30; s->ai_state = 3; s->frame = 0x35;
        }
    } else if (!(a->slow_tick & 1)) {
        if (s->ai_state == 3) {
            if (s->frame != 0x37) { ++s->frame; return; }
            if (!ceiling_contact(w, s)) {
                --s->y;
                if (++s->field_18 != 0x20) return;
            }
            ++s->ai_state;
        } else {
            if (s->frame != 0x35) { --s->frame; return; }
            ++s->y;
            if (!--s->field_18) face_player(w, s);
        }
    }
}

static void extending_enemy(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:5EFF: proximity starts eight-pixel animation steps, paused at each end. */
    if (!s->ai_state) {
        s->extra_attributes = 0x10; s->contact_capability = 0x48;
        if (player_distance(w, s) < 0x30) s->ai_state = 1;
        return;
    }
    if (a->slow_tick & 3) return;
    if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
    if (s->ai_state == 1) {
        s->y += 8;
        if (++s->frame != 0x29) return;
    } else {
        s->y -= 8;
        if (--s->frame != 0x25) return;
    }
    s->fields_1c_1f[0] = 8; s->ai_state ^= 3;
}

static void diamond_flight(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:5F5E traverses two24-update diagonal legs before reversing. */
    if (!(a->slow_tick & 3)) s->frame ^= 1;
    if (!s->ai_state) {
        s->extra_attributes = 0x10; s->contact_capability = 0x48;
        face_player(w, s); s->field_19 = 0x30; return;
    }
    if (a->slow_tick & 3) return;
    s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -2 : 2));
    s->field_19 -= 2;
    if (s->field_19) {
        s->y = (uint16_t)(s->y + (s->field_18 ? 1 : -1));
    } else {
        s->field_19 = 0x30; s->field_18 ^= 1;
        if (!s->field_18) { s->ai_state ^= 3; s->direction_attributes ^= 0x20; }
    }
}

static void animated_walker(const Actors *a, Actor *s, const ActorWorld *w, unsigned routine)
{
    /* Entries reuse source16:4215 (falling) or42F5 (edge reversal). */
    bool falling = routine == 0x5fb9 || routine == 0x5fce || routine == 0x5763;
    animate_four(a, s, routine == 0x54eb ? 1 : 3);
    if (!s->ai_state) {
        s->contact_capability = routine == 0x54eb ? 0x52 : 0x4d;
        if (routine == 0x54eb && !floor_contact(w, s)) { fall(a, s); return; }
        if (routine == 0x54eb) s->timer = 0;
        face_player(w, s);
        if (routine == 0x5fb9) s->extra_attributes = 0x10;
    } else if (falling) walking_fall(a, s, w, true);
    else walking_edges(a, s, w);
}

static void throwing_enemy(Actors *a, Actor *s, const ActorWorld *w, unsigned index)
{
    /* 16:5FDF spawns type24; the child source link points to this slot. */
    if (!s->ai_state) {
        s->contact_capability = 0x4d; face_player(w, s);
        s->fields_1c_1f[0] = 0x18; return;
    }
    if (s->ai_state <= 2) {
        if (s->fields_1c_1f[0] && --s->fields_1c_1f[0]) return;
        spawn_child(a, s, index, 0x24, 0x3e, s->direction_attributes);
        s->fields_1c_1f[0] = 0x10; s->ai_state = 3;
    } else {
        animate_four(a, s, 3);
        if (!--s->fields_1c_1f[0]) {
            s->frame = 0x3b; face_player(w, s); s->fields_1c_1f[0] = 0x70;
        }
    }
}

static void bouncing_projectile(Actors *a, Actor *s, const ActorWorld *w,
                                unsigned index, ActorEvents *events)
{
    /* 16:6031 moves one pixel horizontally and repeats an eight-step bounce. */
    if (!(a->slow_tick & 1)) animate_three(a, s);
    if (!s->ai_state) {
        s->contact_capability = 0x48;
        s->ai_state = s->direction_attributes & 0x20 ? 2 : 1;
        s->x = (uint16_t)(s->x + (s->direction_attributes ? 16 : -16));
        return;
    }
    bool right = s->ai_state == 2;
    if (wall_contact(w, s, right)) { remove_actor(a, s, 2, index, events); return; }
    s->x = (uint16_t)(s->x + (right ? 1 : -1));
    if (!s->field_18) {
        s->y -= vertical_speed(a, s, 0x74ab, 8);
        if (s->timer != 8) return;
    } else {
        if (!floor_contact(w, s)) {
            s->y += vertical_speed(a, s, 0x7488, 8); return;
        }
    }
    s->timer = 0; s->field_18 ^= 1;
}

static void expanding_projectile(const Actors *a, Actor *s)
{
    /* 16:611E expands while moving slowly, then continues at two pixels. */
    if (!s->ai_state) {
        s->contact_capability = 0x48; s->ai_state = s->direction_attributes & 0x20 ? 2 : 1;
        s->y -= 15; s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -11 : 11));
    } else {
        bool early = s->ai_state <= 2, right = !(s->ai_state & 1);
        if (early && (a->slow_tick & 3)) return;
        s->x = (uint16_t)(s->x + (right ? 1 : -1) * (early ? 1 : 2));
        if (early && ++s->frame == 0x49) s->ai_state += 2;
    }
}

static void proximity_shell(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:6179 walks until the player approaches, retracts, waits, reopens. */
    if (s->ai_state < 3) animate_four(a, s, 1);
    if (!s->ai_state) { s->contact_capability = 0x4d; face_player(w, s); return; }
    if (s->ai_state < 3) {
        walking_edges(a, s, w);
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        if (player_distance(w, s) < 0x20) {
            s->ai_state = 3; s->animation = 0; s->frame = 0x63;
            s->contact_capability = 0x4c;
        }
    } else if (s->ai_state == 3) {
        if (!(a->slow_tick & 3) && --s->frame == 0x61) {
            s->ai_state = 4; s->fields_1c_1f[0] = 0x20;
        }
    } else {
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        if (!(a->slow_tick & 3) && ++s->frame == 0x63) {
            face_player(w, s); s->contact_capability = 0x4d; s->fields_1c_1f[0] = 0x40;
        }
    }
}

static void emerging_enemy(const Actors *a, Actor *s)
{
    /* 16:5487 waits hidden, rises on the deceleration curve, then sinks. */
    if (!(a->slow_tick & 3)) {
        if (++s->animation == 4) s->animation = 0;
        const uint8_t *p = rom_at(a, 0x16, 0x7603 + s->animation, 1);
        if (p) s->frame = (uint8_t)(s->frame + *p);
    }
    if (!s->ai_state) {
        s->ai_state = 1; s->contact_capability = 0x4d;
        s->base_attributes = s->fields_1c_1f[0] = 0x80; return;
    }
    if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
    if (s->ai_state == 1) {
        unsigned speed = vertical_speed(a, s, 0x74a4, 15);
        s->y -= speed; s->field_18 += (uint8_t)speed;
        if (s->timer == 15) {
            s->timer = 0; s->fields_1c_1f[0] = 0x30; ++s->ai_state;
        }
    } else if (!(a->slow_tick & 1)) {
        ++s->y;
        if (!--s->field_18) { s->fields_1c_1f[0] = 0x40; s->ai_state = 1; }
    }
}

static void undulating_flight(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:5AAD reuses the12-step rise/fall curves in four successive phases. */
    if (!s->ai_state) { face_player(w, s); s->contact_capability = 0x48; return; }
    if (!(a->slow_tick & 7)) s->frame ^= 1;
    if (a->slow_tick & 1) return;
    s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -1 : 1));
    unsigned speed = vertical_speed(a, s, s->field_18 & 1 ? 0x7488 : 0x74a7, 12);
    bool down = s->field_18 == 1 || s->field_18 == 2;
    s->y = (uint16_t)(s->y + (down ? (int)speed : -(int)speed));
    if (s->timer == 12) { s->timer = 0; s->field_18 = (s->field_18 + 1) & 3; }
}

static void blinking_patrol(const Actors *a, Actor *s, const ActorWorld *w, bool vertical)
{
    /* 16:55FE/566C pause and flash between straight patrol legs. */
    if (!s->ai_state) {
        s->contact_capability = 0x48;
        if (vertical) {
            s->ai_state = 1; s->x -= 4;
            s->field_18 = 0x40; s->fields_1c_1f[0] = 0x20;
        } else {
            s->field_19 = 0x30; face_player(w, s);
            s->extra_attributes = 0x10; s->y -= 4;
        }
        return;
    }
    if (s->fields_1c_1f[0]) {
        if (!(a->slow_tick & 1)) s->extra_attributes ^= 0x10;
        if (--s->fields_1c_1f[0]) return;
        s->extra_attributes = 0x10;
    }
    if (vertical) {
        s->y = (uint16_t)(s->y + (s->ai_state == 1 ? 2 : -2));
        s->field_18 -= 2;
        if (s->field_18) return;
        s->field_18 = 0x40; s->fields_1c_1f[0] = 0x20;
    } else {
        s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -1 : 1));
        if (--s->field_19) return;
        s->field_19 = 0x60; s->fields_1c_1f[0] = 0x10;
    }
    s->ai_state ^= 3;
}

static void short_patrol(const Actors *a, Actor *s, const ActorWorld *w, bool rotate)
{
    /* 16:630E/6721 share16:6325. The rotating family uses eight frames. */
    if (rotate) {
        if (++s->animation == 8) s->animation = 0;
        const uint8_t *p = rom_at(a, 0x16, 0x7669 + s->animation, 1);
        if (p) s->frame = (uint8_t)(s->frame + *p);
        if (s->animation == 3) s->direction_attributes |= 0x40;
        else if (s->animation == 6) s->direction_attributes &= (uint8_t)~0x40;
    } else if (!(a->slow_tick & 1)) s->frame ^= 1;
    if (!s->ai_state) {
        s->contact_capability = 0x48; face_player(w, s);
        s->field_19 = 0x38; s->field_18 = 0x70; return;
    }
    if (a->slow_tick & 1) return;
    s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -1 : 1));
    if (!--s->field_19) {
        s->field_19 = s->field_18; s->ai_state ^= 3; s->direction_attributes ^= 0x20;
    }
}

static void directional_projectile(Actor *s)
{
    /* 16:6816: spawn bit0 selects a rising diagonal instead of horizontal. */
    if (!s->ai_state) {
        s->contact_capability = 0x49;
        s->ai_state = s->direction_attributes & 0x20 ? 2 : 1;
        if (s->direction_attributes & 1) s->direction_attributes &= (uint8_t)~1;
        else s->ai_state += 2;
    } else {
        bool diagonal = s->ai_state <= 2, right = !(s->ai_state & 1);
        s->x = (uint16_t)(s->x + (right ? 1 : -1) * (diagonal ? 1 : 2));
        if (diagonal) --s->y;
    }
}

static void broad_wave_flight(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:7177:21-step curves, each speed one larger than the shared tables. */
    if (!(a->slow_tick & 3)) s->frame ^= 1;
    if (!s->ai_state) {
        s->contact_capability = 0x4d; s->extra_attributes = 0x10; face_player(w, s); return;
    }
    if (a->slow_tick & 1) return;
    s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -2 : 2));
    unsigned speed = vertical_speed(a, s, s->field_18 ? 0x7488 : 0x749e, 21) + 1;
    s->y = (uint16_t)(s->y + (s->field_18 ? (int)speed : -(int)speed));
    if (s->timer == 21) { s->timer = 0; s->field_18 ^= 1; }
}

static void wobbling_hazard(const Actors *a, Actor *s)
{
    /* 16:71C8 wobbles three times, retracts for32 updates, then returns. */
    if (!s->ai_state) {
        s->contact_capability = 0x4c; s->ai_state = 1;
        s->field_18 = s->field_19 = s->animation = 3; return;
    }
    if (s->ai_state == 1) {
        s->y = (uint16_t)(s->y + (s->timer ? 1 : -1));
        if (!--s->field_18) {
            if (!--s->animation) { s->contact_capability = 0; s->ai_state = 2; }
            else { s->field_18 = s->field_19; s->timer ^= 1; }
        }
    } else if (s->ai_state == 2) {
        if (!(a->slow_tick & 3) && ++s->frame == 0x7c) {
            s->fields_1c_1f[0] = 0x20; s->ai_state = 3;
        }
    } else {
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        if (!(a->slow_tick & 3) && --s->frame == 0x7a) {
            s->contact_capability = 0x4c; s->ai_state = 1;
            s->field_18 = s->field_19 = s->animation = 3;
        }
    }
}

static void curve_patrol(const Actors *a, Actor *s, bool horizontal)
{
    /* 16:7241/728C:18-step deceleration/acceleration shared on either axis. */
    if (!s->ai_state) { s->contact_capability = 0x48; s->ai_state = 1; return; }
    unsigned speed = vertical_speed(a, s, s->field_18 ? 0x7488 : 0x74a1, 18);
    int delta = s->ai_state == 1 ? -(int)speed : (int)speed;
    if (horizontal) s->x = (uint16_t)(s->x + delta);
    else s->y = (uint16_t)(s->y + delta);
    if (s->timer == 18) {
        s->timer = 0; s->field_18 ^= 1;
        if (s->field_18) s->ai_state ^= 3;
    }
}

static void triangle_platform(const Actors *a, Actor *s, const ActorWorld *w,
                              unsigned index, ActorEvents *events)
{
    /* 16:5270: diagonal descent uses a half-rate vertical step; ascent is
     * vertical only. Six explicit legs preserve the source initial half-leg. */
    if (!s->ai_state) {
        s->contact_capability = 0x20; s->extra_attributes = 0x10;
        face_player(w, s); s->field_18 = 0x14; return;
    }
    if (!(a->slow_tick & 1)) animate_three(a, s);
    if (s->ai_state == 3 || s->ai_state == 5) {
        platform_motion(s, w, index, events, 0, -1, true);
    } else {
        int dx = s->ai_state == 1 || s->ai_state == 6 ? -1 : 1;
        platform_motion(s, w, index, events, dx, 0, true);
        if (a->slow_tick & 1) return;
        platform_motion(s, w, index, events, 0, 1, true);
    }
    unsigned distance = s->field_18 * (s->ai_state <= 2 ? 1 : 2);
    if (++s->timer != distance) return;
    s->timer = 0;
    if (s->ai_state == 1 || s->ai_state == 6) s->ai_state = 3;
    else if (s->ai_state == 2) s->ai_state = 5;
    else {
        ++s->ai_state;
        if (!(s->ai_state & 1)) s->direction_attributes ^= 0x20;
    }
}

static void reset_from_definition(Actors *a, Actor *s)
{
    /* 16:72BF restores five definition bytes and marks the slot dormant.
     * The original routine intentionally leaves controller counters alone. */
    const ActorDefinition *d = source_definition(a, s);
    if (!d) return;
    s->x = d->x; s->y = d->y; s->definition_flags = d->flags;
    change_type(a, s, s->direction_attributes);
    s->lifecycle = 1; s->hidden = true;
}

static void sinking_platform(Actors *a, Actor *s, const ActorWorld *w,
                             unsigned index, ActorEvents *events)
{
    /* 16:6258 sinks24 pixels while ridden; leaving allows a delayed return. */
    if (!s->ai_state) {
        s->contact_capability = 0x20;
        if (s->contact_result & 8) {
            s->ai_state = 1; s->field_18 = s->field_19 = 0x18;
        }
    } else if (s->ai_state == 1) {
        if (!(s->contact_result & 8)) { s->ai_state = 2; s->fields_1c_1f[0] = 0x10; }
        else if (!(a->slow_tick & 1)) {
            if (!s->field_18) s->ai_state = 3;
            else { --s->field_18; platform_motion(s, w, index, events, 0, 1, true); }
        }
    } else if (s->ai_state == 3) reset_from_definition(a, s);
    else if (s->contact_result & 8) { s->ai_state = 1; s->fields_1c_1f[0] = 0; }
    else {
        if (s->fields_1c_1f[0] && --s->fields_1c_1f[0]) return;
        if (s->field_18 != s->field_19) { ++s->field_18; --s->y; }
    }
}

static void opening_platform(const Actors *a, Actor *s, const ActorWorld *w,
                             unsigned index, ActorEvents *events)
{
    /* 16:6683 opens on contact, carries right112 pixels, then closes. */
    if (!s->ai_state) { s->contact_capability = 0x20; s->ai_state = 4; return; }
    if (s->ai_state == 4) {
        if (s->contact_result & 8) s->ai_state = 1;
    } else if (s->ai_state == 1) {
        if (++s->frame == 0xbe) { s->ai_state = 2; s->field_19 = 0x70; }
    } else if (s->ai_state == 2) {
        if (a->slow_tick & 1) return;
        platform_motion(s, w, index, events, 1, 0, true);
        if (!--s->field_19) { s->ai_state = 3; s->fields_1c_1f[0] = 0x20; }
    } else {
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        if (s->frame != 0xbc) { --s->frame; s->contact_capability = 0; }
    }
}

static void surface_crawler(const Actors *a, Actor *s, const ActorWorld *w, bool ceiling)
{
    /* 16:66E7/6739 reverse when the16-pixel leading surface probe is clear. */
    if (!(a->slow_tick & 3)) s->frame ^= 1;
    if (!s->ai_state) { s->contact_capability = 0x48; face_player(w, s); return; }
    bool right = s->ai_state == 2;
    s->x = (uint16_t)(s->x + (right ? 1 : -1));
    uint16_t x = (uint16_t)(s->x + (right ? 16 : -16));
    bool surface = ceiling ? block(w, x, (uint16_t)(s->y - 18)) < 0x38 :
                            !(s->y & 12) && block(w, x, s->y) < 0x48;
    if (!surface) { s->ai_state ^= 3; s->direction_attributes ^= 0x20; }
}

static void hopping_turner(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:4848 takes three hops per direction; near the player it inserts
     * a six-frame turn animation and a four-pixel side-to-side movement. */
    if (!s->ai_state) {
        face_player(w, s); s->contact_capability = 0x4d; s->fields_1c_1f[0] = 1; return;
    }
    if (s->ai_state >= 3) {
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        if (a->slow_tick & 3) return;
        if (s->animation < 6) {
            bool first = s->animation < 3;
            ++s->animation;
            s->frame = (uint8_t)(s->frame + (first ? 1 : -1));
            bool right = (s->ai_state == 4) == first;
            s->x = (uint16_t)(s->x + (right ? 4 : -4));
            return;
        }
        s->animation = 0; s->fields_1c_1f[0] = 8; s->ai_state -= 2;
        if (s->field_19 != 3) return;
    } else {
        if (s->fields_1c_1f[0]) {
            if (--s->fields_1c_1f[0]) return;
            s->frame = 0x38;
        }
        bool right = s->ai_state == 2;
        if (!wall_contact(w, s, right)) {
            s->x = (uint16_t)(s->x + (right ? 1 : -1));
            if (!s->field_18) {
                s->y -= vertical_speed(a, s, 0x74a7, 12);
                if (s->timer == 12) { s->timer = 0; s->field_18 = 1; s->frame = 0x34; }
                return;
            }
            if (!floor_contact(w, s)) { s->y += vertical_speed(a, s, 0x7488, 12); return; }
            s->timer = s->field_18 = 0; s->fields_1c_1f[0] = 8;
            if (player_distance(w, s) < 0x30) {
                unsigned target = (uint16_t)(w->player_x + 8) < s->x ? 1 : 2;
                if (s->ai_state == target) { ++s->field_19; s->ai_state += 2; }
                else {
                    s->ai_state = (uint8_t)(target + 2);
                    s->field_19 = (uint8_t)(3 - (uint8_t)(s->field_19 + 1));
                    s->direction_attributes ^= 0x20;
                }
                return;
            }
            if (++s->field_19 != 3) return;
        }
    }
    s->field_19 = 0; s->ai_state ^= 3; s->direction_attributes ^= 0x20;
}

static bool tall_wall_contact(const ActorWorld *w, const Actor *s, bool right)
{
    /* 02:44A6/4554 use an eleven-pixel half-width and19/4-pixel heights. */
    uint16_t x = (uint16_t)(s->x + (right ? 11 : -11));
    return block(w, x, (uint16_t)(s->y - 19)) < 0x38 ||
           block(w, x, (uint16_t)(s->y - 4)) < 0x38;
}

static void flying_walker(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:4F4C walks until an obstacle/edge, rises40 pixels, then glides down. */
    if (!s->ai_state) {
        face_player(w, s); s->contact_capability = 0x4d; s->extra_attributes = 0x10; return;
    }
    if (s->ai_state <= 2) {
        animate_collectible(a, s, 1);
        bool obstacle;
        if (!(a->slow_tick & 1)) {
            bool right = s->ai_state == 2;
            s->x = (uint16_t)(s->x + (right ? 1 : -1));
            obstacle = wall_contact(w, s, right);
        } else obstacle = !floor_contact(w, s);
        if (obstacle) { s->ai_state = 5; s->frame = 0x58; s->animation = 0; }
    } else {
        if (!(a->slow_tick & 1)) s->frame ^= 1;
        if (s->ai_state == 5) {
            --s->y;
            if (++s->field_18 == 0x28) {
                s->field_18 = 0; face_player(w, s); s->ai_state += 2; s->fields_1c_1f[0] = 0x20;
            }
            return;
        }
        if (s->fields_1c_1f[0] && --s->fields_1c_1f[0]) return;
        bool landed;
        if (!(a->slow_tick & 1)) {
            bool right = s->ai_state == 4;
            landed = tall_wall_contact(w, s, right);
            if (!landed) s->x = (uint16_t)(s->x + (right ? 2 : -2));
        } else {
            landed = floor_contact(w, s);
            if (!landed) ++s->y;
        }
        if (landed) { face_player(w, s); s->frame = 0x56; }
    }
}

static void bouncing_variant(const Actors *a, Actor *s, const ActorWorld *w, bool emerging)
{
    /* 16:5B0E and62D0 share16:4642 after distinct spawn/animation phases. */
    if (!emerging) {
        if (!(a->slow_tick & 7)) s->direction_attributes ^= 0x20;
        if (!s->ai_state) { face_player(w, s); s->contact_capability = 0x4d; }
        else bouncing_motion(a, s, w);
    } else if (!s->ai_state) {
        s->contact_capability = 0x4d; s->ai_state = 5; s->field_18 = 0x10;
        s->base_attributes = 0x80;
    } else {
        if (!(a->slow_tick & 3)) animate_three(a, s);
        if (s->ai_state != 5) bouncing_motion(a, s, w);
        else if (!(a->slow_tick & 1)) {
            --s->y;
            if (!--s->field_18) { face_player(w, s); s->base_attributes = 0; }
        }
    }
}

static void fast_bouncer(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:5B1F alternates two-pixel side motion and a ceiling-limited jump. */
    if (!(a->slow_tick & 3)) s->frame ^= 1;
    if (!s->ai_state) { face_player(w, s); s->contact_capability = 0x4d; return; }
    if (!(a->slow_tick & 1)) {
        bool right = s->ai_state == 2;
        s->x = (uint16_t)(s->x + (right ? 2 : -2));
        if (wall_contact(w, s, right)) { s->ai_state ^= 3; s->direction_attributes ^= 0x20; }
    } else if (!s->field_18) {
        unsigned speed = vertical_speed(a, s, 0x74a7, 12);
        if (s->timer != 12) {
            s->y -= speed;
            if (!ceiling_contact(w, s)) return;
        }
        s->timer = 0; s->field_18 = 1;
    } else {
        fall(a, s);
        if (floor_contact(w, s)) {
            s->timer = s->field_18 = 0; s->ai_state ^= 3;
        }
    }
}

static void rocket_enemy(const Actors *a, Actor *s, const ActorWorld *w,
                          unsigned index, ActorEvents *events)
{
    /* 16:61F6 points away from the player, then takes a rising three-pixel path. */
    if (!s->ai_state) { s->contact_capability = 0x4d; s->ai_state = 3; return; }
    if (s->ai_state == 3) {
        s->frame |= 1;
        if (player_distance(w, s) < 0x18) {
            face_player(w, s); queued_sound(events, index, s, 1, 0x17);
        }
    } else {
        s->x = (uint16_t)(s->x + (s->ai_state == 1 ? 3 : -3));
        if (!(a->slow_tick & 1)) s->frame ^= 1;
        --s->y;
    }
}

static void shaking_drop(const Actors *a, Actor *s, const ActorWorld *w,
                          unsigned index, ActorEvents *events)
{
    /* 16:55B2 shakes left/right16 times, then falls at twice table speed. */
    if (!s->ai_state) {
        s->contact_capability = 0x48;
        if (player_distance(w, s) < 0x38) {
            s->ai_state = 1; s->fields_1c_1f[0] = 0x10;
            queued_sound(events, index, s, 1, 0x15);
        }
    } else if (s->ai_state == 1) {
        if (!--s->fields_1c_1f[0]) { s->ai_state = 2; return; }
        s->field_19 ^= 1; s->x = (uint16_t)(s->x + (s->field_19 ? 3 : -3));
    } else s->y += (uint16_t)(2 * vertical_speed(a, s, 0x7488, 0x15));
}

static void timed_explosion(Actors *a, Actor *s, unsigned index, ActorEvents *events)
{
    /* 16:54FB flashes48 updates, expands across three frames, then counts a defeat. */
    if (!s->ai_state) {
        s->contact_capability = 0; s->ai_state = 1; s->fields_1c_1f[0] = 0x30;
    } else if (s->ai_state == 1) {
        if (!(a->slow_tick & 1)) { s->frame ^= 1; s->extra_attributes ^= 0x10; }
        if (!--s->fields_1c_1f[0]) {
            s->ai_state = 2; s->extra_attributes = 0x10;
            s->contact_capability = 0x48; s->frame = 0x82;
            queued_sound(events, index, s, 1, 0x0d);
        }
    } else {
        if (!(a->slow_tick & 7)) animate_three(a, s);
        if (s->frame == 0x84) remove_actor(a, s, 1, index, events);
    }
}

static void crushing_enemy(const Actors *a, Actor *s, const ActorWorld *w,
                            unsigned index, ActorEvents *events, bool tall)
{
    /* 16:53DA/6C3C share16:53FB's accelerated drop and its impact sound. */
    if (!s->ai_state) {
        s->extra_attributes = 0x10; s->contact_capability = 0x48;
        if (tall) s->ai_state = 3;
        else if (player_distance(w, s) < 0x30) s->ai_state = 1;
        return;
    }
    if (s->ai_state == 3) {
        if (player_distance(w, s) < 0x30) s->ai_state = 1;
        return;
    }
    if (tall) { if (!(a->slow_tick & 1)) animate_three(a, s); }
    else if (s->ai_state == 1 || !(a->slow_tick & 3)) s->frame ^= 1;
    if (s->ai_state == 1) {
        if (!floor_contact(w, s)) s->y += (uint16_t)(vertical_speed(a, s, 0x7488, 21) + 1);
        else {
            s->timer = 0; s->ai_state = 2; queued_sound(events, index, s, 1, 0x0a);
        }
    } else {
        bool ceiling = tall ? block(w, s->x, (uint16_t)(s->y - 32)) < 0x38 : ceiling_contact(w, s);
        if (ceiling) s->ai_state = tall ? 3 : 0;
        else --s->y;
    }
}

static void orbital_platform(const Actors *a, Actor *s, const ActorWorld *w,
                              unsigned index, ActorEvents *events)
{
    /* 16:6355. Each axis has four curve phases and a separate1..20 index. */
    if (!s->ai_state) {
        s->y -= 0x24; face_player(w, s);
        s->ai_state = s->direction_attributes ? 0x10 : 0x12;
        s->field_18 = 1; s->field_19 = s->direction_attributes ? 0x14 : 1;
        s->contact_capability = 0x48; return;
    }
    if (a->slow_tick & 1) return;
    unsigned phase = s->ai_state >> 4;
    const uint8_t *p = rom_at(a, 0x16,
        (phase == 1 || phase == 4 ? 0x6519 : 0x652f) + s->field_18, 1);
    int speed = p ? *p : 0;
    platform_motion(s, w, index, events, 0, phase <= 2 ? speed : -speed, true);
    bool vertical_end = phase <= 2 ? ++s->field_18 == 21 : !--s->field_18;
    if (vertical_end) {
        s->field_18 = phase == 1 || phase == 4 ? 1 : 20;
        s->ai_state = (uint8_t)((s->ai_state & 15) | ((phase % 4 + 1) << 4));
    }
    phase = s->ai_state & 15;
    p = rom_at(a, 0x16, (phase <= 1 ? 0x6519 : 0x652f) + s->field_19, 1);
    speed = p ? *p : 0;
    platform_motion(s, w, index, events, phase == 0 || phase == 3 ? speed : -speed, 0, true);
    bool horizontal_end = phase == 1 || phase == 2 ? ++s->field_19 == 21 : !--s->field_19;
    if (horizontal_end) {
        s->field_19 = phase <= 1 ? 1 : 20;
        s->ai_state = (uint8_t)((s->ai_state & 0xf0) | ((phase + 1) & 3));
    }
}

static void travelling_platform(Actors *a, Actor *s, const ActorWorld *w,
                                 unsigned index, ActorEvents *events, bool right)
{
    /* 16:65EC/685C: a ridden platform travels two rising legs separated
     * by a shorter descending leg, flashes, then returns to its definition. */
    if (!s->ai_state) {
        if (right) s->extra_attributes = 0x10;
        s->contact_capability = 0x20;
        if (s->contact_result & 8) s->ai_state = 1;
        return;
    }
    if (s->ai_state == 4) {
        if (!(a->slow_tick & 1)) s->extra_attributes ^= 0x10;
        if (!--s->fields_1c_1f[0]) reset_from_definition(a, s);
        return;
    }
    if (a->slow_tick & 1) return;
    platform_motion(s, w, index, events, right ? 1 : -1, s->ai_state == 2 ? 1 : -1, true);
    if (++s->field_19 != (s->ai_state == 2 ? 0x20 : 0x30)) return;
    s->field_19 = 0; ++s->ai_state;
    if (s->ai_state == 3) s->fields_1c_1f[0] = 0x40;
}

static void alternating_platform(const Actors *a, Actor *s, const ActorWorld *w,
                                  unsigned index, ActorEvents *events)
{
    /* 16:6D4D alternates one/two-pixel steps and waits48 updates at each end. */
    if (!s->ai_state) {
        s->contact_capability = 0x20; s->ai_state = 2;
        s->field_19 = 0x28; s->field_18 = 0x50;
        s->extra_attributes = 0x10; s->timer = 1; return;
    }
    if (s->ai_state == 1 || s->ai_state == 3) {
        if (!--s->fields_1c_1f[0]) {
            ++s->ai_state; s->field_19 = s->field_18; s->timer = 0;
        }
        return;
    }
    if (!(a->slow_tick & 3)) {
        if (++s->animation == 4) s->animation = 0;
        const uint8_t *p = rom_at(a, 0x16, 0x7603 + s->animation, 1);
        if (p) s->frame = (uint8_t)(s->frame + *p);
    }
    if (a->easy_mode && (a->slow_tick & 1)) return;
    s->timer ^= 1;
    unsigned speed = s->timer + 1;
    platform_motion(s, w, index, events, s->ai_state == 2 ? (int)speed : -(int)speed, 0, true);
    s->field_19 -= (uint8_t)speed;
    if (!s->field_19) {
        s->ai_state = s->ai_state == 2 ? 3 : 1; s->fields_1c_1f[0] = 0x30;
    }
}

static void sequence_flight(const Actors *a, Actor *s, const ActorWorld *w, bool short_curve)
{
    /* 16:6DDD/6EF4 share explicit four-leg motion, with80 table terminators. */
    animate_four(a, s, 3);
    if (!s->ai_state) {
        s->contact_capability = 0x48; face_player(w, s); s->extra_attributes = 0x10; return;
    }
    bool first = s->ai_state <= 2;
    unsigned horizontal = short_curve ? (first ? 0x6f55 : 0x6f76) : (first ? 0x6e62 : 0x6e87);
    const uint8_t *p = rom_at(a, 0x16, horizontal + s->field_19, 1);
    unsigned speed = p ? *p : 0;
    if (speed == 0x80) {
        static const uint8_t next[4] = {3, 4, 2, 1};
        s->ai_state = next[s->ai_state - 1]; s->field_19 = 0; return;
    }
    bool right = s->ai_state == 2 || s->ai_state == 3;
    s->x = (uint16_t)(s->x + (right ? (int)speed : -(int)speed));
    unsigned vertical = short_curve ? (first ? 0x6f97 : 0x6fb7) : (first ? 0x6eac : 0x6ed0);
    p = rom_at(a, 0x16, vertical + s->field_19, 1);
    speed = p ? *p : 0;
    s->y = (uint16_t)(s->y + (first ? -(int)speed : (int)speed));
    ++s->field_19;
}

static void ricocheting_enemy(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:6B79 switches between floor/ceiling and left/right trajectories. */
    animate_four(a, s, 1);
    if (!s->ai_state) {
        s->contact_capability = 0x4a; face_player(w, s);
        s->fields_1c_1f[3] = s->ai_state; return;
    }
    bool right = !(s->ai_state & 1), down = s->ai_state <= 2;
    bool vertical_hit = down ? floor_contact(w, s) : ceiling_contact(w, s);
    if (vertical_hit) {
        s->ai_state = (uint8_t)(s->ai_state + (down ? 2 : -2));
        s->fields_1c_1f[3] = s->ai_state;
    } else s->y = (uint16_t)(s->y + (down ? 2 : -2));
    if (wall_contact(w, s, right)) {
        s->ai_state = (uint8_t)(s->ai_state + (right ? -1 : 1));
        s->fields_1c_1f[3] = s->ai_state;
    } else s->x = (uint16_t)(s->x + (right ? 2 : -2));
}

static void temporarily_stunned(Actors *a, Actor *s, unsigned index, ActorEvents *events)
{
    /* 16:6C01 restores the preceding type and the behavior retained at1F. */
    if (!s->ai_state) { s->ai_state = 1; s->fields_1c_1f[0] = 0x38; return; }
    if (--s->fields_1c_1f[0]) return;
    if (a->easy_mode) {
        remove_actor(a, s, 2, index, events);
        event(events, ACTOR_EVENT_SPECIAL_REMOVED, index, s, 1); return;
    }
    --s->type; change_type(a, s, s->direction_attributes); s->hidden = true;
    s->contact_capability = 0x49; s->ai_state = s->fields_1c_1f[3];
}

static void shooting_walker(Actors *a, Actor *s, const ActorWorld *w,
                            unsigned index, ActorEvents *events)
{
    /* 16:60AA walks48/88 updates between projectiles and pauses16 to fire. */
    if (!s->ai_state) {
        s->contact_capability = 0x4d; face_player(w, s); s->field_19 = 0x30; return;
    }
    if (!--s->field_19) {
        s->frame = 0x42; spawn_child(a, s, index, 0x26, 0x45, s->direction_attributes);
        s->fields_1c_1f[0] = 0x10; s->ai_state += 2; s->animation = 0;
        queued_sound(events, index, s, 1, 0x11);
    } else if (s->ai_state <= 2) {
        animate_four(a, s, 1); walking_edges(a, s, w);
    } else if (!--s->fields_1c_1f[0]) {
        s->x = (uint16_t)(s->x + (s->direction_attributes ? -3 : 3));
        s->field_19 = 0x58; s->ai_state -= 2;
    }
}

static void random_transformer(const Actors *a, Actor *s, const ActorWorld *w,
                                unsigned index, ActorEvents *events)
{
    /* 16:4A6F. The hardware-divider input selects when walking transforms. */
    bool transform = !s->ai_state;
    if (s->ai_state == 1 || s->ai_state == 2) {
        if (!(a->slow_tick & 1)) s->extra_attributes ^= 0x10;
        if (s->fields_1c_1f[0]) {
            if (--s->fields_1c_1f[0]) return;
            s->frame = 0x40; s->contact_capability = 0x4c;
        }
        if (!(a->slow_tick & 1)) {
            if (++s->animation == 4) s->animation = 0;
            const uint8_t *p = rom_at(a, 0x16, 0x7603 + s->animation, 1);
            if (p) s->frame = (uint8_t)(s->frame + *p);
            if (!w->divider_byte) { event(events, ACTOR_EVENT_ERROR, index, s, 0); return; }
            transform = !(w->divider_byte(w->context) & 0x3f);
            if (!transform) {
                bool right = s->ai_state == 2;
                s->x = (uint16_t)(s->x + (right ? 1 : -1));
                if (wall_contact(w, s, right)) { s->ai_state ^= 3; s->direction_attributes ^= 0x20; }
            }
        }
    } else if (s->ai_state >= 3) {
        if (!(a->slow_tick & 1)) {
            s->frame ^= 1;
            if (++s->field_19 == 0x28) {
                s->field_19 = s->animation = 0; s->ai_state -= 2; s->fields_1c_1f[0] = 0x24;
            }
        }
    }
    if (transform) {
        face_player(w, s); s->ai_state += 2; s->frame = 0x44;
        s->extra_attributes = 0; s->contact_capability = 0x4d;
    }
}

static unsigned track_vertical_player(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:50E9 owns the acceleration index and signed17-entry motion table. */
    if ((uint16_t)(w->player_y + 32) < s->y) { if (s->field_18) --s->field_18; }
    else if (s->field_18 != 16) ++s->field_18;
    const uint8_t *p = rom_at(a, 0x16, 0x5125 + s->field_18, 1);
    int speed = p ? (int8_t)*p : 0;
    s->y = (uint16_t)(s->y + speed);
    return (unsigned)(speed < 0 ? -speed : speed);
}

static void offset_chaser(Actors *a, Actor *s, const ActorWorld *w,
                           unsigned index, ActorEvents *events, bool long_wait)
{
    /* 16:502D/534A uses each of eight spawn offsets at most once. */
    if (!s->ai_state) {
        s->contact_capability = s->direction_attributes = 0; s->extra_attributes = 0x10;
        if (!w->divider_byte) { event(events, ACTOR_EVENT_ERROR, index, s, 0); return; }
        unsigned choice = w->divider_byte(w->context) & 7;
        if (s->fields_1c_1f[3] == 0xff) { remove_actor(a, s, 2, index, events); return; }
        while (s->fields_1c_1f[3] & (1u << choice)) choice = (choice + 1) & 7;
        s->fields_1c_1f[3] |= (uint8_t)(1u << choice);
        const uint8_t *p = rom_at(a, 0x16, 0x5082 + choice, 1);
        if (p) s->y = (uint16_t)(s->y + (int8_t)*p);
        p = rom_at(a, 0x16, 0x508a + choice, 1);
        if (p) s->x = (uint16_t)(s->x + (int8_t)*p);
        s->ai_state = 3; s->fields_1c_1f[0] = 0x30;
    } else if (s->ai_state <= 2) {
        s->frame ^= 1;
        if (a->slow_tick & 3) return;
        unsigned speed = 3 - track_vertical_player(a, s, w);
        s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -(int)speed : (int)speed));
        face_player(w, s);
    } else if (s->ai_state == 3) {
        if (!(a->slow_tick & 7)) s->frame ^= 1;
        if (!--s->fields_1c_1f[0]) {
            s->ai_state = 4; s->contact_capability = 0x52;
            s->frame = long_wait ? 0x70 : 0x6e;
            s->fields_1c_1f[0] = long_wait ? 0x30 : 0x18;
        }
    } else {
        if (!(a->slow_tick & 3)) s->frame ^= 1;
        if (!--s->fields_1c_1f[0]) { face_player(w, s); s->field_18 = 8; }
    }
}

static void reset_defeat(Actors *a, Actor *s, unsigned index, ActorEvents *events, bool count)
{
    /* 16:517A/6A68: defeated animation falls at normal speed, then respawns. */
    if (!s->ai_state) {
        if (count) {
            s->extra_attributes = 0x10; count_defeat(a, s, index, events);
        }
        defeated(a, s, index, events);
    } else if (s->ai_state == 1) defeated(a, s, index, events);
    else {
        fall(a, s);
        if (s->screen_y >= 0xa0) { --s->type; reset_from_definition(a, s); }
    }
}

static void emerging_dart(Actors *a, Actor *s, const ActorWorld *w,
                          unsigned index, ActorEvents *events)
{
    /* 16:69E8 waits until the player is40 pixels away, emerges24 pixels,
     * then crosses the screen quickly and restores its source definition. */
    if (!s->ai_state) {
        s->y += 2; s->base_attributes = 0x80; s->direction_attributes = 0;
        s->contact_capability = 0x4a; s->field_19 = s->fields_1c_1f[0] = 0x18;
        s->ai_state = 5;
    } else if (s->ai_state == 5) {
        if (player_distance(w, s) >= 0x28) face_player(w, s);
    } else if (s->ai_state <= 2) {
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -1 : 1));
        if (!--s->field_19) {
            s->base_attributes = 0; s->ai_state += 2; queued_sound(events, index, s, 1, 0x10);
        }
    } else {
        s->x = (uint16_t)(s->x + (s->ai_state == 3 ? -3 : 3));
        if (s->screen_x >= 0xb8) reset_from_definition(a, s);
    }
}

static void awakening_walker(const Actors *a, Actor *s, const ActorWorld *w,
                              unsigned index, ActorEvents *events)
{
    /* 16:49A6 initially behaves as a platform, then unfolds near the player. */
    if (!s->ai_state) {
        s->contact_capability = 0x20;
        if (player_distance(w, s) < 0x40) s->ai_state = 5;
        return;
    }
    if (s->ai_state == 5) {
        if (!(a->slow_tick & 1)) s->frame ^= 1;
        if (++s->animation == 0x20) { s->animation = 0; s->ai_state = 6; }
    } else if (s->ai_state == 6) {
        if (!(a->slow_tick & 7) && ++s->frame == 0x3e) {
            face_player(w, s); s->contact_capability = 0x4d;
            /* 16:4A64 loads A=08, but00:3D10 consumes B retained by73E0:
             * the actor X high byte. Preserve this original register behavior. */
            carry_player(s, w, index, events, 0, -(int)(s->x >> 8));
        }
    } else {
        if (s->ai_state <= 2 && !(a->slow_tick & 1)) s->frame ^= 1;
        if (!(a->slow_tick & 1)) {
            bool right = !(s->ai_state & 1);
            s->x = (uint16_t)(s->x + (right ? 1 : -1));
            uint16_t probe = (uint16_t)(s->x + (right ? 11 : -11));
            if (block(w, probe, (uint16_t)(s->y - 11)) < 0x38 ||
                block(w, probe, (uint16_t)(s->y - 5)) < 0x38)
                queued_sound(events, index, s, 2, 1);
        } else if (s->ai_state <= 2) {
            if (!floor_contact(w, s)) s->ai_state += 2;
        } else if (!floor_contact(w, s)) fall(a, s);
        else { s->timer = 0; s->ai_state -= 2; }
    }
}

static void fallen_chaser(Actors *a, Actor *s, const ActorWorld *w,
                          unsigned index, ActorEvents *events)
{
    /* 16:5376 falls, waits128 updates, flashes64, then becomes type3E. */
    if (!s->ai_state) {
        s->contact_capability = 0; s->extra_attributes = 0x10; s->ai_state = 1;
        queued_sound(events, index, s, 2, 1); return;
    }
    if (s->ai_state == 1) {
        if (!floor_contact(w, s)) fall(a, s);
        else { s->ai_state = 2; s->fields_1c_1f[0] = 0x80; }
        return;
    }
    if (s->ai_state == 2) {
        if (--s->fields_1c_1f[0]) return;
        s->ai_state = 3; s->fields_1c_1f[0] = 0x40;
    }
    if (!(a->slow_tick & 3)) s->frame ^= 0x1b;
    if (!--s->fields_1c_1f[0]) {
        s->type = 0x3e; change_type(a, s, s->direction_attributes); s->hidden = true;
        face_player(w, s); s->field_18 = 8; s->extra_attributes = 0x10;
        s->contact_capability = 0x52; s->frame = 0x70; s->fields_1c_1f[0] = 0x30;
    }
}

static void watched_ghost(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:5545 advances only when the player's facing matches the side of
     * the actor: the player is facing away from it. */
    if (a->slow_tick & 3) return;
    unsigned anchor = (uint16_t)(w->player_x + 8);
    uint8_t side = anchor > s->x ? 1 : anchor < s->x ? 0xff : 0;
    if (side != w->player_facing) { s->frame = 0x0e; s->ai_state = 0; return; }
    if (!s->ai_state) {
        s->frame = 0x0d; s->direction_attributes = side == 1 ? 0x20 : 0;
        s->ai_state = side == 1 ? 1 : 2; s->field_18 = s->field_19 = 8;
        s->contact_capability = 0x48; return;
    }
    (void)track_vertical_player(a, s, w);
    s->x = (uint16_t)(s->x + (s->ai_state == 1 ? 1 : -1));
    if (s->ai_state == 1) { if (s->field_19 != 16) ++s->field_19; }
    else if (s->field_19) --s->field_19;
}

static void shell_thrower(Actors *a, Actor *s, const ActorWorld *w, unsigned index)
{
    /* 16:56D9 throws its type52 child, then waits96 updates after recoil. */
    if (!s->ai_state) { s->contact_capability = 0x4d; face_player(w, s); return; }
    if (s->ai_state <= 2) {
        if (s->fields_1c_1f[0] && --s->fields_1c_1f[0]) return;
        s->frame = 0x2b; spawn_child(a, s, index, 0x52, 0x2c, s->direction_attributes);
        s->fields_1c_1f[0] = 0x10; s->ai_state = 3;
    } else if (!--s->fields_1c_1f[0]) {
        s->frame = 0x2a; face_player(w, s); s->fields_1c_1f[0] = 0x60;
    }
}

static void thrown_shell(const Actors *a, Actor *s, unsigned index, ActorEvents *events)
{
    /* 16:5728: the thrown shell rises diagonally at half AI rate. */
    if (!s->ai_state) {
        s->contact_capability = 0x4d; s->ai_state = s->direction_attributes & 0x20 ? 2 : 1;
        queued_sound(events, index, s, 2, 0x0b); return;
    }
    if (a->slow_tick & 1) return;
    animate_three(a, s);
    s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -2 : 2)); --s->y;
}

static void rushing_enemy(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:5774 waits nearby, then rushes with the tall body wall probes. */
    if (!s->ai_state) { s->contact_capability = 0x4d; s->ai_state = 3; return; }
    if (s->ai_state == 3) {
        if (!(a->slow_tick & 7)) s->direction_attributes ^= 0x20;
        if (player_distance(w, s) < 0x30) face_player(w, s);
    } else {
        animate_four(a, s, 1);
        bool right = s->ai_state == 2;
        if (tall_wall_contact(w, s, right)) { s->ai_state ^= 3; s->direction_attributes ^= 0x20; }
        else s->x = (uint16_t)(s->x + (right ? 2 : -2));
    }
}

static unsigned motion_table(const Actors *a, unsigned table, uint8_t index)
{
    const uint8_t *p = rom_at(a, 0x16, table + index, 1);
    return p ? *p : 0;
}

static void looping_flight(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:57BD follows eight explicit curve legs from5938..59A5. */
    if (!s->ai_state) {
        s->contact_capability = 0x48; face_player(w, s); s->field_18 = s->field_19 = 1; return;
    }
    animate_four(a, s, 3);
    if (a->slow_tick & 1) return;
    unsigned phase = s->ai_state;
    bool up = phase <= 2 || phase >= 7;
    unsigned vertical = motion_table(a, phase <= 4 ? 0x5938 : 0x597a, s->field_18);
    s->y = (uint16_t)(s->y + (up ? -(int)vertical : (int)vertical));
    if (phase <= 2 || phase == 5 || phase == 6) {
        if (++s->field_18 == 21) s->field_18 = phase <= 2 ? 20 : 1;
    } else if (!--s->field_18) s->field_18 = 1;
    unsigned horizontal = motion_table(a, phase <= 2 ? 0x594e : phase <= 4 ? 0x5964 : 0x5990, s->field_19);
    bool right = phase == 2 || phase == 4 || phase == 5 || phase == 7;
    s->x = (uint16_t)(s->x + (right ? (int)horizontal : -(int)horizontal));
    bool end = phase <= 6 ? ++s->field_19 == 21 : !--s->field_19;
    if (!end) return;
    if (phase <= 4) {
        s->field_19 = 1; s->ai_state += 2;
        if (phase >= 3) {
            if (phase == 4) s->field_18 = 1;
            s->animation = 0; s->direction_attributes ^= 0x20;
        }
    } else if (phase <= 6) { s->field_18 = s->field_19 = 20; s->ai_state += 2; }
    else { s->field_18 = s->field_19 = 1; s->ai_state = phase == 7 ? 2 : 1; }
}

static void diving_flight(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:59A6 emerges64 pixels, settles, then follows alternating dive curves. */
    if (!s->ai_state) { s->contact_capability = 0x4d; s->ai_state = 5; return; }
    if (s->ai_state == 5) {
        s->y -= 2; s->field_18 += 2;
        if (s->field_18 == 0x40) { s->field_18 = 9; s->ai_state = 6; }
    } else if (s->ai_state == 6) {
        s->y += 3; s->field_18 -= 3;
        if (!s->field_18) { s->ai_state = 7; s->frame = 0x97; }
    } else if (s->ai_state == 7) {
        s->y += vertical_speed(a, s, 0x74ab, 8);
        if (s->timer == 8) { s->timer = 0; face_player(w, s); }
    } else {
        unsigned phase = s->ai_state;
        unsigned speed = motion_table(a, 0x5a9d, s->field_19);
        bool right = phase == 2 || phase == 3;
        s->x = (uint16_t)(s->x + (right ? (int)speed : -(int)speed));
        s->field_19 = (uint8_t)(s->field_19 + (phase <= 2 ? 1 : -1));
        if (floor_contact(w, s)) {
            s->field_18 = s->field_19 = s->timer = 0; s->ai_state = 5; s->frame = 0x96; return;
        }
        s->y += motion_table(a, 0x5a8d, s->field_18);
        if (++s->field_18 != 16) return;
        s->field_18 = 0;
        if (phase <= 2) { s->ai_state += 2; --s->field_19; }
        else { s->ai_state = phase == 3 ? 2 : 1; ++s->field_19; }
    }
}

static void bouncing_mount(Actors *a, Actor *s, const ActorWorld *w,
                           unsigned index, ActorEvents *events)
{
    /* 16:6545: the rider is an independently scheduled, parent-linked actor. */
    if (!s->ai_state) {
        s->contact_capability = 0x20; s->extra_attributes = 0x10;
        face_player(w, s);
        spawn_child(a, s, index, 0x45, 0xa8, s->direction_attributes);
        return;
    }
    if (!(a->slow_tick & 7)) {
        if (++s->animation == 4) s->animation = 0;
        const uint8_t *p = rom_at(a, 0x16, 0x7603 + s->animation, 1);
        if (p) s->frame = (uint8_t)(s->frame + *p);
    }
    if (a->slow_tick & 1) return;
    bool right = s->ai_state == 2;
    if (tall_wall_contact(w, s, right)) {
        s->ai_state ^= 3; s->direction_attributes ^= 0x20; return;
    }
    platform_motion(s, w, index, events, right ? 1 : -1, 0, true);
    int speed = vertical_speed(a, s, s->field_18 ? 0x7488 : 0x74ab, 8);
    platform_motion(s, w, index, events, 0, s->field_18 ? speed : -speed, true);
    if (s->timer == 8) { s->timer = 0; s->field_18 ^= 1; }
}

static void linked_rider(Actors *a, Actor *s, unsigned index,
                         ActorEvents *events, bool below)
{
    /* 16:65C7/6969 and74F5 copy the parent's first five stored slot bytes.
     * Earlier slots have already run this frame; later slots have not. */
    if (!s->ai_state) {
        s->ai_state = 1;
        if (!below) { s->contact_capability = 0x4d; s->y -= 24; }
        return;
    }
    animate_four(a, s, below ? 1 : 3);
    unsigned address = s->source_flags_address;
    if (address < 0xad00 || address >= 0xaf00 || (address & 31)) {
        event(events, ACTOR_EVENT_ERROR, index, s, s->type); return;
    }
    const Actor *parent = &a->slots[(address - 0xad00) / 32];
    s->x = parent->x; s->y = parent->y;
    s->definition_flags = parent->definition_flags;
    s->y = (uint16_t)(s->y + (below ? 20 : -24));
    if (below && parent->lifecycle != 2) remove_actor(a, s, 2, index, events);
}

static void collectible_vehicle(Actors *a, Actor *s, const ActorWorld *w,
                                unsigned index, ActorEvents *events)
{
    /* 16:68E4 uses a floor anchor16 pixels below its own position. */
    if (!s->ai_state) {
        face_player(w, s); s->direction_attributes ^= 0x20;
        s->contact_capability = 0x85;
        spawn_child(a, s, index, 0x37, 0x5a, s->direction_attributes);
        s->y -= 16; queued_sound(events, index, s, 1, 0x12); return;
    }
    if (!(a->slow_tick & 1)) {
        s->x = (uint16_t)(s->x + (s->ai_state & 1 ? 3 : -3)); return;
    }
    bool falling = s->ai_state >= 3;
    if (falling) fall(a, s);
    uint16_t anchor = (uint16_t)(s->y + 16);
    bool landed = !(anchor & 12) && block(w, s->x, anchor) < 0x48;
    if (falling && landed) s->ai_state -= 2;
    else if (!falling && !landed) s->ai_state += 2;
}

static void aiming_shooter(Actors *a, Actor *s, const ActorWorld *w,
                           unsigned index, ActorEvents *events)
{
    /* 16:6773: walk, aim, shoot, pause, then reverse the walking direction. */
    if (!s->ai_state) {
        s->contact_capability = 0x4d; face_player(w, s);
        s->field_19 = 0x18; s->field_18 = 0x30; return;
    }
    if (s->ai_state <= 2) {
        if (!(a->slow_tick & 3)) s->frame ^= 1;
        s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -1 : 1));
        if (!--s->field_19) {
            s->fields_1c_1f[3] = s->ai_state; s->ai_state = 3;
            s->fields_1c_1f[0] = 0x10;
            s->frame = (uint16_t)(w->player_y + 32) < s->y ? 0x94 : 0x93;
        }
    } else if (s->ai_state == 3) {
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        spawn_child(a, s, index, 0x4e, 0x91,
                    (uint8_t)(s->direction_attributes + (s->frame == 0x94)));
        s->ai_state = 4; s->fields_1c_1f[0] = 0x20;
        queued_sound(events, index, s, 1, 0x11);
    } else if (!--s->fields_1c_1f[0]) {
        s->frame = 0x92; s->ai_state = s->fields_1c_1f[3] ^ 3;
        s->field_19 = s->field_18; s->direction_attributes ^= 0x20;
    }
}

static void zigzag_thrower(Actors *a, Actor *s, const ActorWorld *w, unsigned index)
{
    /* 16:6FD7: two complete vertical arcs precede the throwing animation. */
    if (!s->ai_state) {
        s->contact_capability = 0x4d; face_player(w, s);
        s->field_18 = 1; s->field_19 = 2; return;
    }
    if (s->ai_state <= 2) {
        s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -1 : 1));
        if (a->slow_tick & 1) return;
        int speed = vertical_speed(a, s, s->field_18 ? 0x7488 : 0x74a4, 15);
        s->y = (uint16_t)(s->y + (s->field_18 ? -speed : speed));
        if (s->timer != 15) return;
        s->timer = 0; s->field_18 ^= 1;
        if (!s->field_18) { s->ai_state ^= 3; s->direction_attributes ^= 0x20; }
        else if (!--s->field_19) {
            s->fields_1c_1f[3] = s->ai_state; s->ai_state = 3;
        }
    } else if (s->ai_state == 3) {
        if (a->slow_tick & 3) return;
        if (++s->frame != 0x70) return;
        s->frame = 0x6c;
        spawn_child(a, s, index, 0x59, 0x70, 0);
        s->ai_state = 4; s->fields_1c_1f[0] = 8;
    } else if (!--s->fields_1c_1f[0]) {
        s->ai_state = s->fields_1c_1f[3]; s->field_19 = 2;
    }
}

static void thrown_hazard(Actors *a, Actor *s, const ActorWorld *w,
                          unsigned index, ActorEvents *events)
{
    /* 16:706D aims at the player independently of its parent's direction. */
    if (!s->ai_state) {
        s->y -= 16; s->contact_capability = 0x48; face_player(w, s); return;
    }
    if (s->ai_state == 3) {
        if (!--s->fields_1c_1f[0]) remove_actor(a, s, 2, index, events);
        return;
    }
    s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -1 : 1));
    if (!s->field_18) {
        s->y -= vertical_speed(a, s, 0x74a7, 12);
        if (s->timer == 12) { s->timer = 0; s->field_18 = 1; }
    } else if (floor_contact(w, s)) {
        s->frame = 0x71; s->ai_state = 3; s->fields_1c_1f[0] = 0x10;
    } else fall(a, s);
}

static void square_flight(const Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:70D5 preserves the initial facing in timer to select turn order. */
    if (!(a->slow_tick & 1)) s->frame ^= 1;
    if (!s->ai_state) {
        s->contact_capability = 0x4d; s->field_19 = 0x18; s->field_18 = 0x30;
        face_player(w, s); s->timer = s->ai_state; return;
    }
    if (s->ai_state <= 2) s->x = (uint16_t)(s->x + (s->ai_state == 1 ? -1 : 1));
    else s->y = (uint16_t)(s->y + (s->ai_state == 3 ? 1 : -1));
    if (--s->field_19) return;
    s->field_19 = s->field_18;
    if (s->ai_state <= 2) {
        bool down = (s->ai_state == 1) == (s->timer == 1);
        if (s->ai_state == 2) s->direction_attributes = 0;
        s->ai_state = down ? 3 : 4; s->frame = down ? 0x78 : 0x76;
    } else {
        bool right = (s->ai_state == 3) == (s->timer == 1);
        s->frame = 0x74; s->ai_state = right ? 2 : 1;
        if (right) s->direction_attributes = 0x20;
    }
}

static void folding_hazard(const Actors *a, Actor *s, unsigned index,
                           ActorEvents *events, bool narrow)
{
    /* 16:6AB9/6B4B: standing on the block unfolds the larger hazard. */
    if (!s->ai_state) {
        s->extra_attributes = 0x10; s->contact_capability = 0x20;
        s->ai_state = 3; s->field_18 = s->field_19 = 0x30; return;
    }
    if (s->ai_state == 3) {
        if (s->contact_result & 8) {
            s->y += 8; s->frame = 0xdd; s->ai_state = 4; s->fields_1c_1f[0] = 4;
        }
    } else if (s->ai_state == 4) {
        if (!--s->fields_1c_1f[0]) {
            s->base_attributes = 0x80; s->x -= narrow ? 48 : 64; s->y -= 64;
            s->frame = 0xc1; s->ai_state = 1; s->contact_capability = 0x48;
        }
    } else {
        unsigned speed = a->easy_mode ? 2 : 4;
        s->y = (uint16_t)(s->y + (s->ai_state == 1 ? (int)speed : -(int)speed));
        s->field_18 -= (uint8_t)speed;
        if (!s->field_18) {
            s->field_18 = s->field_19;
            if (s->ai_state == 1) queued_sound(events, index, s, 1, 0x0a);
            s->ai_state ^= 3;
        }
    }
}

static void crumbling_platform(Actors *a, Actor *s, unsigned index, ActorEvents *events)
{
    /* 16:6C7D has separate normal/easy crumble sprite sequences. */
    if (!s->ai_state) {
        s->contact_capability = 0x20; s->ai_state = 1;
        if (a->easy_mode) s->frame = 0xe0;
    } else if (s->ai_state == 1) {
        if (!(a->slow_tick & 1)) s->frame ^= 1;
        if (s->contact_result & 8) s->ai_state = a->easy_mode ? 3 : 2;
    } else if (!(a->slow_tick & 7)) {
        ++s->frame;
        if (s->frame == (s->ai_state == 2 ? 0xce : 0xe2)) queued_sound(events, index, s, 2, 1);
        else if (s->frame == (s->ai_state == 2 ? 0xd0 : 0xe4)) remove_actor(a, s, 0, index, events);
    }
}

static void disappearing_dart(Actors *a, Actor *s, unsigned index,
                              ActorEvents *events, bool right)
{
    /* 16:6CE4/6D2D use the preceding display X for the reset boundary. */
    if (!s->ai_state) {
        if (right) s->direction_attributes = 0x20;
        s->fields_1c_1f[0] = 0x20; s->ai_state = 2; s->extra_attributes = 0x10;
    } else if (s->ai_state == 2) {
        if (s->fields_1c_1f[0] & 6) s->hidden = true;
        if (!--s->fields_1c_1f[0]) {
            s->ai_state = 1; s->contact_capability = 0x50;
            queued_sound(events, index, s, 2, 6);
        }
    } else {
        if (!(a->slow_tick & 3)) animate_three(a, s);
        s->x = (uint16_t)(s->x + (right ? 2 : -2));
        if (s->screen_x >= (right ? 0xb8 : 0xb0)) reset_from_definition(a, s);
    }
}

static void bumped(Actors *a, Actor *s, const ActorWorld *w)
{
    /* 16:4101. Block impact tosses the actor away from the player. */
    bool right;
    if (!s->ai_state) {
        face_player(w, s); s->direction_attributes ^= 0x20;
        return;
    }
    right = s->ai_state == 1 || s->ai_state == 3;
    s->x = (uint16_t)(s->x + (right ? 1 : -1));
    if (wall_contact(w, s, right))
        s->ai_state = (uint8_t)(s->ai_state + (right ? 1 : -1));
    if (s->ai_state < 3) {
        s->y -= vertical_speed(a, s, 0x74a4, 0x0f);
        if (s->timer == 0x0f) { s->timer = 0; s->ai_state += 2; }
    } else {
        fall(a, s);
        if (floor_contact(w, s)) {
            uint8_t capability = s->contact_capability;
            bool item = s->alternate_ai == 2;
            change_type(a, s, s->direction_attributes); s->hidden = true;
            if (item) {
                s->contact_capability = capability;
                s->ai_state = (s->direction_attributes & 0x20) ? 3 : 2;
            }
        }
    }
}

static bool short_wall_contact(const ActorWorld *w, const Actor *s, bool right)
{
    /* 02:441A/44C8: shell probes are three pixels lower than body probes. */
    uint16_t x = (uint16_t)(s->x + (right ? 7 : -7));
    return block(w, x, (uint16_t)(s->y - 8)) < 0x38 ||
           block(w, x, (uint16_t)(s->y - 5)) < 0x38;
}

static void kill_contact(Actors *a, Actor *s, const ActorWorld *w,
                         unsigned index, ActorEvents *events, bool *clear, uint8_t sound)
{
    /* 02:45FC deliberately retains the old frame and direction. */
    if (sound && w->current_sound != 9) event(events, ACTOR_EVENT_SOUND, index, s, sound);
    count_defeat(a, s, index, events);
    s->type = s->ai_state = s->base_attributes = s->contact_capability = 0;
    s->hidden = true; *clear = true;
}

static bool shell_hits_actors(Actors *a, const Actor *s)
{
    /* 16:7697. Candidate geometry uses cached display coordinates/bounds;
     * its pending contact is consumed according to ordinary slot order. */
    bool hit = false;
    for (unsigned i = 0; i < ACTOR_SLOT_COUNT; ++i) {
        Actor *other = &a->slots[i];
        uint8_t cap = other->contact_capability;
        uint8_t top, left, height, width;
        if (!cap || cap == 0x5f || (cap & 0xa0)) continue;
        top = (uint8_t)(other->screen_y + other->bounds[0] + 3);
        height = (uint8_t)(other->bounds[1] + 10 - other->bounds[0] - 3);
        left = (uint8_t)(other->screen_x + other->bounds[2] - 5);
        width = (uint8_t)(other->bounds[3] + 5 - other->bounds[2] + 5);
        if ((uint8_t)(s->screen_y - top) >= height ||
            (uint8_t)(s->screen_x - left) >= width) continue;
        other->contact_result |= 2; hit = true;
    }
    return hit;
}

static void moving_shell(Actors *a, Actor *s, const ActorWorld *w,
                          unsigned index, ActorEvents *events)
{
    /* 16:439B. Its state direction is the inverse of the walking enemy. */
    bool right;
    if (s->contact_capability & 0x40) (void)shell_hits_actors(a, s);
    if (!(a->slow_tick & 3)) {
        const uint8_t *p;
        if (++s->animation == 4) {
            s->animation = 0; s->direction_attributes &= (uint8_t)~0x20;
        }
        p = rom_at(a, 0x16, 0x7635 + s->animation, 1);
        if (p) s->frame = (uint8_t)(s->frame + *p);
        if (s->animation == 3) s->direction_attributes |= 0x20;
    }
    if (!s->ai_state) { face_player(w, s); s->contact_capability = 0x5f; return; }
    right = s->ai_state == 1 || s->ai_state == 3;
    if (short_wall_contact(w, s, right)) {
        s->ai_state = (uint8_t)(s->ai_state + (right ? 1 : -1));
        event(events, ACTOR_EVENT_SOUND, index, s, 2);
        return;
    }
    s->x = (uint16_t)(s->x + (right ? 3 : -3));
    if (s->ai_state < 3) {
        if (!floor_contact(w, s)) s->ai_state += 2;
    } else if (floor_contact(w, s)) {
        s->timer = 0; s->ai_state -= 2; s->y &= 0xfff0;
    } else s->y += (uint16_t)(vertical_speed(a, s, 0x7488, 0x15) + 1);
}

static void carried_shell(Actors *a, Actor *s, const ActorWorld *w,
                           unsigned index, ActorEvents *events)
{
    /* 16:4444 / 74C1: attachment, release, and impact cancellation. */
    if (w->player_state & 0xf0) {
        a->carrying = false; remove_actor(a, s, 0, index, events); return;
    }
    if (!s->ai_state) { s->contact_capability = 0; s->ai_state = 1; return; }
    s->x = w->player_x + 8;
    s->y = (uint16_t)(w->player_y +
        (w->player_powered ? (w->player_crouching ? 14 : 5) :
                             (w->player_crouching ? 20 : 14)));
    if (shell_hits_actors(a, s) || block(w, s->x, (uint16_t)(s->y - 8)) < 0x38) {
        bool clear = false;
        a->carrying = false;
        kill_contact(a, s, w, index, events, &clear, 0);
        return;
    }
    if (w->run_held) return;
    a->carrying = false; --s->type;
    change_type(a, s, s->direction_attributes); s->hidden = true;
    bool right = w->player_facing == 1;
    s->x = (uint16_t)(s->x + (right ? 16 : -16));
    if (short_wall_contact(w, s, right)) {
        s->x = (uint16_t)(s->x + (right ? -16 : 16));
        s->contact_capability = 0x5f; s->ai_state = right ? 1 : 2;
    }
}

static bool contact_transition(Actors *a, Actor *s, const ActorWorld *w,
                                unsigned index, ActorEvents *events, bool *clear)
{
    /* 02:45AA owns reactions to flags produced by player/block controllers.
     * Branch priority matters if several contacts happened in one frame. */
    uint8_t flags = s->contact_result;
    if (!flags || flags == 8) return false;
    if (flags & 1) {
        s->ai_state = 0;
        if (!w->player_spin && (s->contact_capability & 2)) {
            s->type = (uint8_t)(s->type + ((s->contact_capability & 1) ? -1 : 1));
            event(events, ACTOR_EVENT_SOUND, index, s, 3);
            change_type(a, s, s->direction_attributes);
            s->hidden = true;
            return true;
        }
        kill_contact(a, s, w, index, events, clear, 6);
        return true;
    }
    if (flags & 6) {
        kill_contact(a, s, w, index, events, clear, (flags & 2) ? 6 : 0x10);
        return true;
    }
    if (flags & 0x10) {
        if (s->contact_capability & 0x80) {
            if (s->contact_capability == 0x85) {
                s->type = 0x10; change_type(a, s, 0);
            } else if (s->contact_capability >= 0x86 && s->contact_capability != 0x89) {
                return false;
            } else {
                change_type(a, s, 0); s->type = 0x10;
            }
        } else {
            if (s->contact_capability != 6) return false;
            if (w->run_held) {
                if (a->carrying) return false;
                a->carrying = true; s->type += 2;
                change_type(a, s, s->direction_attributes);
            } else {
                bool right = w->player_facing == 1;
                ++s->type; event(events, ACTOR_EVENT_SOUND, index, s, 3);
                change_type(a, s, s->direction_attributes);
                s->x = (uint16_t)(s->x + (right ? 16 : -16));
                if (short_wall_contact(w, s, right)) {
                    s->x = (uint16_t)(s->x + (right ? -16 : 16));
                    s->contact_capability = 0x5f; s->ai_state = right ? 1 : 2;
                }
            }
        }
        s->hidden = true;
        return true;
    }
    if (!(flags & 0x20)) return false;
    *clear = true;
    if (!(s->contact_capability & 0x80)) {
        uint8_t capability = s->contact_capability;
        s->ai_state = 0;
        if (capability & 0x10) {
            s->type = (uint8_t)(s->type + ((capability & 8) ? -1 : 1));
            event(events, ACTOR_EVENT_SOUND, index, s, 3);
        } else if (capability & 8) {
            kill_contact(a, s, w, index, events, clear, 6);
            return true;
        } else event(events, ACTOR_EVENT_SOUND, index, s, 6);
        change_type(a, s, s->direction_attributes);
        s->alternate_ai = 1; s->hidden = true;
        return true;
    }
    if (s->contact_capability == 0x80) {
        /* 02:46E4: a powered Mario turns hidden type11 into type12. */
        if (s->type == 0x11 && w->player_powered) {
            ActorDefinition *d = source_definition(a, s);
            if (d) d->type = 0x12;
            s->type = 0x12; s->frame = 0x1a;
        }
        s->contact_capability = (uint8_t)(s->type + 0x70);
        s->ai_state = 1; s->y += 0x10;
        event(events, ACTOR_EVENT_SOUND, index, s, 8);
        s->definition_flags = 0x10;
        return false;
    }
    uint8_t capability = s->contact_capability;
    change_type(a, s, s->direction_attributes);
    s->contact_capability = capability;
    s->alternate_ai = 2; s->hidden = true;
    return true;
}

static bool ai(Actors *a, Actor *s, const ActorWorld *w,
                unsigned index, ActorEvents *events)
{
    bool clear_contact = false;
    if (w->player_transition == 0xff) return false;
    if (w->player_goal_walk) s->contact_capability = 0;
    if (contact_transition(a, s, w, index, events, &clear_contact)) return clear_contact;
    /* 16:401B staggers adjacent slots; the AI's own slow_tick tests are
     * additional constraints, not a replacement for this scheduling. */
    if (s->ai_state && ((index & 1u) ^ a->alternating_tick)) return clear_contact;
    if (s->alternate_ai) { bumped(a, s, w); return true; }
    const uint8_t *dispatch = s->type < (a->alternate_tileset ? 112 : 96) ?
        rom_at(a, 0x16, (a->alternate_tileset ? 0x5c07 : 0x4040) + s->type * 2u, 2) : NULL;
    if (!dispatch) {
        event(events, ACTOR_EVENT_UNSUPPORTED, index, s, s->type);
        return true;
    }
    /* Original jump-table entries are data selecting named C handlers.
     * Shared entries in both graphics families retain one interpretation. */
    switch (little_word(dispatch)) {
    case 0x41b1: defeated(a, s, index, events); break;
    case 0x42d1: walker1(a, s, w); break;
    case 0x434f: shell(a, s, w); break;
    case 0x439b: moving_shell(a, s, w, index, events); break;
    case 0x4444: carried_shell(a, s, w, index, events); break;
    case 0x4100: break;
    case 0x462e: bounce8(a, s, w); break;
    case 0x4201: walker9(a, s, w); break;
    case 0x42a9: flattened(a, s, index, events); break;
    case 0x44cb: pipe12(a, s, w, index, events, true); break;
    case 0x44e0: pipe12(a, s, w, index, events, false); break;
    case 0x4541: firing_pipe(a, s, w, index, events); break;
    case 0x45ed: pipe_projectile(a, s); break;
    case 0x46e1: s->contact_capability = 0x85; break;
    case 0x46e6: s->contact_capability = 0x89; break;
    case 0x46eb:
        if (!(a->slow_tick & 3)) s->extra_attributes ^= 0x10;
        /* fall through */
    case 0x46ee: s->contact_capability = (uint8_t)(s->type + 0x66); break;
    case 0x46f5: collected(a, s, index, events); break;
    case 0x4712: hidden_walker(a, s, w); break;
    case 0x4758: case 0x4755: hidden_stationary(a, s); break;
    case 0x47a1:
        if (!(a->slow_tick & 1)) s->extra_attributes ^= 0x10;
        if (!s->ai_state) {
            s->contact_capability = 0x84; s->y -= 0x38; s->ai_state = 1;
        } else ++s->y;
        break;
    case 0x47bd: case 0x47d7: checkpoint(a, s, w, index, events); break;
    case 0x4804: emerging_bubble(a, s, w, index, events); break;
    case 0x4848: hopping_turner(a, s, w); break;
    case 0x49a6: awakening_walker(a, s, w, index, events); break;
    case 0x4a6f: random_transformer(a, s, w, index, events); break;
    case 0x4b43: flying_acceleration(a, s, w); break;
    case 0x4bba: case 0x4bcb: case 0x4c41: case 0x4c52: case 0x4c6a: case 0x4d34:
        oscillating_platform(a, s, w, index, events, little_word(dispatch)); break;
    case 0x4ce0: diagonal_platform(a, s, w, index, events); break;
    case 0x4dd1: falling_platform(a, s, w, index, events); break;
    case 0x4e12: rising_platform(s, w, index, events); break;
    case 0x4e7e: rectangular_platform(s, w, index, events); break;
    case 0x4f4c: flying_walker(a, s, w); break;
    case 0x502d: offset_chaser(a, s, w, index, events, false); break;
    case 0x517a: reset_defeat(a, s, index, events, true); break;
    case 0x51a3:
        if (!s->ai_state) {
            if (!(a->slow_tick & 7)) s->frame ^= 1;
            s->contact_capability = 0x20; s->extra_attributes = 0x10;
            if (s->contact_result & 8) {
                s->ai_state = 1; s->field_18 = s->field_19 = 0x60; s->frame = 0x62;
                queued_sound(events, index, s, 1, 0x16);
            }
        } else {
            if (!(a->slow_tick & 1)) animate_three(a, s);
            rising_platform(s, w, index, events);
        }
        break;
    case 0x51de:
        if (!(a->slow_tick & 1)) animate_three(a, s);
        if (!s->ai_state) { s->contact_capability = 0x4d; s->ai_state = 1; }
        else --s->x;
        break;
    case 0x51f9: floating_enemy(a, s, w); break;
    case 0x5270: triangle_platform(a, s, w, index, events); break;
    case 0x5487: emerging_enemy(a, s); break;
    case 0x53da: crushing_enemy(a, s, w, index, events, false); break;
    case 0x534a: offset_chaser(a, s, w, index, events, true); break;
    case 0x5376: fallen_chaser(a, s, w, index, events); break;
    case 0x54fb: timed_explosion(a, s, index, events); break;
    case 0x55b2: shaking_drop(a, s, w, index, events); break;
    case 0x5545: watched_ghost(a, s, w); break;
    case 0x56d9: shell_thrower(a, s, w, index); break;
    case 0x5728: thrown_shell(a, s, index, events); break;
    case 0x5774: rushing_enemy(a, s, w); break;
    case 0x57bd: looping_flight(a, s, w); break;
    case 0x59a6: diving_flight(a, s, w); break;
    case 0x54eb: case 0x5763: case 0x5baf: case 0x5fb9: case 0x5fce:
        animated_walker(a, s, w, little_word(dispatch)); break;
    case 0x5431: case 0x545a:
        if (!(a->slow_tick & 7)) s->frame ^= 1;
        if (s->ai_state == 1 || s->ai_state == 2) walking_edges(a, s, w);
        else {
            if (little_word(dispatch) == 0x545a) s->extra_attributes = 0x10;
            if (!floor_contact(w, s)) fall(a, s);
            else {
                s->timer = 0; face_player(w, s);
                s->contact_capability = little_word(dispatch) == 0x545a ? 0x48 : 0x4d;
            }
        }
        break;
    case 0x5aad: undulating_flight(a, s, w); break;
    case 0x5b0e: bouncing_variant(a, s, w, false); break;
    case 0x5b1f: fast_bouncer(a, s, w); break;
    case 0x5bc5: paused_walker(a, s, w, true); break;
    case 0x5be1:
        if (!s->ai_state) {
            s->extra_attributes = 0x10; s->ai_state = 1; s->fields_1c_1f[0] = 0x20;
        } else if (!--s->fields_1c_1f[0]) remove_actor(a, s, 1, index, events);
        break;
    case 0x55fe: blinking_patrol(a, s, w, true); break;
    case 0x566c: blinking_patrol(a, s, w, false); break;
    case 0x5ce7: paused_walker(a, s, w, false); break;
    case 0x5d5a: charging_enemy(a, s, w); break;
    case 0x5e06: vertical_patrol(a, s); break;
    case 0x5e5b: rising_walker(a, s, w); break;
    case 0x5eff: extending_enemy(a, s, w); break;
    case 0x5f5e: diamond_flight(a, s, w); break;
    case 0x5fdf: throwing_enemy(a, s, w, index); break;
    case 0x6031: bouncing_projectile(a, s, w, index, events); break;
    case 0x60aa: shooting_walker(a, s, w, index, events); break;
    case 0x611e: expanding_projectile(a, s); break;
    case 0x6179: proximity_shell(a, s, w); break;
    case 0x61f6: rocket_enemy(a, s, w, index, events); break;
    case 0x6236:
        if (!s->ai_state) { s->contact_capability = 0x20; s->ai_state = 1; }
        else {
            if (!(a->slow_tick & 3)) animate_three(a, s);
            if (!(a->slow_tick & 1)) carry_player(s, w, index, events, 1, 0);
        }
        break;
    case 0x6258: sinking_platform(a, s, w, index, events); break;
    case 0x62d0: bouncing_variant(a, s, w, true); break;
    case 0x630e: short_patrol(a, s, w, false); break;
    case 0x6355: orbital_platform(a, s, w, index, events); break;
    case 0x6545: bouncing_mount(a, s, w, index, events); break;
    case 0x65c7: linked_rider(a, s, index, events, false); break;
    case 0x65ec: travelling_platform(a, s, w, index, events, true); break;
    case 0x6667:
        if (!s->ai_state) { s->contact_capability = 0x4d; face_player(w, s); }
        else { animate_four(a, s, 3); walking_fall(a, s, w, false); }
        break;
    case 0x6683: opening_platform(a, s, w, index, events); break;
    case 0x66e7: surface_crawler(a, s, w, true); break;
    case 0x6721: short_patrol(a, s, w, true); break;
    case 0x6739: surface_crawler(a, s, w, false); break;
    case 0x6773: aiming_shooter(a, s, w, index, events); break;
    case 0x6816: directional_projectile(s); break;
    case 0x685c: travelling_platform(a, s, w, index, events, false); break;
    case 0x68e4: collectible_vehicle(a, s, w, index, events); break;
    case 0x6969: linked_rider(a, s, index, events, true); break;
    case 0x68d2:
        s->hidden = true;
        if (!s->ai_state) { s->contact_capability = 0x10; s->ai_state = 1; }
        break;
    case 0x6992:
        if (!(a->slow_tick & 1)) s->frame ^= 1;
        if (!s->ai_state) {
            s->contact_capability = 0x48; s->ai_state = 1; s->field_18 = s->field_19 = 0x40;
        } else if (!(a->slow_tick & 1)) {
            s->y = (uint16_t)(s->y + (s->ai_state == 1 ? 1 : -1));
            if (!--s->field_18) { s->field_18 = s->field_19; s->ai_state ^= 3; }
        }
        break;
    case 0x69d3:
        if (!(a->slow_tick & 7)) s->frame ^= 1;
        s->direction_attributes = 0;
        if (!s->ai_state) { s->contact_capability = 0x4d; s->ai_state = 1; }
        else bouncing_motion(a, s, w);
        break;
    case 0x69e8: emerging_dart(a, s, w, index, events); break;
    case 0x6a71:
        if (!s->ai_state) {
            s->contact_capability = 0x48; s->ai_state = 3; s->field_18 = s->field_19 = 8;
        } else if (s->ai_state == 3) {
            if (player_distance(w, s) < 0x20) { s->frame |= 1; s->ai_state = 1; }
        } else {
            s->y = (uint16_t)(s->y + (s->ai_state == 1 ? -1 : 1));
            if (!--s->field_18) { s->field_18 = s->field_19; s->ai_state ^= 3; }
        }
        break;
    case 0x6a68: reset_defeat(a, s, index, events, false); break;
    case 0x6ab9: folding_hazard(a, s, index, events, false); break;
    case 0x6b4b: folding_hazard(a, s, index, events, true); break;
    case 0x6b79: ricocheting_enemy(a, s, w); break;
    case 0x6c01: temporarily_stunned(a, s, index, events); break;
    case 0x6c3c: crushing_enemy(a, s, w, index, events, true); break;
    case 0x6c7d: crumbling_platform(a, s, index, events); break;
    case 0x6ce4: disappearing_dart(a, s, index, events, false); break;
    case 0x6d2d: disappearing_dart(a, s, index, events, true); break;
    case 0x6d4d: alternating_platform(a, s, w, index, events); break;
    case 0x6ddd: sequence_flight(a, s, w, false); break;
    case 0x6ef4: sequence_flight(a, s, w, true); break;
    case 0x6fd7: zigzag_thrower(a, s, w, index); break;
    case 0x706d: thrown_hazard(a, s, w, index, events); break;
    case 0x70d5: square_flight(a, s, w); break;
    case 0x7177: broad_wave_flight(a, s, w); break;
    case 0x71c8: wobbling_hazard(a, s); break;
    case 0x7241: curve_patrol(a, s, false); break;
    case 0x728c: curve_patrol(a, s, true); break;
    default: event(events, ACTOR_EVENT_UNSUPPORTED, index, s, s->type); break;
    }
    return true;
}

static bool spawn(Actors *a, unsigned definition)
{
    unsigned i;
    ActorDefinition *d = &a->definitions[definition];
    Actor *s;
    for (i = 0; i < ACTOR_SLOT_COUNT && a->slots[i].lifecycle; ++i) {}
    if (i == ACTOR_SLOT_COUNT) return false;
    s = &a->slots[i]; memset(s, 0, sizeof *s);
    s->x = d->x; s->y = d->y; s->type = d->type;
    s->definition_flags = d->flags;
    s->source_flags_address = (uint16_t)(0xab08 + definition * 6);
    s->lifecycle = 1;
    d->flags = 2;
    change_type(a, s, 0);
    update_bounds(a, s);
    ++a->dormant_count;
    return true;
}

static void scan_spawns(Actors *a, const ActorWorld *w)
{
    int direction = w->camera_x > a->previous_camera_x ? 1 :
                    w->camera_x < a->previous_camera_x ? -1 : 0;
    uint16_t edge;
    if (!direction) return;
    edge = direction > 0 ? (uint16_t)(w->camera_x + 0x70) :
        (w->camera_x >= 0x70 ? (uint16_t)(w->camera_x - 0x70) : 0);
    edge &= 0xfff8;
    /* 02:4C31 / 4CEB keep a directional cursor, including the two sentinel
     * positions. They spawn only at the currently examined eight-pixel edge. */
    while (a->spawn_cursor >= 0 && a->spawn_cursor < (int)a->definition_count) {
        ActorDefinition *d = &a->definitions[a->spawn_cursor];
        if ((direction > 0 && d->x > edge) || (direction < 0 && d->x < edge)) break;
        if (d->x == edge && !(d->flags & (a->easy_mode ? 15 : 14)) &&
            !spawn(a, (unsigned)a->spawn_cursor)) break;
        /* Original checks the neighboring sentinel before storing its cursor,
         * so reversing direction must still begin at the last real record. */
        if (a->spawn_cursor + direction < 0 ||
            a->spawn_cursor + direction >= (int)a->definition_count) break;
        a->spawn_cursor += direction;
    }
}

void actors_step(Actors *a, const ActorWorld *w, ActorEvents *events)
{
    unsigned i;
    ActorEvents discarded_events;
    if (!events) events = &discarded_events;
    memset(events, 0, sizeof *events);
    if (!a || !w) return;
    ActorWorld pass_world = *w;
    w = &pass_world;
    ++a->frames;
    a->camera_x = w->camera_x; a->camera_y = w->camera_y;
    a->scroll_x = w->scroll_x; a->scroll_y = w->scroll_y;
    a->alternating_tick ^= 1;
    if (a->alternating_tick) ++a->slow_tick;
    for (i = 0; i < ACTOR_SLOT_COUNT; ++i) {
        Actor *s = &a->slots[i];
        s->hidden = false;
        if (!s->lifecycle) continue;
        if ((a->slow_tick & 1) && !in_window(s->x, w->camera_x, 0xa0)) {
            remove_actor(a, s, 0, i, events);
            continue;
        }
        if (s->lifecycle == 1) {
            /* 02:41D5 activates the record but returns before composition.
             * The first visible frame is its following active AI pass. */
            s->hidden = true;
            if (!(a->slow_tick & 1) && in_window(s->x, w->camera_x, 0x60) &&
                in_window(s->y, w->camera_y, 0x60)) {
                s->lifecycle = 2;
                s->contact_capability = s->inactive_contact_capability;
                --a->dormant_count; ++a->active_count;
            }
            continue;
        }
        if (!(a->slow_tick & 1) && !in_window(s->y, w->camera_y, 0xa0)) {
            if (s->type == 0x10 || (s->source_flags_address >> 8) >= 0xad) {
                remove_actor(a, s, 0, i, events);
            } else {
                s->lifecycle = 1;
                s->inactive_contact_capability = s->contact_capability;
                s->contact_capability = 0;
                --a->active_count; ++a->dormant_count;
            }
            continue;
        }
        unsigned before = events->count;
        bool clear_contact = ai(a, s, w, i, events);
        for (unsigned e = before; e < events->count; ++e)
            if (events->items[e].kind == ACTOR_EVENT_SOUND && !events->items[e].sound_channel)
                pass_world.current_sound = events->items[e].value;
        /* 03:4090 derives display origins even when an AI transition hides
         * this frame. Rendering consumes these facts rather than reparsing. */
        if (s->lifecycle == 2) {
            s->screen_y = (uint8_t)(s->y + 16 - w->scroll_y);
            s->screen_x = (uint8_t)(s->x + 8 - w->scroll_x);
        }
        if (clear_contact) s->contact_result = 0;
        update_bounds(a, s);
    }
    scan_spawns(a, w);
    a->previous_camera_x = w->camera_x;
}

bool actors_visual(const Actors *a, unsigned index, ActorVisual *v)
{
    const Actor *s;
    if (!a || !v || index >= ACTOR_SLOT_COUNT) return false;
    s = &a->slots[index];
    v->slot = index; v->lifecycle = s->lifecycle; v->frame = s->frame;
    v->flags_xor = s->base_attributes ^ s->direction_attributes ^ s->extra_attributes;
    v->x = s->screen_x; v->y = s->screen_y;
    v->alternate_tileset = a->alternate_tileset;
    v->hidden = s->hidden || s->lifecycle != 2 || s->contact_capability == 0x80 ||
                s->screen_x >= 0xb8 || s->screen_y >= 0xb8;
    return true;
}

bool actors_hit_block(Actors *a, uint16_t x, uint16_t y)
{
    bool hidden_item = false;
    /* Input is the actual tile center. 00:2F72 normalizes its hardware-style
     * probe to (tileCenterX-SCX, tileCenterY-SCY), then compares cached OAM
     * origins with unsigned eight-bit differences. */
    uint8_t probe_x = (uint8_t)(x - a->scroll_x);
    uint8_t probe_y = (uint8_t)(y - a->scroll_y);
    for (unsigned i = 0; i < ACTOR_SLOT_COUNT; ++i) {
        Actor *s = &a->slots[i];
        if (!s->contact_result && s->contact_capability &&
            (uint8_t)(s->screen_y - probe_y) < 16 &&
            (uint8_t)(s->screen_x - probe_x) < 20) {
            s->contact_result |= 0x20;
            if (s->contact_capability == 0x80) hidden_item = true;
        }
    }
    return hidden_item;
}
