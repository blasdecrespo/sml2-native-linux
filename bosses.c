#include "bosses.h"

#include <string.h>

static uint8_t byte(const Bosses *b, unsigned address)
{
    return address >= 0x4000 && address < 0x8000 ? b->assets[0x4000 + address] : 0;
}

static unsigned word(const Bosses *b, unsigned address)
{
    return byte(b, address) | (unsigned)byte(b, address + 1) << 8;
}

static void emit(BossEvents *events, BossEventKind kind, unsigned slot,
                  uint16_t value, uint8_t channel)
{
    if (events->count == sizeof events->items / sizeof *events->items) {
        events->overflow = true; return;
    }
    events->items[events->count++] = (BossEvent){.kind=kind, .slot=slot, .value=value, .channel=channel};
}

static void sound(BossEvents *events, unsigned slot, uint8_t channel, uint8_t id)
{
    emit(events, BOSS_EVENT_SOUND, slot, id, channel);
}

static void bounds(const Bosses *b, Actor *s)
{
    unsigned address = word(b, 0x505b + 2u * s->frame);
    for (unsigned i = 0; i < 4; ++i) s->bounds[i] = byte(b, address + i);
}

bool bosses_load(Bosses *b, const uint8_t *assets, size_t size,
                 unsigned level_id, uint8_t phase, uint8_t tick)
{
    /* 02:4D9C clears AD00..AFFF, then copies18 bytes per starting record. */
    static const unsigned levels[6] = {5, 9, 13, 16, 19, 23};
    unsigned configuration;
    if (!b || !assets || size != 0x80000) return false;
    for (configuration = 0; configuration < 6 && levels[configuration] != level_id; ++configuration) {}
    configuration += phase;
    if (configuration >= 9) return false;
    memset(b, 0, sizeof *b);
    b->assets = assets; b->asset_size = size; b->tick = tick;
    b->configuration = (uint8_t)configuration; b->hit_wait = 0x50;
    unsigned p = word(b, 0x4e3f + configuration * 2);
    b->last_slot = byte(b, p); b->health = byte(b, p + 1); b->defeat_frame = byte(b, p + 2);
    for (unsigned i = 0; i <= b->last_slot; ++i) {
        uint8_t encoded[32] = {0};
        unsigned address = word(b, (i ? 0x4eac : 0x4e9a) + configuration * 2);
        if (i) address += (i - 1) * 18;
        for (unsigned j = 0; j < 18; ++j) encoded[5 + j] = byte(b, address + j);
        actors_decode_slot(&b->slots[i], encoded);
    }
    return true;
}

static uint8_t speed(const Bosses *b, Actor *s, unsigned table, uint8_t last)
{
    uint8_t result = byte(b, table + s->timer);
    if (s->timer != last) ++s->timer;
    return result;
}

static void animate(const Bosses *b, Actor *s, uint8_t mask, uint8_t delta)
{
    if (!(b->tick & mask)) s->frame ^= delta;
}

static void flash(const Bosses *b, Actor *s)
{
    if (!(b->tick & 3)) s->extra_attributes ^= 0x10;
}

static void clear_slot(Bosses *b, Actor *s, bool auxiliary)
{
    /* 02:7157/715B: only auxiliary removals decrement the last-slot index. */
    if (auxiliary) --b->last_slot;
    memset(s, 0, sizeof *s); s->hidden = true;
}

static void spawn(Bosses *b, const Actor *parent, uint8_t type, uint8_t frame, uint8_t direction)
{
    /* 02:7224 appends rather than searching holes; new slots run next pass. */
    if (b->last_slot < 15) {
        Actor *s = &b->slots[++b->last_slot];
        s->type = type; s->source_flags_address = parent->source_flags_address;
        s->lifecycle = parent->lifecycle; s->screen_y = parent->screen_y;
        s->screen_x = parent->screen_x; s->frame = frame; s->direction_attributes = direction;
        s->hidden = true; /* The controller captured its slot count before this append. */
    }
    b->spawn_type = b->saved_frame = b->spawn_direction = 0;
}

static bool hit(Bosses *b, Actor *s, unsigned index, BossEvents *events, bool *stomp)
{
    /* 02:5285: a stomp removes two HP; a fireball removes one. */
    *stomp = (s->contact_result & 1) != 0;
    if (!*stomp && !(s->contact_result & 2)) return false;
    s->hidden = true;
    if (index) return true;
    unsigned amount = *stomp ? 2 : 1;
    sound(events, index, *stomp ? 0 : 1, *stomp ? 0x13 : 9);
    bool defeated = b->health <= amount;
    b->health -= (uint8_t)amount;
    if (defeated) {
        b->defeat_phase = 1;
        if (b->configuration < 6) {
            emit(events, BOSS_EVENT_INPUT_WAIT, index, 0x280, 0);
            emit(events, BOSS_EVENT_DEFEATED, index, 1, 0);
        }
    }
    return true;
}

static void begin_defeat(const Bosses *b, Actor *s)
{
    s->ai_state = 2; s->fields_1c_1f[0] = 0x20; s->frame = b->defeat_frame;
}

static void defeat_pause(const Bosses *b, Actor *s, unsigned index, BossEvents *events)
{
    /* 02:5443 switches to the common falling defeat animation. */
    flash(b, s);
    if (--s->fields_1c_1f[0]) return;
    s->ai_state = 0; s->type = 3; s->animation = s->extra_attributes = 0;
    if (!index) sound(events, index, 3, 0xff);
}

static void defeated(Bosses *b, Actor *s, unsigned index, BossEvents *events)
{
    /* 02:5323. The primary waits208 updates after falling before completion. */
    if (!s->ai_state) {
        s->screen_y -= 0x20; s->direction_attributes ^= 0x40;
        s->timer = s->contact_capability = 0; s->ai_state = 1;
        sound(events, index, 0, index ? 0x17 : 0x0d);
    } else if (s->ai_state == 1) {
        s->screen_y -= speed(b, s, 0x7214, 15);
        if (s->timer == 15) { s->ai_state = 2; s->timer = 0; }
    } else if (s->ai_state == 2) {
        s->screen_y += speed(b, s, 0x71a3, 21);
        if (s->screen_y < 0xd0) return;
        if (!index) {
            s->ai_state = 3; s->fields_1c_1f[0] = 0xd0;
            sound(events, index, 3, 0x0c);
        } else { clear_slot(b, s, true); b->defeat_phase = 2; }
    } else if (!--s->fields_1c_1f[0]) {
        clear_slot(b, s, false); b->defeat_phase = 2;
    }
}

