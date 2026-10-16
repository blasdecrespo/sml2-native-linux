#include "game.h"
#include "screens.h"
#include "stage.h"
#include "world.h"
#include "demo.h"
#include "audio_driver.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"

enum GamePhase { GAME_MENU, GAME_STAGE, GAME_WORLD };
/* Zero means no pending handler; source scene0 therefore needs a sentinel. */
enum { GAME_TITLE_LOADING=0xff };

struct Game {
    ScreenState screens;
    Stage stage;
    World world;
    uint64_t frames;
    uint8_t previous_buttons, controller_buttons;
    uint8_t sound_requests[3];
    AudioDriver audio;
    enum GamePhase phase;
    Demo demo;
    bool demo_active;
    uint8_t next_demo;
    /* Main-loop scene handlers run between the visible menu/map and Stage.
     * A298 survives these handoffs independently of the resettable FF97. */
    uint8_t entry_scene, scene_animation_clock;
    uint8_t entry_timer;
    bool easy_card_visible;
    SpriteList entry_sprites;
    bool world_return_pending, world_file_pending;
    enum WorldReturn world_return_result;
    uint8_t save_image[GAME_SAVE_SIZE];
    uint64_t save_revision;
    uint8_t (*timer_reader)(void *context);
};

static void queue_level_audio(Game *game) {
    Level *level=&game->stage.level;
    if(level->music_request)game->audio.music_request=level->music_request;
    if(level->pause_request)game->audio.pause_request=level->pause_request;
    level->music_request=level->pause_request=0;
    game->audio.work.volume_duck=level->volume_duck;
    game->audio.player_star_chain=(uint8_t)((level->star_chain/10<<4)|(level->star_chain%10));
}
static void world_audio_event(Game *game,const WorldAudioEvent *event) {
    if(event->music_request)game->audio.music_request=event->music_request;
    memcpy(game->audio.request,event->sound_requests,3);
    audio_driver_step(&game->audio);
}
static void queue_world_music(Game *game) {
    if(game->world.music_request)game->audio.music_request=game->world.music_request;
    game->world.music_request=0;
}
static void queue_world_audio(Game *game) {
    World *world=&game->world;
    /* Global13 skips main2089. Ending actions5..19 own a 2A96 call before
     * their new requests; those requests remain pending for the next action. */
    if(world->audio_clock==WORLD_AUDIO_BEFORE_REQUESTS)audio_driver_step(&game->audio);
    unsigned retained=0;
    for(unsigned i=0;i<world->audio_event_count;++i) {
        if(world->audio_events[i].after_tick)world->audio_events[retained++]=world->audio_events[i];
        else world_audio_event(game,&world->audio_events[i]);
    }
    world->audio_event_count=(uint8_t)retained;
    if(!retained&&world->audio_clock!=WORLD_AUDIO_WAIT_THEN_MAIN)queue_world_music(game);
}
static bool stage_audio_in_vblank(const Game *game) {
    const Level *level=&game->stage.level;
    return game->phase==GAME_STAGE&&!game->entry_scene&&!level->paused&&
        (level->life_state==LEVEL_ALIVE||level->life_state==LEVEL_DYING||level->exit!=LEVEL_EXIT_NONE);
}
static void finish_audio(Game *game) {
    /* 00:2089 suppresses the driver only for the original attract demos.
     * Requests are events: consuming a repeated ID must restart that song. */
    if(game->demo_active) {
        queue_level_audio(game);
        for(unsigned i=0;i<3;++i)if(game->sound_requests[i])game->audio.request[i]=game->sound_requests[i];
        return;
    }
    if(game->phase==GAME_STAGE)queue_level_audio(game);
    else if(game->phase==GAME_WORLD)queue_world_audio(game);
    else if(game->screens.music_request) {
        game->audio.music_request=game->screens.music_request;
        game->screens.music_request=0;
    }
    bool after_world_tick=game->phase==GAME_WORLD&&game->world.audio_event_count!=0;
    bool final_world_wait=game->phase==GAME_WORLD&&game->world.audio_clock==WORLD_AUDIO_WAIT_THEN_MAIN;
    bool skip_main=game->phase==GAME_WORLD&&(game->world.audio_clock==WORLD_AUDIO_SKIP||
                                            game->world.audio_clock==WORLD_AUDIO_BEFORE_REQUESTS);
    if(!after_world_tick&&!final_world_wait) {
        /* A frame without a producer leaves the source queue byte intact,
         * including requests waiting across an unticked fade entry. */
        for(unsigned i=0;i<3;++i)if(game->sound_requests[i])game->audio.request[i]=game->sound_requests[i];
    }
    /* Main0280 skips2089 in scenes4/18; their next ISR01C5 consumes these
     * newly queued requests before the next player update. */
    if(!stage_audio_in_vblank(game)&&!skip_main)audio_driver_step(&game->audio);
    if(game->phase==GAME_WORLD) {
        for(unsigned i=0;i<game->world.audio_event_count;++i)world_audio_event(game,&game->world.audio_events[i]);
        game->world.audio_event_count=0;
        if(after_world_tick||final_world_wait) {
            queue_world_music(game);
            memcpy(game->audio.request,game->sound_requests,3);
        }
        /* Returning from the final69EC wait also reaches the main2089 call.
         * Resumed scene requests belong between those two driver calls. */
        if(final_world_wait)audio_driver_step(&game->audio);
    }
    if(game->audio.error)game->stage.error=true;
}

