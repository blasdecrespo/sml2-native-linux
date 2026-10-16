#include "stage.h"
#include <string.h>

static uint8_t actor_block(void *context, uint16_t x, uint16_t y) {
    const Level *level = &((const Stage *)context)->level;
    /* Actor 00:1F2F coordinates are actual map pixels; no player offsets. */
    return level_map_block(level,x,y);
}

static uint8_t divider_byte(void *context) {
    Stage *stage=context;
    return stage->divider_reader(stage->divider_context);
}

static bool block_hit(void *context, const LevelEvent *event) {
    Stage *stage = context;
    return actors_hit_block(&stage->actors, (uint16_t)event->x, (uint16_t)event->y);
}

static void player_fireballs(void *context,Level *level,uint8_t pressed) {
    Stage *stage=context;
    Actor *targets=stage->bosses_ready?stage->bosses.slots:stage->actors.slots;
    fireballs_step(&stage->fireballs,level,targets,pressed);
}

static void append_actors(Stage *stage) {
    for (unsigned i = 0; i < ACTOR_SLOT_COUNT; ++i) {
        ActorVisual v;
        if (!actors_visual(&stage->actors, i, &v) || v.hidden) continue;
        if (!sprites_actor(&stage->sprites, stage->level.rom, stage->level.rom_size,
                           v.frame, v.alternate_tileset, v.y, v.x, v.flags_xor))
            stage->error = true;
    }
}

static void step_bosses(Stage *stage,uint8_t player_x,uint8_t player_y) {
    Level *level=&stage->level;
    BossWorld world={
        .context=stage,.divider_byte=stage->divider_reader?divider_byte:NULL,
        .player_x=player_x,.player_y=player_y,
        .player_transition=level->life_state!=LEVEL_ALIVE?255:level->pending_damage?15:0,
        .player_power=(uint8_t)level->power,
        .timer_bcd=(uint8_t)((level->time%100/10<<4)|(level->time%10)),
        .input_wait=(uint16_t)level->input_wait,.easy_mode=stage->actors.easy_mode,
    };
    BossEvents events;
    bosses_step(&stage->bosses,&world,&events);
    for(unsigned i=0;i<events.count;++i) {
        const BossEvent *event=&events.items[i];
        switch(event->kind) {
            case BOSS_EVENT_SOUND:
                if(event->channel<3) stage->sound_requests[event->channel]=(uint8_t)event->value;
                else if(event->channel==3) {
                    stage->boss_music=(uint8_t)event->value;
                    level->music_request=(uint8_t)event->value;
                }
                break;
            case BOSS_EVENT_INPUT_WAIT:level->input_wait=event->value;break;
            case BOSS_EVENT_DEFEATED:level->boss_defeated=event->value!=0;break;
            case BOSS_EVENT_SCENE:stage->scene_request=(uint8_t)event->value;break;
            case BOSS_EVENT_SHAKE:level->shake_frames=(uint8_t)event->value;break;
            case BOSS_EVENT_PLAYER_RISE:level_apply_actor_movement(level,0,-(int)event->value,0,0);break;
            case BOSS_EVENT_PHASE:
                if(!bosses_release_actors(&stage->bosses,&stage->actors,event)) {
                    stage->error=true;break;
                }
                level->wario_phase=(uint8_t)event->value;
                level->boss_mode=0;
                stage->bosses_ready=false;
                break;
            case BOSS_EVENT_UNSUPPORTED:case BOSS_EVENT_ERROR:
                stage->error=true;break;
        }
    }
    if(events.overflow) stage->error=true;
    for(unsigned i=0;i<ACTOR_SLOT_COUNT;++i) {
        BossVisual visual;
        if(!bosses_visual(&stage->bosses,i,&visual)||visual.hidden) continue;
        if(!sprites_boss(&stage->sprites,level->rom,level->rom_size,
                          visual.frame,visual.y,visual.x,visual.flags_xor)) stage->error=true;
    }
}

static bool start_actors(Stage *stage, bool easy_mode) {
    uint8_t defeated_count=stage->actors.defeated_count;
    /* 03:6BBB preserves FFF0. Its current owner is the active controller;
     * a boss victory leaves the ordinary controller's old value behind. */
    uint8_t slow_tick=stage->bosses_ready?stage->bosses.tick:stage->actors.slow_tick;
    if (!actors_load(&stage->actors, stage->level.rom, stage->level.rom_size, stage->level.id, easy_mode)) return false;
    stage->actors.defeated_count=defeated_count;
    stage->actors.slow_tick=slow_tick;
    stage->fireballs = (Fireballs){0};
    stage->bosses = (Bosses){0};
    stage->bosses_ready=false;
    stage->scene_request=0;
    stage->boss_music=0xff;
    stage->level.block_hit = block_hit;
    stage->level.block_context = stage;
    stage->level.fireballs = player_fireballs;
    stage->level.fireballs_context = stage;
    memset(stage->unsupported_types, 0, sizeof(stage->unsupported_types));
    stage->error = false;
    memset(stage->sound_requests,0,sizeof(stage->sound_requests));
    /* Scene3 initializes assets/actors and requests scene4. Mario's OAM is
     * first composed by the following scene4 player pass (00:060F). */
    sprites_clear(&stage->sprites);
    return true;
}

bool stage_load(Stage *stage, unsigned id, bool easy_mode, bool checkpoint) {
    return level_load_checkpoint(&stage->level, id, checkpoint) && start_actors(stage, easy_mode);
}