static void tree_boss(Bosses *b, Actor *s, const BossWorld *w,
                       unsigned index, BossEvents *events)
{
    /* 02:53A7: rising and falling arcs follow the original5595 speed table. */
    bool stomp;
    if (hit(b, s, index, events, &stomp)) {
        b->saved_frame = s->frame; b->saved_state = s->ai_state;
        b->saved_capability = s->contact_capability;
        if (stomp) { s->frame = 0; s->fields_1c_1f[0] = b->hit_wait; }
        s->contact_capability = 0; s->ai_state = 1; return;
    }
    if (!s->ai_state) {
        animate(b, s, 3, 6);
        if (w->input_wait) return;
        s->contact_capability = 0x45; s->frame = s->ai_state = 3; return;
    }
    if (s->ai_state == 1) {
        flash(b, s);
        if (s->fields_1c_1f[0]) {
            animate(b, s, 1, 1);
            if (--s->fields_1c_1f[0]) return;
        }
        if (b->defeat_phase) { begin_defeat(b, s); return; }
        s->frame = 4;
        if (++s->animation != 0x10) return;
        s->animation = s->extra_attributes = 0;
        s->frame = b->saved_frame; s->ai_state = b->saved_state;
        s->contact_capability = 0x45; return;
    }
    if (s->ai_state == 2) { defeat_pause(b, s, index, events); return; }
    if (s->ai_state == 3) {
        s->screen_y += speed(b, s, 0x71a3, 12);
        if (s->timer == 12) {
            s->timer = 1; s->ai_state = 4; sound(events, index, 2, 2);
        }
        return;
    }
    bool outgoing = s->ai_state == 4 || s->ai_state == 6;
    bool right = s->ai_state == 4 || s->ai_state == 7;
    if (outgoing) {
        animate(b, s, 3, 1);
        if (s->timer >= 12) --s->screen_y;
        uint8_t dx = byte(b, 0x5595 + s->timer);
        s->screen_x = (uint8_t)(s->screen_x + (right ? dx : -dx));
        if (++s->timer != 0x69) return;
        s->timer = 0x68; ++s->ai_state; s->fields_1c_1f[0] = 0x50;
        if (b->health < 3) { b->aggressive = 1; s->timer = 0x5c; }
    } else {
        if (s->fields_1c_1f[0]) {
            animate(b, s, 3, 1);
            if (--s->fields_1c_1f[0]) return;
            s->frame = 5; sound(events, index, 1, 8);
        }
        animate(b, s, 3, 6);
        if (b->aggressive || s->timer >= 12) ++s->screen_y;
        uint8_t dx = byte(b, 0x5595 + s->timer);
        s->screen_x = (uint8_t)(s->screen_x + (right ? dx : -dx));
        if (--s->timer) return;
        s->timer = 1; s->ai_state = right ? 4 : 6; s->frame = 2;
        s->direction_attributes ^= 0x20; sound(events, index, 2, 2);
    }
}

static void tree_exit(Bosses *b, Actor *s)
{
    /* 02:5606, the auxiliary bird waits for the primary's defeat to end. */
    if (s->ai_state || b->defeat_phase != 2) return;
    s->contact_capability = 0; s->ai_state = 1;
    spawn(b, s, 4, 9, b->spawn_direction); s->frame = 7;
}

static void exit_flight(Bosses *b, Actor *s, unsigned index, BossEvents *events, bool trailing)
{
    /* 02:5642/5698: paired upward/downward arcs, then leave through the top. */
    if (!s->ai_state) {
        if (trailing) {
            if (++s->fields_1c_1f[0] != 8) return;
            s->fields_1c_1f[0] = 0; s->ai_state = 1;
            /* 02:56AD loads A=18 but7276 consumes the retained B register.
             * Its preceding controller copy leaves B=0. */
        } else {
            s->ai_state = 1; spawn(b, s, 5, 0x0c, s->direction_attributes);
        }
        return;
    }
    if (trailing) animate(b, s, 7, 1);
    else if (!(b->tick & 1)) {
        uint8_t phase = ++s->animation;
        if (phase == 5) { phase = s->animation = 0; }
        if (phase == 0 || phase == 3) s->direction_attributes ^= 0x20;
        s->frame += byte(b, 0x73b4 + phase);
    }
    if (b->tick & 1) return;
    if (s->ai_state == 1) {
        s->screen_y -= speed(b, s, 0x720e, 21);
        if (s->timer == 21) {
            s->timer = 0; s->ai_state = 2; s->direction_attributes ^= 0x40;
            if (trailing) s->screen_y -= 16;
        }
    } else if (s->ai_state == 2) {
        s->screen_y += speed(b, s, 0x71a3, 21);
        if (s->timer == 21) {
            s->timer = 0; s->ai_state = 3; s->direction_attributes ^= 0x40;
            if (trailing) s->screen_y += 16;
        }
    } else {
        s->screen_y -= 4;
        if (s->screen_y < 8) emit(events, BOSS_EVENT_SCENE, index, 0x1d, 0);
    }
}

static uint8_t random_byte(const BossWorld *w, unsigned index, BossEvents *events)
{
    if (w->divider_byte) return w->divider_byte(w->context);
    emit(events, BOSS_EVENT_ERROR, index, 4, 0); return 0;
}

static void save_hit(Bosses *b, Actor *s, bool stomp, uint8_t frame, uint8_t wait)
{
    b->saved_frame = s->frame; b->saved_state = s->ai_state;
    b->saved_capability = s->contact_capability;
    if (stomp) { s->frame = frame; s->fields_1c_1f[0] = wait; }
    s->contact_capability = 0; s->ai_state = 1;
}

static void turtle_boss(Bosses *b, Actor *s, const BossWorld *w,
                         unsigned index, BossEvents *events)
{
    /* 02:56FE alternates emergence, projectile flight and retreat. */
    bool stomp;
    if (hit(b, s, index, events, &stomp)) { save_hit(b, s, stomp, 0x11, b->hit_wait); return; }
    if (!s->ai_state) {
        if (w->input_wait || ++s->fields_1c_1f[0] != 0x70) return;
        s->fields_1c_1f[0] = 0; s->ai_state = 3; s->animation = 8;
        s->screen_y = 0x70;
        if (random_byte(w, index, events) & 3) b->side ^= 1;
        s->screen_x = b->side ? 0x18 : 0x98;
        s->direction_attributes = b->side ? 0x20 : 0;
        sound(events, index, 1, 2); return;
    }
    if (s->ai_state == 1) {
        flash(b, s);
        if (s->fields_1c_1f[0] && --s->fields_1c_1f[0]) return;
        if (b->defeat_phase) { begin_defeat(b, s); return; }
        s->extra_attributes = 0; s->frame = 0x14; s->ai_state = 5;
        s->contact_capability = 0; return;
    }
    if (s->ai_state == 2) { defeat_pause(b, s, index, events); return; }
    if (s->ai_state == 3) {
        /* 02:5C19 advances the emergence once per eight controller ticks. */
        if (!(b->tick & 7)) { --s->animation; s->frame &= 0xfe; }
        else if ((b->tick & 7) == s->animation) s->frame ^= 1;
        if (s->animation) return;
        s->ai_state = 4; s->frame = 0x10; s->contact_capability = 0x45;
        s->fields_1c_1f[0] = 0x70;
        b->projectile_variant = random_byte(w, index, events) & 3;
        spawn(b, s, 9, 0x1a, s->direction_attributes);
        sound(events, index, 2, 6); return;
    }
    if (s->ai_state == 4) {
        if (--s->fields_1c_1f[0]) return;
        s->frame = 0x14; s->ai_state = 5; s->contact_capability = 0;
    } else if (s->ai_state == 5) {
        if (b->tick & 3) return;
        if (++s->frame != 0x19) return;
        s->frame = 0x0e; s->ai_state = 6; s->field_18 = 0x30;
    } else if (s->ai_state == 6) {
        s->field_18 -= 2;
        if (!s->field_18) s->ai_state = 7;
    } else s->ai_state = 0;
}