static void update_save(Game *game) {
    if(memcmp(game->save_image,game->screens.file_records,sizeof(game->screens.file_records))) {
        screens_export_records(&game->screens,game->save_image);
        ++game->save_revision;
    }
}
static void copy_world_save(Game *game) {
    if(game->world.save_requested)
        memcpy(game->screens.file_records[game->screens.selected_file],
               game->world.save_record,sizeof(game->world.save_record));
}

static bool load_file_world(Game *game) {
    const uint8_t *record=game->screens.file_records[game->screens.selected_file];
    uint8_t frame=game->world.global_frame;
    if(!world_init_with_clock(&game->world,game_assets,sizeof(game_assets),record,
                             game->stage.divider_reader,game->timer_reader,
                             game->stage.divider_context)) return false;
    game->world.global_frame=frame;
    game->stage.actors.defeated_count=record[0x44]; /* 00:2934 / A28D */
    game->stage.level.coins=game->world.coins;
    game->stage.level.lives=game->world.lives;
    game->stage.level.power=LEVEL_SMALL;
    return true;
}

static bool begin_file(Game *game, bool skip_world) {
    game->demo_active=false;
    game->world.global_frame=game->screens.frame_counter;
    game->stage.level.animation=game->screens.animation_timer;
    if(skip_world) {
        if(!load_file_world(game))return false;
        /* The developer shortcut omits these map handlers entirely. Their
         * pending requests must not survive into a later real map return. */
        game->world.audio_event_count=0;game->world.music_request=0;
        memset(game->world.sound_requests,0,sizeof(game->world.sound_requests));
        if(!stage_load(&game->stage,0,game->screens.easy_mode,false)) return false;
        game->stage.level.frame=game->world.global_frame;
        game->stage.level.asset_animation_clock=game->scene_animation_clock;
        game->phase=GAME_STAGE;
    } else {
        game->world_file_pending=true;
        game->entry_scene=0x15; /* 0C:41C6 -> scene21 ->20 ->12. */
    }
    return true;
}

static bool initialize_title(Game *game,bool cold) {
    uint8_t (*divider)(void *)=game->stage.divider_reader;
    void *timer_context=game->stage.divider_context;
    /* 00:029F/38CE stop voices before jumping to01E5. The bootstrap clears
     * volatile game/driver state; SRAM records and A0F0's demo index survive. */
    if(cold) {
        if(!audio_driver_init(&game->audio,game_assets,sizeof(game_assets),NULL,NULL))return false;
    } else {
        audio_driver_stop_music(&game->audio);
        audio_driver_reset(&game->audio);
    }
    if(!screens_init(&game->screens,game_assets,sizeof(game_assets))||
       !stage_init(&game->stage,game_assets,sizeof(game_assets),false))return false;
    if(!cold&&!screens_import_save(&game->screens,game->save_image,sizeof(game->save_image)))return false;
    game->stage.divider_reader=divider;game->stage.divider_context=timer_context;
    memset(&game->world,0,sizeof(game->world));
    memset(&game->demo,0,sizeof(game->demo));
    memset(game->sound_requests,0,sizeof(game->sound_requests));
    game->phase=GAME_MENU;game->demo_active=false;
    game->entry_scene=game->scene_animation_clock=0;
    game->entry_timer=0;game->easy_card_visible=false;
    sprites_clear(&game->entry_sprites);
    game->world_return_pending=game->world_file_pending=false;
    game->previous_buttons=game->controller_buttons=0;
    /* The completed title loader includes its main028A audio call. */
    game->audio.music_request=game->screens.music_request;game->screens.music_request=0;
    audio_driver_step(&game->audio);
    update_save(game);
    return true;
}
static bool restart_title(Game *game) {return initialize_title(game,false);}