bool stage_init(Stage *stage, const uint8_t *rom, size_t size, bool easy_mode) {
    memset(stage, 0, sizeof(*stage));
    return level_init(&stage->level, rom, size) && start_actors(stage, easy_mode);
}

void stage_step(Stage *stage, uint8_t held, uint8_t pressed) {
    Level *level = &stage->level;
    unsigned previous_frame = level->frame;
    bool was_paused = level->paused;
    memset(stage->sound_requests,0,sizeof(stage->sound_requests));
    stage->fireballs.stepped=false;
    level_step(level, held, pressed);
    if(level->room_script_unsupported) stage->error=true;
    if (level->frame == previous_frame) return;
    if (level->room_script_frame) return;
    /* Entering pause still completes the current player/actor pass. The
     * scene8 resume handler returns before another gameplay update. */
    if (was_paused) return;
    if (level->exit != LEVEL_EXIT_NONE && !level->exit_started_this_frame) return;
    if(level->boss_started_this_frame) {
        stage->bosses_ready=bosses_load(&stage->bosses,level->rom,level->rom_size,
                                        level->id,level->wario_phase,stage->actors.slow_tick);
        if(!stage->bosses_ready) {stage->error=true;return;}
        stage->boss_music=level->id==0x18?0x21+level->wario_phase:0x20;
        level->default_music=level->music_request=stage->boss_music;
    }
    held=level->effective_held;pressed=level->effective_pressed;
    stage->sound_requests[0] = level->sound_request;
    stage->sound_requests[1] = level->sound_secondary;
    stage->sound_requests[2] = level->sound_noise;
    /* 00:060F: Mario and block effects are composed before player contacts.
     * Their OAM positions stay latched if a stomp subsequently adjusts Y. */
    sprites_clear(&stage->sprites);
    if(stage->fireballs.stepped) {
        stage->sprites=stage->fireballs.sprites;
        if(stage->fireballs.sound_requests[1])stage->sound_requests[1]=stage->fireballs.sound_requests[1];
        if(stage->fireballs.sound_requests[2])stage->sound_requests[2]=stage->fireballs.sound_requests[2];
        if(stage->fireballs.error) stage->error=true;
    }
    level_compose_sprites(level, &stage->sprites);
    uint8_t player_x=(uint8_t)(level->x-level->camera_x+96);
    uint8_t player_y=(uint8_t)(level->y-level->camera_y+98);
    /* Consume early wave/noise events before the contact producer runs. */
    level->sound_secondary=level->sound_noise=0;
    Actor *targets=stage->bosses_ready?stage->bosses.slots:stage->actors.slots;
    if (level->life_state == LEVEL_ALIVE || level->death_started_this_frame) (void)contacts_step(targets, level, held);
    if(level->life_state==LEVEL_ALIVE||level->death_started_this_frame) {
        level_step_water_effect(level);
        level_compose_water_effect(level,&stage->sprites);
    }
    stage->sound_requests[0] = level->sound_request;
    if(level->sound_secondary)stage->sound_requests[1]=level->sound_secondary;
    if(level->sound_noise)stage->sound_requests[2]=level->sound_noise;
    if(stage->bosses_ready) {step_bosses(stage,player_x,player_y);return;}
    if (level->life_state == LEVEL_ALIVE || level->life_state == LEVEL_DYING || level->death_arc_frame) {
        ActorWorld world = {
            .context = stage, .block_at = actor_block,
            .divider_byte = stage->divider_reader ? divider_byte : NULL,
            .camera_x = (uint16_t)level->camera_x, .camera_y = (uint16_t)level->camera_y,
            .player_x = (uint16_t)level->x, .player_y = (uint16_t)level->y,
            .scroll_x = (uint8_t)(level->camera_x - 80), .scroll_y = (uint8_t)(level->camera_y - 72 - level->background_shake),
            .player_transition = level->life_state != LEVEL_ALIVE ? 255 : level->pending_damage ? 15 : 0,
            .player_goal_walk = level->goal_walk, .player_powered = level->power != LEVEL_SMALL,
            .player_crouching = level->crouching,
            .player_spin = level->spinning,
            .player_state = level->pipe,
            .player_speed_index = level->speed_index,
            .player_facing = (uint8_t)level->facing,
            .checkpoint_collected = level->checkpoint, .run_held = (held & 32) != 0,
            .current_sound = stage->sound_requests[0],
        };
        ActorEvents events;
        actors_step(&stage->actors, &world, &events);
        for (unsigned i = 0; i < events.count; ++i) {
            const ActorEvent *event = &events.items[i];
            if (event->kind == ACTOR_EVENT_SOUND && event->sound_channel < 3)
                stage->sound_requests[event->sound_channel] = event->value;
            else if (event->kind == ACTOR_EVENT_UNSUPPORTED) stage->unsupported_types[event->value] = true;
            else if (event->kind == ACTOR_EVENT_ERROR) stage->error = true;
            else if (event->kind == ACTOR_EVENT_SPECIAL_REMOVED) {
                level_room_enemy_removed(level);
            }
            else if (event->kind == ACTOR_EVENT_PLAYER_MOVE)
                level_apply_actor_movement(level,event->delta_x,event->delta_y,event->camera_right,event->camera_left);
        }
        if (events.overflow) stage->error = true;
    }
    if(!level->room_script_started_this_frame) append_actors(stage);
}

void stage_render(const Stage *stage, uint8_t fb[160 * 144]) {
    level_render_scene(&stage->level, &stage->sprites, stage->actors.defeated_count, stage->level.palettes, fb);
}