static void turn_into_exit(Actor *s, uint8_t y)
{
    /* 02:58F8 shared exit bird replaces several boss auxiliaries. */
    s->screen_y = y; s->screen_x = 0x58; s->type = 4; s->frame = 9;
    s->ai_state = s->timer = s->contact_capability = 0;
}

static void turtle_column(Bosses *b, Actor *s, unsigned index, BossEvents *events, bool middle)
{
    /* 02:5824/58C8; projectile completion selects one arena column. */
    if (middle && b->defeat_phase == 2) { turn_into_exit(s, 0x74); return; }
    if (!s->ai_state) {
        if (!b->field_c8) return;
        if (middle) { if (b->projectile_variant != 2) return; }
        else {
            if (!(b->projectile_variant & 1)) return;
            bool last = (b->projectile_variant == 1) == (b->side != 0);
            if (index != (last ? 3u : 1u)) return;
        }
        s->ai_state = 1; s->field_18 = middle ? 0x40 : 0x50;
        s->fields_1c_1f[0] = 0x40; s->frame = 0x19; return;
    }
    if (s->ai_state == 1) {
        if (!(b->tick & 3)) s->direction_attributes ^= 0x20;
        if (!--s->fields_1c_1f[0]) { s->ai_state = 2; s->frame = 0x13; sound(events, index, 2, 5); }
    } else if (s->ai_state == 2) {
        if (b->defeat_phase) { s->contact_capability = 0; return; }
        s->screen_y -= 2;
        if (s->contact_result & 8) emit(events, BOSS_EVENT_PLAYER_RISE, index, 2, 0);
        s->field_18 -= 2;
        if (!s->field_18) { s->ai_state = 3; s->fields_1c_1f[0] = 0x20; }
    } else if (s->ai_state == 3) {
        if (!--s->fields_1c_1f[0]) { b->field_c8 = 0; s->ai_state = 4; }
    } else {
        uint8_t floor = middle ? 0x74 : 0x84;
        if (s->screen_y < floor) s->screen_y += speed(b, s, 0x71a3, 21);
        else { s->screen_y = floor; s->ai_state = 0; }
    }
}

static void turtle_projectile(Bosses *b, Actor *s)
{
    /* 02:5927 uses separate horizontal and vertical data tables. */
    static const unsigned horizontal[] = {0x59e2, 0x5a63, 0x5ab4, 0x5af1};
    static const unsigned vertical[] = {0, 0x5b3e, 0x5b8f, 0x5bcc};
    animate(b, s, 3, 1);
    if (!s->ai_state) {
        s->contact_capability = 0x40;
        s->ai_state = s->direction_attributes & 0x20 ? 2 : 1;
        s->screen_y -= 0x1c;
        s->screen_x += s->direction_attributes ? 16 : -16; return;
    }
    if (s->ai_state <= 2) {
        unsigned variant = b->projectile_variant & 3;
        uint8_t dx = byte(b, horizontal[variant] + s->timer);
        if (dx == 0x80) {
            s->frame += 2; s->ai_state = 3; s->fields_1c_1f[0] = 0x20; return;
        }
        s->screen_x += s->ai_state == 1 ? -dx : dx;
        if (variant) s->screen_y += byte(b, vertical[variant] + s->timer);
        ++s->timer;
    } else if (s->ai_state == 3) {
        if (--s->fields_1c_1f[0]) return;
        if (b->projectile_variant) { b->field_c8 = 1; s->ai_state = 4; }
        else clear_slot(b, s, true);
    } else if (!b->field_c8) clear_slot(b, s, true);
}

static void restore_after_hit(Bosses *b, Actor *s)
{
    /* 02:5CAC is shared by most later bosses and their destructible helpers. */
    flash(b, s);
    if (s->fields_1c_1f[0] && --s->fields_1c_1f[0]) return;
    if (b->defeat_phase) { begin_defeat(b, s); return; }
    s->extra_attributes = 0; s->frame = b->saved_frame;
    s->ai_state = b->saved_state; s->contact_capability = b->saved_capability;
}

static void macro_begin(Actor *s, unsigned index, BossEvents *events)
{
    s->frame = 0x20; s->fields_1c_1f[0] = 0;
    s->ai_state = 3; s->contact_capability = 0x45;
    sound(events, index, 1, 4);
}

static void macro_boss(Bosses *b, Actor *s, const BossWorld *w,
                        unsigned index, BossEvents *events)
{
    /* 02:5C36/5D4F/5E07: two auxiliary opponents precede the main boss.
     * Auxiliaries keep their own health in slot08; the primary usesAFC5. */
    unsigned type = s->type;
    bool stomp;
    if (hit(b, s, index, events, &stomp)) {
        save_hit(b, s, stomp, 0x1f, b->hit_wait);
        if (type != 12) {
            unsigned damage = stomp ? 2 : 1;
            sound(events, index, stomp ? 0 : 1, stomp ? 0x13 : 9);
            bool dead = s->lifecycle <= damage;
            s->lifecycle -= (uint8_t)damage;
            if (dead) begin_defeat(b, s);
        }
        return;
    }
    if (!s->ai_state) {
        if (!(b->tick & 15)) s->direction_attributes ^= 0x20;
        if (type == 10) {
            if (w->input_wait) return;
        } else {
            if (b->last_slot != (type == 11 ? 1 : 0)) return;
            b->defeat_phase = 0;
            if (++s->fields_1c_1f[0] != 0x30) return;
            if (type == 12) spawn(b, s, 13, 0x0e, b->spawn_direction);
        }
        macro_begin(s, index, events); return;
    }
    if (s->ai_state == 1) { restore_after_hit(b, s); return; }
    if (s->ai_state == 2) {
        animate(b, s, 1, 1); defeat_pause(b, s, index, events); return;
    }
    if (s->ai_state == 3) {
        if (type == 10) ++s->screen_x;
        else { if (type == 11) --s->screen_x; s->fields_1c_1f[0] = 8; }
        if (!s->field_18) {
            s->screen_y -= speed(b, s, 0x7214, 15);
            if (s->timer == 15) { s->timer = 0; s->field_18 = 1; }
        } else {
            s->screen_y += speed(b, s, 0x71a3, 21);
            if (s->screen_y < 0x90) return;
            s->screen_y = 0x90; s->timer = s->field_18 = 0;
            s->ai_state = 4; s->direction_attributes = 0x20;
        }
        return;
    }
    if (type == 10) {
        if (!(b->tick & 3)) {
            if (++s->animation == 8) { s->animation = 0; s->frame -= 7; }
            else ++s->frame;
        }
    } else {
        if (s->fields_1c_1f[0]) {
            s->frame = 0x28;
            if (--s->fields_1c_1f[0]) return;
            s->frame = type == 12 ? 0x29 : 0x20;
        }
        if (type == 12 && (b->tick & 1)) return;
    }
    bool right = type == 10 ? s->ai_state == 4 : s->ai_state == 5;
    uint8_t dx = (uint8_t)(4 - ((type == 12 ? b->health : s->lifecycle) >> 1));
    s->screen_x += right ? dx : -dx;
    if ((right && s->screen_x >= 0xa0) || (!right && s->screen_x < 0x10)) {
        s->ai_state ^= 1; s->direction_attributes ^= 0x20;
        sound(events, index, 1, s->type); return;
    }
    if (type == 10) return;
    if (!s->field_18) {
        s->screen_y -= (uint8_t)(speed(b, s, 0x7214, 15) + (type == 12 ? 2 : 0));
    } else s->screen_y += (uint8_t)(speed(b, s, 0x71a3, 15) + (type == 12 ? 2 : 0));
    if (s->timer == 15) {
        s->timer = 0; s->field_18 ^= 1;
        if (!s->field_18) { s->fields_1c_1f[0] = 8; sound(events, index, 1, s->type); }
    }
}