static bool begin_demo(Game *game) {
    if(!demo_init(&game->demo,game_assets,sizeof(game_assets),game->next_demo)||
       !stage_load(&game->stage,game->demo.level,false,false)) return false;
    game->stage.level.coins=game->stage.level.lives=0;
    game->stage.level.power=LEVEL_SMALL;
    game->stage.actors.defeated_count=0;
    /* 05:40D6 resets FF97. Three controller reads occur in scenes0B,2,3
     * before the first scene4 player update. Loading itself is synchronous
     * in the native port; these are game-loop steps, not hardware delays. */
    uint8_t held,pressed;
    for(unsigned i=0;i<3;++i) if(!demo_step(&game->demo,&held,&pressed)) return false;
    game->stage.level.frame=3;
    game->scene_animation_clock=(uint8_t)(game->scene_animation_clock+3);
    game->stage.level.asset_animation_clock=game->scene_animation_clock;
    game->stage.level.effective_held=held;
    game->stage.level.music_request=0; /* Demo load00:0592 skips the song. */
    game->audio.music_request=0xff; /* 05:40FB remains queued while2089 skips demos. */
    game->demo_active=true;game->phase=GAME_STAGE;
    return true;
}

static void transfer_stage_progress(Game *game) {
    game->world.coins=game->stage.level.coins;
    game->world.lives=game->stage.level.lives;
    game->world.powerup=game->stage.level.power;
    game->world.record[0x44]=game->stage.actors.defeated_count;
    game->world.global_frame=(uint8_t)game->stage.level.frame;
    game->scene_animation_clock=game->stage.level.asset_animation_clock;
}