static void witch_turn(Actor *s, unsigned index, BossEvents *events)
{
    s->field_18 = s->field_19; s->ai_state ^= 1;
    if (!(s->ai_state & 1)) sound(events, index, 1, 7);
}

static void witch_boss(Bosses *b, Actor *s, const BossWorld *w,
                        unsigned index, BossEvents *events)
{
    /* 02:5EF4 alternates climbing and descending passes across the arena. */
    bool stomp;
    if (hit(b, s, index, events, &stomp)) { save_hit(b, s, stomp, 0x30, 0x68); return; }
    if (!s->ai_state) {
        if (w->input_wait) return;
        s->ai_state = 3; s->contact_capability = 0x45;
        s->field_18 = s->field_19 = 0x20; return;
    }
    if (s->ai_state == 1) { restore_after_hit(b, s); return; }
    if (s->ai_state == 2) { defeat_pause(b, s, index, events); return; }
    if (s->ai_state == 3) {
        s->screen_y -= 2; s->field_18 -= 2;
        if (s->field_18) return;
        s->base_attributes = 0; s->ai_state = 4; s->field_18 = s->field_19;
        s->frame = 0x2e; sound(events, index, 1, 7); return;
    }
    if (!(s->ai_state & 1)) {
        if (b->tick & 1) return;
        --s->screen_y;
        if (--s->field_18) return;
        if (random_byte(w, index, events) & 3) {
            Actor *child = &b->slots[b->field_cb ? 2 : 1];
            if (!child->ai_state) {
                child->screen_y = s->screen_y; child->screen_x = s->screen_x; child->frame = 0x31;
                b->field_cb ^= 1; s->frame = 0x2f; s->fields_1c_1f[0] = 0x30;
                witch_turn(s, index, events); return;
            }
        }
        s->frame = 0x2c; witch_turn(s, index, events); return;
    }
    if (s->fields_1c_1f[0]) {
        if (--s->fields_1c_1f[0]) return;
        s->frame = 0x2c;
    }
    animate(b, s, 3, 1);
    if (b->tick & 3) return;
    bool right = s->ai_state == 5;
    if ((right && s->screen_x >= 0xa0) || (!right && s->screen_x < 0x10)) {
        s->ai_state ^= 2; s->direction_attributes ^= 0x20; return;
    }
    s->screen_x += right ? 1 : -1;
    s->screen_y += 2; s->field_18 -= 2;
    if (!s->field_18) { s->frame = 0x2e; witch_turn(s, index, events); }
}

static void witch_helper(Bosses *b, Actor *s, const BossWorld *w,
                          unsigned index, BossEvents *events)
{
    /* 02:6029: launched helpers track player height while crossing the room. */
    if (b->defeat_phase == 2) {
        if (index == 2) turn_into_exit(s, 0x70);
        return;
    }
    bool stomp;
    if (hit(b, s, index, events, &stomp)) return;
    if (!s->ai_state) {
        if (s->frame == 0x0e) return;
        s->ai_state = 3; s->contact_capability = 0x45; return;
    }
    if (s->ai_state == 3) {
        s->screen_y += speed(b, s, 0x7214, 15);
        if (s->timer != 15) return;
        s->timer = 0; s->frame = 0x32; s->field_19 = 0x20;
        bool right = w->player_x >= s->screen_x;
        s->ai_state = right ? 2 : 1; s->direction_attributes = right ? 0x20 : 0; return;
    }
    animate(b, s, 3, 1);
    if (b->tick & 3) return;
    s->screen_x += s->ai_state == 1 ? -1 : 1;
    if (s->screen_x >= 0xb8) { s->frame = 0x0e; s->ai_state = 0; return; }
    s->screen_y += w->player_y >= s->screen_y ? 1 : -1;
}

static void pig_boss(Bosses *b, Actor *s, const BossWorld *w,
                      unsigned index, BossEvents *events)
{
    /* 02:60B9 fires from either side between two table-driven loops. */
    bool stomp;
    if (hit(b, s, index, events, &stomp)) {
        save_hit(b, s, stomp, 0x35, 1);
        if (stomp) s->ai_state = 9;
        return;
    }
    if (!s->ai_state) {
        if (w->input_wait) return;
        s->ai_state = 3; s->contact_capability = 0x45;
        b->projectile_variant = 4; s->fields_1c_1f[0] = 1; return;
    }
    if (s->ai_state == 1) { restore_after_hit(b, s); return; }
    if (s->ai_state == 2) {
        if (!(b->tick & 3)) {
            if (++s->animation == 4) { s->animation = 0; s->frame -= 3; }
            else ++s->frame;
        }
        defeat_pause(b, s, index, events); return;
    }
    if (s->ai_state == 9) {
        if (--s->fields_1c_1f[0]) return;
        sound(events, index, 1, 5); s->ai_state = 1; s->fields_1c_1f[0] = b->hit_wait; return;
    }
    if (s->ai_state == 3 || s->ai_state == 6) {
        if (!b->projectile_variant) {
            s->field_18 = 0; ++s->ai_state; s->timer = 0x5c; return;
        }
        if (s->field_18) s->screen_y -= speed(b, s, 0x71a3, 12);
        else s->screen_y += speed(b, s, 0x7217, 12);
        if (s->timer == 12) { s->timer = 0; s->field_18 ^= 1; }
        if (--s->fields_1c_1f[0]) return;
        Actor *upper = &b->slots[1], *lower = &b->slots[2];
        if (!upper->ai_state) {
            upper->screen_y = s->screen_y;
            upper->screen_x = (uint8_t)(s->screen_x + (s->ai_state == 3 ? -8 : 8));
            upper->frame = 0x3b; upper->direction_attributes = s->direction_attributes;
        }
        if (!lower->ai_state) {
            lower->screen_y = (uint8_t)(s->screen_y + 16); lower->screen_x = s->screen_x;
            lower->frame = 0x3c; lower->direction_attributes = s->direction_attributes;
        }
        --b->projectile_variant; s->fields_1c_1f[0] = 0x60; return;
    }
    bool right = s->ai_state >= 7;
    bool outward = s->ai_state == 4 || s->ai_state == 7;
    s->screen_y += outward ? 1 : -1;
    uint8_t dx = byte(b, 0x5595 + s->timer);
    s->screen_x += right ? dx : -dx;
    if (outward) {
        if (--s->timer) return;
        s->timer = 1; ++s->ai_state; s->direction_attributes ^= 0x20;
    } else {
        if (++s->timer != 0x5d) return;
        s->timer = 0; s->ai_state = right ? 3 : 6;
        s->screen_y = 0x30; s->screen_x = right ? 0x90 : 0x20;
        s->fields_1c_1f[0] = 0x50; b->projectile_variant = random_byte(w, index, events) & 3;
    }
}

static void pig_projectile(Bosses *b, Actor *s, const BossWorld *w,
                            unsigned index, BossEvents *events, bool seeking)
{
    /* 02:627E/62BE: one straight projectile and one proximity-triggered burst. */
    if (seeking && b->defeat_phase == 2) { turn_into_exit(s, 0x80); return; }
    if (!s->ai_state) {
        if (s->frame == 0x0e) return;
        s->contact_capability = 0x40;
        s->ai_state = seeking ? 3 : s->direction_attributes & 0x20 ? 2 : 1;
        if (!seeking) sound(events, index, 2, 9);
        return;
    }
    if (s->ai_state <= 2) {
        if (b->tick & 1) return;
        s->screen_x += s->ai_state == 1 ? -3 : 3;
        if (seeking) {
            uint8_t distance = (uint8_t)(w->player_x - s->screen_x);
            if (distance & 0x80) distance = (uint8_t)-distance;
            if (distance < 0x10) s->ai_state = 4;
        } else if (s->screen_x >= 0xb8) { s->frame = 0x0e; s->ai_state = 0; }
    } else if (s->ai_state == 3) {
        s->screen_y += speed(b, s, 0x71a3, 21);
        if (s->screen_y >= 0x80) s->ai_state = s->direction_attributes & 0x20 ? 2 : 1;
    } else if (s->ai_state == 4) {
        if (!(b->tick & 3) && ++s->frame == 0x3e) { s->ai_state = 5; s->field_18 = 0x30; }
    } else if (s->ai_state == 5) {
        animate(b, s, 3, 1); s->screen_y -= 2; s->field_18 -= 2;
        if (!s->field_18) {
            s->ai_state = 6; s->fields_1c_1f[0] = 6; sound(events, index, 1, 0x0d);
        }
    } else if (!(b->tick & 3)) {
        if (s->frame != 0x41) ++s->frame;
        else if (!--s->fields_1c_1f[0]) { s->ai_state = s->contact_capability = 0; s->frame = 0x0e; }
    }
}

static bool space_curve(const Bosses *b, Actor *s, unsigned y_table,
                         unsigned x_table, int dx, int dy)
{
    uint8_t y = byte(b, y_table + s->timer);
    if (y == 0x80) { s->timer = 0; ++s->ai_state; return true; }
    s->screen_y = (uint8_t)(s->screen_y + dy * y);
    s->screen_x = (uint8_t)(s->screen_x + dx * byte(b, x_table + s->timer));
    ++s->timer; return false;
}

static bool space_straight(Actor *s, int dx, int dy, bool horizontal)
{
    s->screen_x = (uint8_t)(s->screen_x + dx); s->screen_y = (uint8_t)(s->screen_y + dy);
    uint8_t *distance = horizontal ? &s->field_19 : &s->field_18;
    *distance -= (uint8_t)(horizontal ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy));
    return !*distance;
}

static void space_pose(Actor *s, uint8_t frame, uint8_t capability)
{
    s->frame = frame; s->animation = 0; s->contact_capability = capability;
}

static void space_boss(Bosses *b, Actor *s, const BossWorld *w,
                        unsigned index, BossEvents *events)
{
    /* 02:6372:31 named animation/movement phases follow the arena circuit. */
    bool stomp;
    if (hit(b, s, index, events, &stomp)) { save_hit(b, s, stomp, 0x4a, 0x30); return; }
    if (!s->ai_state) {
        if (w->input_wait) return;
        s->ai_state = 3; s->contact_capability = 0x45;
        s->field_19 = 0x24; s->extra_attributes = 0x10; return;
    }
    if (s->ai_state == 1) { restore_after_hit(b, s); return; }
    if (s->ai_state == 2) {
        animate(b, s, 3, 1); defeat_pause(b, s, index, events); s->extra_attributes = 0x10; return;
    }
    s->extra_attributes = 0x10;
    if (!(b->tick & 3)) {
        if (++s->animation == 4) s->animation = 0;
        s->frame += byte(b, 0x7356 + s->animation);
    }
    if (!s->base_attributes && !(b->tick & 1)) {
        sound(events, index, 2, 7);
        if (!(random_byte(w, index, events) & 0x3f)) sound(events, index, 1, 3);
    }
    switch (s->ai_state) {
    case 3: case 9: case 20:
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        if (space_straight(s, s->ai_state == 9 ? 2 : -2, 0, true)) {
            s->base_attributes = 0; ++s->ai_state;
        }
        break;
    case 4:
        if (space_curve(b, s, 0x6472, 0x648c, -1, 1)) s->field_19 = 0x4c;
        break;
    case 5: case 11: case 22:
        if (space_straight(s, s->ai_state == 11 ? 2 : -2, 0, true)) {
            ++s->ai_state; space_pose(s, 0x45, 0x44);
        }
        break;
    case 6: case 12: case 23: case 29: {
        unsigned phase = s->ai_state;
        if (space_curve(b, s, 0x64ea, 0x64f7, phase == 12 || phase == 29 ? 1 : -1, -1))
            s->field_18 = phase == 6 ? 0x1c : phase == 29 ? 0x3c : 0x44;
        break;
    }
    case 7:
        if (!space_straight(s, 0, -1, false)) break;
        s->ai_state = 8; s->frame = 0x42; s->animation = 0; s->field_19 = 0x24;
        s->screen_x += 4; s->base_attributes = 0x80;
        /* Source6504 falls directly into6527 after setting this pose. */
        /* fall through */
    case 8:
        if (space_straight(s, -2, 0, true)) {
            s->ai_state = 9; s->fields_1c_1f[0] = 0x40;
            s->contact_capability = 0x45; s->direction_attributes = 0x20; s->field_19 = 0x24;
        }
        break;
    case 10:
        if (space_curve(b, s, 0x658f, 0x65a0, 1, 1)) s->field_19 = 0x50;
        break;
    case 13: case 24:
        if (space_straight(s, 0, -1, false)) {
            s->direction_attributes = s->ai_state == 13 ? 0x40 : 0x60;
            ++s->ai_state; s->frame = 0x42; s->animation = 0;
        }
        break;
    case 14: case 25:
        if (space_curve(b, s, 0x64f7, 0x64ea, s->ai_state == 14 ? -1 : 1, -1)) s->field_19 = 0x1c;
        break;
    case 15:
        if (space_straight(s, -2, 0, true)) {
            s->ai_state = 16; s->field_18 = 0x60; space_pose(s, 0x45, 0x45);
        }
        break;
    case 16: case 27:
        if (space_straight(s, 0, 3, false)) {
            s->field_19 = s->ai_state == 16 ? 0x1c : 0x38;
            ++s->ai_state; s->direction_attributes = 0x20; s->frame = 0x42; s->animation = 0;
        }
        break;
    case 17:
        if (space_straight(s, 2, 0, true)) s->ai_state = 18;
        break;
    case 18:
        if (space_curve(b, s, 0x66b7, 0x66c3, 1, -1)) { s->field_19 = 0x24; s->base_attributes = 0x80; }
        break;
    case 19:
        if (space_straight(s, 2, 0, true)) {
            s->ai_state = 20; s->direction_attributes = 0;
            s->fields_1c_1f[0] = 0x40; s->field_19 = 0x24;
        }
        break;
    case 21:
        if (space_curve(b, s, 0x6730, 0x673c, -1, 1)) s->field_19 = 0x54;
        break;
    case 26: case 28:
        if (space_straight(s, 2, 0, true)) {
            bool before_drop = s->ai_state == 26;
            ++s->ai_state; space_pose(s, 0x45, before_drop ? 0x45 : 0x44);
            s->direction_attributes = before_drop ? 0x40 : 0x20; s->field_18 = 0x60;
        }
        break;
    case 30:
        if (space_straight(s, 0, -1, false)) {
            s->ai_state = 31; s->frame = 0x42; s->animation = 0;
            s->direction_attributes = 0x20; s->field_19 = 0x24;
            s->screen_x -= 4; s->base_attributes = 0x80;
        }
        break;
    case 31:
        if (space_straight(s, 2, 0, true)) {
            s->ai_state = 3; s->direction_attributes = 0;
            s->contact_capability = 0x45; s->field_19 = 0x24; s->fields_1c_1f[0] = 0x40;
        }
        break;
    default: emit(events, BOSS_EVENT_ERROR, index, s->ai_state, 0); break;
    }
}