static void step_entry(Game *game,uint8_t held) {
    /* 00:02FF increments both clocks once per scene handler. LCD-off
     * decoding can span display intervals without advancing either clock. */
    ++game->world.global_frame;
    ++game->scene_animation_clock;
    switch(game->entry_scene) {
        case GAME_TITLE_LOADING:
            if(!screens_init(&game->screens,game_assets,sizeof(game_assets))||
               !screens_import_save(&game->screens,game->save_image,sizeof(game->save_image)))
                game->stage.error=true;
            game->screens.frame_counter=game->world.global_frame;
            game->entry_scene=0;game->phase=GAME_MENU;
            break;
        case 5:
            game->world.sprite_palette[0]=game->stage.level.palettes[1];
            game->world.sprite_palette[1]=game->stage.level.palettes[2];
            if(world_game_over(&game->world)) {
                game->stage.actors.defeated_count=0;
                copy_world_save(game);
            } else game->stage.error=true;
            game->entry_scene=0;game->phase=GAME_WORLD;
            break;
        case 0x1d:
            if(game->world.award.timer)--game->world.award.timer;
            if(!world_boss_prepare(&game->world,(uint8_t)game->stage.level.id))game->stage.error=true;
            /* 0C:45B6 enters0348 after LY145. Restoring VBlank IE after
             * its next LY145 wait services one queued0154 countdown,
             * while FF97/A298 remain at this same scene update. */
            if(game->world.award.timer)--game->world.award.timer;
            game->entry_scene=0x1e;
            break;
        case 0x1e:
            if(game->world.award.timer)--game->world.award.timer;
            if(!world_boss_clear(&game->world,(uint8_t)game->stage.level.id))game->stage.error=true;
            game->entry_scene=0;
            break;
        case 0x15:game->entry_scene=0x14;break;
        case 0x14:
            if(game->world_file_pending) {
                game->world_file_pending=false;
                if(!load_file_world(game))game->stage.error=true;
            } else if(game->world_return_pending) {
                game->world_return_pending=false;
                if(game->world_return_result==WORLD_EXISTING) {
                    if(!world_return(&game->world,WORLD_EXISTING))game->stage.error=true;
                } else {
                    world_inherit_metatiles(&game->world,(const uint8_t*)game->stage.level.metatiles);
                    if(!world_finish_stage(&game->world,game->world_return_result,
                                          game->stage.level.goal_walk,game->stage.level.time))
                        game->stage.error=true;
                }
            }
            copy_world_save(game);
            game->entry_scene=0;game->phase=GAME_WORLD;break;
        case 0x16:
            /* 00:3572 dispatches the castle before the easy-mode label. */
            if(game->world.stage==0xff){game->entry_scene=0x1b;break;}
            /* 00:357F/358B presents the easy-mode label before every stage.
             * LCDC E2 disables the background; the label is one metasprite. */
            if(game->screens.easy_mode) {
                memcpy(game->world.tiles+0x800,game_assets+0x1a*0x4000+0x656,0x300);
                sprites_clear(&game->entry_sprites);
                game->entry_timer=64;game->easy_card_visible=true;game->entry_scene=0x17;
            } else game->entry_scene=0x0b;
            break;
        case 0x1b:
            if(!world_castle_prepare(&game->world))game->stage.error=true;
            game->entry_scene=0;
            break;
        case 0x17:
            sprites_clear(&game->entry_sprites);
            if(!sprites_mario(&game->entry_sprites,game_assets,sizeof(game_assets),0xf0,0x50,0x34,false,false))
                game->stage.error=true;
            if(!--game->entry_timer)game->entry_scene=0x0b;
            break;
        case 0x0b:
            game->easy_card_visible=false;
            sprites_clear(&game->entry_sprites);
            game->entry_scene=2;break;
        case 2:game->entry_scene=3;break;
        case 3: {
            if(!stage_load(&game->stage,game->world.stage,game->screens.easy_mode,
                           (game->world.stage_flags&8)!=0)) {
                game->stage.error=true;game->entry_scene=0;return;
            }
            game->stage.level.coins=game->world.coins;
            game->stage.level.lives=game->world.lives;
            game->stage.level.power=(LevelPower)game->world.powerup;
            game->stage.level.stage_cleared=(game->world.stage_flags&0x80)!=0;
            game->stage.level.frame=game->world.global_frame;
            game->stage.level.asset_animation_clock=game->scene_animation_clock;
            game->stage.level.effective_held=held;
            game->easy_card_visible=false;
            game->entry_scene=0;game->phase=GAME_STAGE;
            break;
        }
        default:game->stage.error=true;game->entry_scene=0;break;
    }
}

static void return_to_world(Game *game, enum WorldReturn result) {
    transfer_stage_progress(game);
    /* 00:356E/3127 only request scene20. Its next main-loop handler owns
     * map initialization, DIV-driven objects, progress and the save write. */
    game->world_return_pending=true;
    game->world_return_result=result;
    game->phase=GAME_WORLD;
    game->audio.work.hurry=0; /* 00:277C, returning to the overworld. */
    game->entry_scene=0x14;
}

Game *game_create(bool start_in_level) {
    Game *game=calloc(1,sizeof(*game));
    if(!game) return NULL;
    if(!initialize_title(game,true)) {
        free(game);return NULL;
    }
    if(start_in_level&&!begin_file(game,true)) { free(game);return NULL; }
    if(start_in_level)queue_level_audio(game);
    update_save(game);
    return game;
}

void game_destroy(Game *game) { free(game); }

void game_set_audio_output(Game *game,AudioRegisterWrite emit,void *context) {
    if(!game||game->frames)return;
    game->audio.emit=emit;game->audio.emit_context=context;
    if(emit) {
        static const uint8_t initial_writes[]={0x26,0x24,0x25,0x25,0x12,0x17,0x21,0x14,0x19,0x23,0x10,0x1a};
        for(unsigned i=0;i<sizeof(initial_writes);++i)
            emit(context,initial_writes[i],game->audio.registers[initial_writes[i]-0x10]);
    }
}
const AudioDriver *game_audio_state(const Game *game) {return game?&game->audio:NULL;}