static void animate_four(Bosses *b, Actor *s)
{
    if (b->tick & 3) return;
    if (++s->animation == 4) s->animation = 0;
    s->frame += byte(b, 0x7356 + s->animation);
}

static void face_player(Actor *s, const BossWorld *w)
{
    bool right = w->player_x >= s->screen_x;
    s->ai_state = right ? 2 : 1; s->direction_attributes = right ? 0x20 : 0;
}

static uint8_t player_distance(const Actor *s, const BossWorld *w)
{
    uint8_t distance = (uint8_t)(w->player_x - s->screen_x);
    return distance & 0x80 ? (uint8_t)-distance : distance;
}

static void input_wait_low(const BossWorld *w, unsigned index, BossEvents *events)
{
    emit(events, BOSS_EVENT_INPUT_WAIT, index, (uint16_t)((w->input_wait & 0xff00) | 5), 0);
}

static void wario_phase(Bosses *b, Actor *s, const BossWorld *w,
                         unsigned index, BossEvents *events, unsigned phase)
{
    /* 02:6982/6C6A/6ED2 hand back to the ordinary level controller. */
    clear_slot(b, s, false);
    unsigned before = events->count;
    emit(events, BOSS_EVENT_PHASE, index, (uint16_t)phase, 0);
    if (events->count > before && phase < 3) {
        BossEvent *e = &events->items[before];
        e->has_definition = true;
        e->definition = (ActorDefinition){.x=phase == 1 ? 0x7c8 : 0x8c8, .y=0xb0,
            .type=phase == 1 || w->easy_mode ? 0x1d : 0x1b};
    }
}

static bool vertical_arc(Bosses *b, Actor *s, unsigned upward, uint8_t last, unsigned bonus)
{
    unsigned dy = speed(b, s, s->field_18 ? 0x71a3 : upward, last) + bonus;
    s->screen_y += s->field_18 ? (uint8_t)dy : (uint8_t)-dy;
    if (s->timer != last) return false;
    s->timer = 0; s->field_18 ^= 1; return true;
}

static void slam(Bosses *b, Actor *s, unsigned index, BossEvents *events, uint8_t shake, uint8_t wait)
{
    s->ai_state = 9; s->fields_1c_1f[0] = wait; b->field_ce = 1;
    emit(events, BOSS_EVENT_SHAKE, index, shake, 0); sound(events, index, 0, 0x18);
}

static void wario_intro(Bosses *b, Actor *s, const BossWorld *w,
                         unsigned index, BossEvents *events, unsigned form)
{
    if ((uint8_t)w->input_wait == 0x61) {
        sound(events, index, 1, form ? 1 : 0x0f); b->field_cf = 1;
    }
    if (b->field_cf) animate(b, s, 7, 1);
    if (w->input_wait) return;
    b->field_cf = 0; s->ai_state = 3; s->contact_capability = 0x41;
    if (form < 2) s->frame = form ? 0x56 : 0x4f;
}

static void wario_boss(Bosses *b, Actor *s, const BossWorld *w,
                        unsigned index, BossEvents *events, unsigned form)
{
    /* 02:68AB/6B8B/6D93: ground, bunny and fire forms share damage storage. */
    bool stomp;
    if (hit(b, s, index, events, &stomp)) { save_hit(b, s, stomp, 0x52, b->hit_wait); return; }
    if (!s->ai_state) {
        if (!form) { wario_intro(b, s, w, index, events, form); return; }
        s->ai_state = form == 1 ? 7 : 12; s->fields_1c_1f[0] = 0x20;
        spawn(b, s, form == 1 ? 0x16 : 0x19, 0x0e, b->spawn_direction); return;
    }
    if (s->ai_state == 1) { restore_after_hit(b, s); return; }
    if (s->ai_state == 11) { wario_intro(b, s, w, index, events, form); return; }
    if (s->ai_state == 2) {
        if (form == 2) {
            emit(events, BOSS_EVENT_DEFEATED, index, 1, 0); animate(b, s, 7, 0x31);
        } else flash(b, s);
        s->screen_y += speed(b, s, 0x71a3, 21);
        if (s->screen_y < 0x8a) return;
        s->ai_state = form == 2 ? 13 : 6; s->fields_1c_1f[0] = 0x80;
        if (form == 2) { s->timer = 2; sound(events, index, 3, 0x18); }
        return;
    }
    if ((form == 1 && s->ai_state == 7) || (form == 2 && s->ai_state == 12)) {
        if (!b->field_d0) return;
        animate(b, s, 7, form == 1 ? 0x1f : 0x0b);
        if (--s->fields_1c_1f[0]) return;
        s->ai_state = 11; s->frame = form == 1 ? 0x72 : 0x74; b->field_d0 = 0; return;
    }
    if (form < 2 && s->ai_state == 6) {
        if (form) input_wait_low(w, index, events);
        flash(b, s);
        if (--s->fields_1c_1f[0]) return;
        s->animation = s->extra_attributes = 0;
        s->ai_state = 8; s->frame = form ? 0x53 : 0x4c; s->direction_attributes = 0x20; return;
    }
    if (form < 2 && s->ai_state == 8) {
        input_wait_low(w, index, events); flash(b, s); animate_four(b, s); s->screen_x += 2;
        if (s->screen_x >= 0xc0) wario_phase(b, s, w, index, events, form + 1);
        return;
    }
    if (form == 2 && s->ai_state == 13) {
        animate(b, s, 7, 0x31);
        if (--s->fields_1c_1f[0] || --s->timer) return;
        s->ai_state = 8; s->fields_1c_1f[0] = 0xff; s->frame = 0x64; return;
    }
    if (form == 2 && s->ai_state == 8) {
        input_wait_low(w, index, events); animate(b, s, 15, 1);
        if (--s->fields_1c_1f[0]) {
            if (s->fields_1c_1f[0] < 0x20) s->frame = 0x63;
        } else {
            s->fields_1c_1f[0] = 0x30; face_player(s, w); s->ai_state = 9; s->frame = 0x63;
            spawn(b, s, 0x1c, 0x6c, s->direction_attributes);
        }
        return;
    }
    if (form == 2 && s->ai_state == 9) {
        input_wait_low(w, index, events);
        if (--s->fields_1c_1f[0]) return;
        s->ai_state = 10; s->frame = 0x66; s->direction_attributes = s->animation = 0;
        s->fields_1c_1f[0] = 0x30; return;
    }
    if (form == 2 && s->ai_state == 10) {
        input_wait_low(w, index, events);
        if (!(b->tick & 3)) {
            if (++s->animation == 4) { s->animation = 0; s->direction_attributes &= 0xdf; }
            s->frame += byte(b, 0x7388 + s->animation);
            if (s->animation == 3) s->direction_attributes |= 0x20;
        }
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        if (b->tick & 1) return;
        s->screen_x += 3;
        if (s->screen_x >= 0xc0) wario_phase(b, s, w, index, events, 3);
        return;
    }
    if (form < 2 && s->ai_state == 9) {
        if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
        if (!vertical_arc(b, s, 0x7217, 12, 0)) return;
        s->animation = 0; s->frame = form ? 0x53 : 0x4c;
        if (!s->field_18) { s->ai_state = form ? 3 : 4; if (form) s->frame = 0x56; }
        return;
    }
    if (!form) {
        if (s->ai_state == 3) {
            if (!s->field_18) {
                s->screen_y -= speed(b, s, 0x7217, 12);
                if (s->timer == 12) { s->timer = 0; s->field_18 = 1; }
            } else {
                s->screen_y += speed(b, s, 0x71a3, 24);
                if (s->screen_y >= 0x8a) {
                    s->timer = s->field_18 = 0; slam(b, s, index, events, 0x34, 0x30);
                }
            }
        } else if (s->ai_state == 4) {
            if (!(w->timer_bcd & 7)) { s->ai_state = 5; s->frame = 0x4f; return; }
            animate_four(b, s); s->screen_x += s->field_19 ? 2 : -2;
            if ((s->field_19 && s->screen_x >= 0x98) || (!s->field_19 && s->screen_x < 0x10)) {
                s->direction_attributes ^= 0x20; s->field_19 ^= 1; s->fields_1c_1f[0] = 8;
            }
        } else if (s->ai_state == 5) {
            if (vertical_arc(b, s, 0x720e, 21, 0) && !s->field_18)
                slam(b, s, index, events, 0x38, 0x38);
        } else emit(events, BOSS_EVENT_ERROR, index, s->ai_state, 0);
        return;
    }
    if (form == 1) {
        if (s->ai_state == 3) {
            s->screen_y -= speed(b, s, 0x71ff, 36);
            if (s->timer != 36) return;
            s->timer = 0; face_player(s, w); s->ai_state += 3; sound(events, index, 2, 2);
        } else if (s->ai_state == 4 || s->ai_state == 5) {
            animate(b, s, 7, 1);
            bool right = s->ai_state == 5;
            s->screen_x += right ? 1 : -1;
            if ((right && s->screen_x >= 0x90) || (!right && s->screen_x < 0x20)) {
                s->ai_state ^= 1; s->direction_attributes ^= 0x20; sound(events, index, 2, 2); return;
            }
            if (b->tick & 7) return;
            if (player_distance(s, w) < 0x20 && !(w->timer_bcd & 3)) {
                s->ai_state = 10; s->frame = 0x56; s->fields_1c_1f[0] = 0x20; return;
            }
            if (++s->screen_y >= 0x8a) { s->ai_state = 3; s->frame = 0x56; }
        } else if (s->ai_state == 10) {
            if (s->fields_1c_1f[0]) { --s->fields_1c_1f[0]; return; }
            s->screen_y += speed(b, s, 0x71a3, 21);
            if (s->screen_y >= 0x8a) { s->timer = 0; slam(b, s, index, events, 0x38, 0x30); }
        } else emit(events, BOSS_EVENT_ERROR, index, s->ai_state, 0);
        return;
    }
    if (s->ai_state == 3) {
        s->animation = 0; s->frame = 0x5c; face_player(s, w); s->ai_state += 3;
        spawn(b, s, 0x1b, 0x0e, s->direction_attributes);
        sound(events, index, 2, 8); s->fields_1c_1f[0] = 0x50; return;
    }
    if (s->ai_state == 4 || s->ai_state == 5) {
        if (s->fields_1c_1f[0]) {
            if (!--s->fields_1c_1f[0]) s->frame = 0x58;
            return;
        }
        animate_four(b, s);
        bool right = s->ai_state == 5;
        s->screen_x += right ? 2 : -2;
        if ((right && s->screen_x >= 0x98) || (!right && s->screen_x < 0x18)) {
            s->ai_state ^= 1; s->direction_attributes ^= 0x20; return;
        }
        if (player_distance(s, w) >= 0x30) {
            if (!(w->timer_bcd & 3) && b->slots[1].frame < 0x0e) s->ai_state = 3;
        } else {
            s->ai_state += 2; s->frame = 0x5b; b->field_d1 = (uint8_t)(w->player_power - 2);
        }
    } else if (s->ai_state == 6 || s->ai_state == 7) {
        bool right = s->ai_state == 7;
        s->screen_x += right ? 1 : -1;
        if ((right && s->screen_x >= 0x98) || (!right && s->screen_x < 0x18)) {
            s->ai_state ^= 1; s->direction_attributes ^= 0x20; return;
        }
        if (!vertical_arc(b, s, b->field_d1 ? 0x7214 : 0x7208, b->field_d1 ? 15 : 27, 0) || s->field_18) return;
        s->ai_state -= 2; s->frame = 0x58; s->animation = 0;
        if (b->slots[1].frame < 0x0e) s->ai_state = 3;
    } else emit(events, BOSS_EVENT_ERROR, index, s->ai_state, 0);
}

static void wario_ceiling_hazard(Bosses *b, Actor *s, const BossWorld *w,
                                 unsigned index, BossEvents *events)
{
    /* 02:6AA5: a ceiling hazard reacts to the primary's shared slam signal. */
    if (!s->ai_state) { s->ai_state = 1; return; }
    if (s->ai_state == 1) {
        if (!(b->tick & 3)) {
            s->screen_x += s->field_19 ? 3 : -3;
            if (s->screen_x >= 0xb0) s->field_19 ^= 1;
        }
        if (b->field_ce) { b->field_ce = 0; s->ai_state = 2; s->contact_capability = 0x40; }
    } else if (s->ai_state == 2) {
        flash(b, s); s->screen_y += speed(b, s, 0x71a3, 36);
        if (s->timer == 36) { s->extra_attributes = s->timer = 0; s->ai_state = 3; }
    } else if (!(b->tick & 7)) {
        if (++s->frame != 0x63) return;
        s->screen_y = 0x38; s->frame = 0x60; s->contact_capability = s->ai_state = s->field_19 = 0;
        if (random_byte(w, index, events) & 1) s->screen_x = 0xa7;
        else { s->screen_x = 0; s->field_19 = 1; }
    }
}