bool game_import_save(Game *game,const uint8_t *bytes,size_t size) {
    if(!game||game->frames||!bytes||size!=GAME_SAVE_SIZE) return false;
    if(!screens_import_save(&game->screens,bytes,size)) return false;
    memcpy(game->save_image,bytes,size);
    update_save(game);
    if(game->phase==GAME_STAGE&&!begin_file(game,true)) return false;
    return true;
}

void game_export_save(const Game *game,uint8_t bytes[GAME_SAVE_SIZE]) {
    memcpy(bytes,game->save_image,GAME_SAVE_SIZE);
}

uint64_t game_save_revision(const Game *game) { return game->save_revision; }

void game_set_timer_readers(Game *game,uint8_t (*divider)(void *context),
                            uint8_t (*timer)(void *context),void *context) {
    game->timer_reader=timer;
    game->stage.divider_reader=divider;
    game->stage.divider_context=context;
    game->world.read_divider=divider;
    game->world.read_timer=timer;
    game->world.divider_context=context;
}

bool game_paused(const Game *game) { return game->phase==GAME_STAGE&&game->stage.level.paused; }

static void step_game(Game *game,uint8_t held_buttons) {
    uint8_t previous_buttons=game->previous_buttons;
    uint8_t pressed=held_buttons&(uint8_t)~game->previous_buttons;
    game->previous_buttons=held_buttons;
    game->frames++;
    memset(game->sound_requests,0,sizeof(game->sound_requests));
    if(!game->demo_active&&stage_audio_in_vblank(game))audio_driver_step(&game->audio);
    game->stage.level.current_music=game->audio.music;
    game->stage.level.volume_duck=game->audio.work.volume_duck!=0;
    if(game->entry_scene) {
        game->world.audio_clock=WORLD_AUDIO_MAIN;
        step_entry(game,held_buttons);
        update_save(game);
        finish_audio(game);
        return;
    }
    if(game->phase==GAME_STAGE) {
        if(game->demo_active&&!game->stage.level.room_transition) {
            if((pressed&BUTTON_START)||!demo_step(&game->demo,&held_buttons,&pressed)) {
                game->next_demo=(uint8_t)((game->demo.index+1)&3);
                if(!restart_title(game)) game->stage.error=true;
                return;
            }
        }
        stage_step(&game->stage,held_buttons,pressed);
        memcpy(game->sound_requests,game->stage.sound_requests,sizeof(game->sound_requests));
        if(!game->demo_active)queue_level_audio(game);
        Level *level=&game->stage.level;
        if(game->demo_active&&(level->complete||level->life_state==LEVEL_DEAD)) {
            game->next_demo=(uint8_t)((game->demo.index+1)&3);
            if(!restart_title(game)) game->stage.error=true;
            return;
        } else if(level->return_requested) {
            level->return_requested=false;
            return_to_world(game,WORLD_FAILED);
        } else if(game->stage.scene_request==0x1d) {
            transfer_stage_progress(game);
            game->world.award.timer=(uint16_t)level->input_wait;
            game->entry_scene=0x1d;game->phase=GAME_WORLD;
        } else if(level->ending_requested) {
            transfer_stage_progress(game);
            if(world_ending(&game->world)) {
                game->world.ending.sprite_clock=level->animation;
                game->world.ending.timer=(uint16_t)level->input_wait;
                game->world.background_palette=level->palettes[0];
                game->world.sprite_palette[0]=level->palettes[1];
                game->world.sprite_palette[1]=level->palettes[2];
                game->phase=GAME_WORLD;
            } else game->stage.error=true;
        } else if(level->complete) {
            return_to_world(game,level->exit==LEVEL_EXIT_SECRET?WORLD_SECRET:WORLD_CLEAR);
        } else if(level->life_state==LEVEL_DEAD&&!level->game_over) {
            return_to_world(game,level->checkpoint?WORLD_FAILED_CHECKPOINT:WORLD_FAILED);
        } else if(level->life_state==LEVEL_DEAD&&level->game_over) {
            transfer_stage_progress(game);
            game->entry_scene=5;game->phase=GAME_WORLD; /* 00:314B requests the next handler. */
        }
    } else if(game->phase==GAME_WORLD) {
        uint8_t previous_clock=game->world.global_frame;
        enum WorldScene previous_scene=game->world.scene;
        world_step(&game->world,held_buttons,pressed);
        if(previous_scene==WORLD_COIN_AWARD&&game->world.scene!=WORLD_COIN_AWARD)
            game->stage.level.animation=game->world.award.animation;
        /* 0F:69EC waits and copies sprites without polling00:1FD2. */
        if(game->world.global_frame==previous_clock)game->previous_buttons=previous_buttons;
        game->scene_animation_clock=(uint8_t)(game->scene_animation_clock+
            (uint8_t)(game->world.global_frame-previous_clock));
        memcpy(game->sound_requests,game->world.sound_requests,sizeof(game->sound_requests));
        copy_world_save(game);
        if(game->world.return_requested) {
            /* Coin animation and castle departure request20 after their
             * final OAM pass. Its next handler owns map setup and saving. */
            game->world.return_requested=false;
            if(previous_scene==WORLD_COIN_AWARD)
                game->stage.level.animation=game->world.award.animation;
            game->world_return_pending=true;
            game->world_return_result=game->world.return_result;
            game->audio.work.hurry=0;
            game->entry_scene=0x14;
        } else if(game->world.title_requested) {
            update_save(game);
            game->entry_scene=GAME_TITLE_LOADING; /* 00:2898 requests scene0. */
        } else if(game->world.stage_requested) {
            game->entry_scene=0x16; /* 0F:41EC ->22 ->11 ->2 ->3 ->4. */
        }
    } else {
        screens_tick(&game->screens,held_buttons);
        ++game->scene_animation_clock;
        game->sound_requests[0]=game->screens.sound_request;
        if(game->screens.gameplay_requested) {
            if(!begin_file(game,false)) game->stage.error=true;
        } else if(game->screens.demo_requested) {
            if(!begin_demo(game)) game->stage.error=true;
        }
    }
    update_save(game);
    finish_audio(game);
}