static void wario_powerup(Bosses *b, Actor *s, bool fire)
{
    /* 02:6B30/6B6E: falling power-up aligns with the primary's stored position. */
    if (!s->ai_state) {
        s->frame = fire ? 0x71 : 0x70; s->screen_x = b->slots[0].screen_x;
        s->screen_y = 0x10; s->ai_state = 1; s->extra_attributes = 0x10; return;
    }
    if (b->tick & 1) return;
    ++s->screen_y;
    if ((uint8_t)((s->screen_y & 0xf0) + 0x20) == (b->slots[0].screen_y & 0xf0)) {
        b->field_d0 = 1; clear_slot(b, s, true);
    }
}

static void wario_fireball(Bosses *b, Actor *s)
{
    /* 02:6FE0 retains signed deltas in707E and loops the late flight segment. */
    if (!(b->tick & 1)) {
        if (++s->animation == 4) { s->animation = 0; s->frame -= 3; }
        else ++s->frame;
    }
    if (!s->ai_state) {
        s->contact_capability = 0x40; s->ai_state = s->direction_attributes & 0x20 ? 2 : 1;
        s->frame = 0x68; s->animation = 0; s->screen_y -= 16; return;
    }
    uint8_t dx = byte(b, 0x7039 + s->timer);
    if (dx == 0x80) { s->timer = 0x3a; return; }
    s->screen_x += s->ai_state == 1 ? -dx : dx;
    if (s->screen_x >= 0xb0) { clear_slot(b, s, true); return; }
    s->screen_y += byte(b, 0x707e + s->timer); ++s->timer;
}

static void wario_coin(Bosses *b, Actor *s, const BossWorld *w,
                       unsigned index, BossEvents *events)
{
    /* 02:70C2: the final dropped object approaches Mario before settling. */
    if (!s->ai_state) { s->ai_state = s->direction_attributes & 0x20 ? 2 : 1; return; }
    if (s->ai_state == 4) return;
    if (s->ai_state <= 2) s->screen_x += s->ai_state == 1 ? -2 : 2;
    if (!(b->tick & 3)) {
        if (++s->animation == 4) { s->animation = 0; s->frame -= 3; }
        else ++s->frame;
    }
    if (s->ai_state <= 2) {
        if (b->tick & 3) return;
        if (player_distance(s, w) < 8) {
            s->ai_state = 3; s->timer = s->field_18 = 0; sound(events, index, 0, 6); return;
        }
        (void)vertical_arc(b, s, 0x7214, 15, 0);
    } else if (!s->field_18) {
        s->screen_y -= speed(b, s, 0x7217, 12);
        if (s->timer == 12) { s->timer = 0; s->field_18 = 1; }
    } else if (s->screen_y < 0x90) s->screen_y += speed(b, s, 0x71a3, 21);
    else { s->frame = 0x6c; s->ai_state = 4; }
}

void bosses_step(Bosses *b, const BossWorld *w, BossEvents *events)
{
    BossEvents discarded;
    if (!events) events = &discarded;
    memset(events, 0, sizeof *events);
    if (!b || !w || !b->assets) return;
    ++b->tick;
    unsigned count = b->last_slot + 1u;
    if (count > ACTOR_SLOT_COUNT) { emit(events, BOSS_EVENT_ERROR, 0, count, 0); return; }
    for (unsigned i = 0; i < count; ++i) {
        Actor s = b->slots[i];
        if (!s.type) continue;
        s.hidden = false;
        if (w->player_transition != 0xff) switch (s.type) {
        case 1: tree_boss(b, &s, w, i, events); break;
        case 2: tree_exit(b, &s); break;
        case 3: defeated(b, &s, i, events); break;
        case 4: exit_flight(b, &s, i, events, false); break;
        case 5: exit_flight(b, &s, i, events, true); break;
        case 6: turtle_boss(b, &s, w, i, events); break;
        case 7: turtle_column(b, &s, i, events, false); break;
        case 8: turtle_column(b, &s, i, events, true); break;
        case 9: turtle_projectile(b, &s); break;
        case 10: case 11: case 12: macro_boss(b, &s, w, i, events); break;
        case 13:
            if (b->defeat_phase == 2) turn_into_exit(&s, 0x90);
            break;
        case 14: witch_boss(b, &s, w, i, events); break;
        case 15: witch_helper(b, &s, w, i, events); break;
        case 16: pig_boss(b, &s, w, i, events); break;
        case 17: pig_projectile(b, &s, w, i, events, false); break;
        case 18: pig_projectile(b, &s, w, i, events, true); break;
        case 19: space_boss(b, &s, w, i, events); break;
        case 20: wario_boss(b, &s, w, i, events, 0); break;
        case 21: case 24: wario_ceiling_hazard(b, &s, w, i, events); break;
        case 22: wario_powerup(b, &s, false); break;
        case 23: wario_boss(b, &s, w, i, events, 1); break;
        case 25: wario_powerup(b, &s, true); break;
        case 26: wario_boss(b, &s, w, i, events, 2); break;
        case 27: wario_fireball(b, &s); break;
        case 28: wario_coin(b, &s, w, i, events); break;
        default:
            b->unsupported = true; emit(events, BOSS_EVENT_UNSUPPORTED, i, s.type, 0); break;
        }
        s.contact_result = 0;
        bounds(b, &s);
        b->slots[i] = s;
    }
}

bool bosses_visual(const Bosses *b, unsigned index, BossVisual *v)
{
    if (!b || !v || index >= ACTOR_SLOT_COUNT) return false;
    const Actor *s = &b->slots[index];
    *v = (BossVisual){s->frame, s->screen_x, s->screen_y,
                     s->base_attributes ^ s->direction_attributes ^ s->extra_attributes,
                     !s->type || s->hidden};
    return true;
}

bool bosses_release_actors(const Bosses *b, Actors *a, const BossEvent *phase)
{
    if (!b || !a || !phase || phase->kind != BOSS_EVENT_PHASE ||
        phase->value < 1 || phase->value > 3 ||
        phase->has_definition != (phase->value < 3)) return false;
    /* Boss initialization cleared AF00..AFFF and its controller never writes
     * the ordinary scheduler/counters. The item handoff sets onlyAF06/1E/1F.
     * A28D is outside that clear and therefore retains the defeat counter. */
    memcpy(a->slots, b->slots, sizeof a->slots);
    a->definition_count = phase->has_definition ? 1 : 0;
    if (phase->has_definition) {
        a->definitions[0] = phase->definition;
        /* 02:6982/6C6A end the reward list with one FF byte atAB0C.
         * AB0D onward retain the previous list, including its old sentinel. */
        if(a->stored_definition_count>1)
            a->definitions[1].x=(uint16_t)(0xff00|(a->definitions[1].x&255));
        else if(!a->stored_definition_count) {
            a->stored_definition_count=1;a->definition_terminator_bytes=1;
        }
    }
    a->spawn_cursor = 0; a->previous_camera_x = 0;
    a->alternating_tick = 0; a->slow_tick = b->tick;
    a->active_count = a->dormant_count = 0; a->carrying = false;
    a->alternate_tileset = phase->has_definition;
    /* Final Wario leaves AF1E/F zero. During the forced rightward ending
     * walk, 02:4C52 compares ROM[0000]=C3 with the camera edge high byte
     * (09) and returns without reading a definition. Represent that
     * source path by an empty spawn list; the shared slots still run. */
    return true;
}