static bool world_waiting(const Game *game) {
    return game->phase==GAME_WORLD&&!game->entry_scene&&
        (game->world.scene==WORLD_FADE||game->world.scene==WORLD_ZONE_ENTRY||game->world.scene==WORLD_ZONE_EXIT);
}

void game_step(Game *game,uint8_t held_buttons) {
    bool stage=game->phase==GAME_STAGE&&!game->entry_scene;
    bool demo=game->demo_active;
    Level *level=&game->stage.level;
    bool stage_wait=stage&&(level->room_script||level->room_transition||level->scene_loading_frames);
    if(!world_waiting(game)&&!stage_wait)game->controller_buttons=held_buttons;
    bool exit_wait=stage&&level->exit!=LEVEL_EXIT_NONE;
    step_game(game,held_buttons);
    /* 00:028D tests the final FF80 after the scene handler and room script.
     * Blocked scene continuations have not reached that main-loop boundary.
     * Demo Start owns its separate reset/index-advance path at00:38CE. */
    if(demo||world_waiting(game)||
       (stage&&(level->room_script||level->scene_loading_frames)))return;
    if(stage&&!stage_wait&&!exit_wait)game->controller_buttons=level->effective_held;
    if((game->controller_buttons&0xf0)==0xf0&&!restart_title(game))game->stage.error=true;
}

uint8_t game_music(const Game *game) {
    return game->audio.music;
}

uint8_t game_sound_request(const Game *game,unsigned channel) {
    return channel<3&&!game->demo_active?game->sound_requests[channel]:0;
}

void game_render(const Game *game,uint8_t shades[GAME_WIDTH*GAME_HEIGHT]) {
    if(game->easy_card_visible) {
        uint8_t colors[GAME_WIDTH*GAME_HEIGHT]={0};
        memset(shades,0,GAME_WIDTH*GAME_HEIGHT);
        sprites_render(&game->entry_sprites,game->world.tiles,colors,shades,0xd2,0x39);
    }
    else if(game->phase==GAME_STAGE||game->entry_scene==5||game->entry_scene==0x1d) stage_render(&game->stage,shades);
    else if(game->phase==GAME_WORLD&&game->world.scene==WORLD_ENDING&&
            (game->world.ending.phase<2||game->world.scene_action<5)) {
        /* Global23 fades the retained final gameplay image. The ending
         * replaces its background/tile layout only at1A action4. */
        uint8_t palettes[]={game->world.background_palette,game->world.sprite_palette[0],game->world.sprite_palette[1]};
        level_render_scene(&game->stage.level,&game->stage.sprites,game->stage.actors.defeated_count,palettes,shades);
    }
    else if(game->phase==GAME_WORLD) world_render(&game->world,shades);
    else screens_render(&game->screens,shades);
}

void game_write_state(const Game *g,FILE *out) {
    const Level *level=&g->stage.level;
    fprintf(out,"{\n  \"phase\": \"%s\",\n  \"frames\": %" PRIu64 ",\n",
        g->phase==GAME_STAGE?"level":g->phase==GAME_WORLD?"world":screens_phase_name(&g->screens),g->frames);
    fprintf(out,"  \"menu\": {\"selected_file\": %u, \"action\": %u, \"x\": %u, \"y\": %u, \"easy_mode\": %u, \"erase_mode\": %u, \"demo_requested\": %u},\n",
        g->screens.selected_file,g->screens.menu_action,g->screens.mario_x,g->screens.mario_y,
        g->screens.easy_mode,g->screens.erase_mode,g->screens.demo_requested);
    fprintf(out,"  \"demo\": {\"active\": %s, \"index\": %u, \"cursor\": %u, \"error\": %s},\n",
        g->demo_active?"true":"false",g->demo.index,g->demo.cursor,g->demo.error?"true":"false");
    fprintf(out,"  \"level\": {\"id\": %u, \"frames\": %u, \"x\": %d, \"y\": %d, \"camera_x\": %d, \"camera_y\": %d, \"speed_index\": %u, \"jump_index\": %u, \"grounded\": %s, \"coins\": %u, \"lives\": %u, \"time\": %u, \"paused\": %s, \"power\": %u, \"life_state\": %u},\n",
        level->id,level->frame,level->x,level->y,level->camera_x,level->camera_y,
        level->speed_index,level->jump_index,level->grounded?"true":"false",
        level->coins,level->lives,level->time,level->paused?"true":"false",level->power,level->life_state);
    fprintf(out,"  \"room\": {\"transition\": %u, \"script\": %u, \"loading\": %u, \"pipe\": %u, \"input_wait\": %u},\n",
        level->room_transition,level->room_script,level->scene_loading_frames,level->pipe,level->input_wait);
    fprintf(out,"  \"boss\": {\"active\": %s, \"mode\": %u, \"wario_phase\": %u, \"defeated\": %s, \"scene_request\": %u},\n",
        g->stage.bosses_ready?"true":"false",level->boss_mode,level->wario_phase,
        level->boss_defeated?"true":"false",g->stage.scene_request);
    fprintf(out,"  \"ending\": {\"requested\": %s, \"phase\": %u, \"finished\": %s},\n",
        level->ending_requested?"true":"false",g->world.ending.phase,g->world.ending.finished?"true":"false");
    fprintf(out,"  \"audio\": {\"current_music\": %u, \"paused\": %s, \"hurry\": %s, \"error\": %s},\n",
        g->audio.music,g->audio.paused?"true":"false",g->audio.work.hurry?"true":"false",g->audio.error?"true":"false");
    fprintf(out,"  \"actors\": {\"active\": %u, \"dormant\": %u, \"error\": %s, \"unsupported_types\": [",
        g->stage.actors.active_count,g->stage.actors.dormant_count,g->stage.error?"true":"false");
    bool comma=false;
    for(unsigned i=0;i<256;++i) if(g->stage.unsupported_types[i]) {
        fprintf(out,"%s%u",comma?",":"",i);comma=true;
    }
    fprintf(out,"]},\n  \"world\": {\"node\": %u, \"region\": %u, \"scene\": %u, \"moving\": %s, \"stage\": %u, \"golden_coins\": %u, \"cleared_stages\": %u, \"asset_error\": %s}\n}\n",
        g->world.node,g->world.region,g->world.scene,g->world.moving?"true":"false",g->world.stage,
        world_golden_coins(&g->world),(g->world.record[0x42]>>4)*10+(g->world.record[0x42]&15),g->world.asset_error?"true":"false");
}
