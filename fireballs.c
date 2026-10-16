#include "fireballs.h"
#include "contacts.h"
#include <string.h>

static void throw_ball(Fireballs *fireballs,Level *level,uint8_t pressed) {
    /* 00:32C1..331B. A newly pressed B can fill only the first free slot. */
    if(!(pressed&32)||level->power!=LEVEL_FIRE||level->crouching||level->goal_walk) return;
    for(unsigned i=0;i<2;++i) if(!fireballs->slots[i].active) {
        fireballs->slots[i]=(Fireball){.active=true,
            .x=(uint16_t)(level->x+16),.y=(uint16_t)(level->y+28),
            .direction=(int8_t)(level->spinning?((int)(level->frame&2)-1):level->facing)};
        level->fire_pose=8;
        level->sound_request=11;
        return;
    }
}

static void extinguish(Fireballs *fireballs,Fireball *ball,Level *level,uint16_t x,uint16_t y,uint8_t block) {
    /* 00:3415 leaves the old stored position intact when the slot dies. */
    if(block==6&&level_fire_break_block(level,x,y)) fireballs->sound_requests[2]=1;
    ball->active=false;
    level->sound_request=2;
}

static void advance(Fireballs *fireballs,Fireball *ball,Level *level) {
    uint16_t x=(uint16_t)(ball->x+(ball->direction<0?-3:3)),y=ball->y;
    uint8_t block=level_map_block(level,x-8,y-16);
    if(block<0x38) {extinguish(fireballs,ball,level,x,y,block);return;}
    unsigned bounce=ball->bounce;
    if(bounce>=24) {fireballs->error=true;return;}
    unsigned amount=level->assets[0x3439+bounce];
    y=(uint16_t)(bounce<12?y+amount:y-amount);
    block=level_map_block(level,x-8,y-16);
    if(block<0x48&&bounce<12) {
        if(block==6) {extinguish(fireballs,ball,level,x,y,block);return;}
        bounce=24;y=(uint16_t)(y-3);
    }
    if(bounce) --bounce;
    ball->x=x;ball->y=y;ball->bounce=(uint8_t)bounce;
}

static void compose_and_contact(Fireballs *fireballs,Fireball *ball,Level *level,Actor targets[ACTOR_SLOT_COUNT]) {
    uint8_t x=(uint8_t)(ball->x-level->camera_x+80);
    uint8_t y=(uint8_t)(ball->y-level->camera_y+70);
    if((x&0xf0)==0xc0||(y&0xf0)==0xc0) {ball->active=false;return;}
    unsigned sprite=(ball->direction==-1?0xb0:0xb4)+((level->frame&6)>>1);
    if(!sprites_mario(&fireballs->sprites,level->assets,level->asset_size,sprite,y,x,false,false))
        fireballs->error=true;
    if(contacts_fireball(targets,x,y,&level->sound_request)) ball->active=false;
}

void fireballs_step(Fireballs *fireballs,Level *level,Actor targets[ACTOR_SLOT_COUNT],uint8_t pressed) {
    fireballs->stepped=true;
    memset(fireballs->sound_requests,0,sizeof(fireballs->sound_requests));
    sprites_clear(&fireballs->sprites);
    throw_ball(fireballs,level,pressed);
    for(unsigned i=0;i<2;++i) if(fireballs->slots[i].active) advance(fireballs,&fireballs->slots[i],level);
    for(unsigned i=0;i<2;++i) if(fireballs->slots[i].active) compose_and_contact(fireballs,&fireballs->slots[i],level,targets);
}

void fireballs_encode(const Fireballs *fireballs,uint8_t bytes[32]) {
    memset(bytes,0,32);
    for(unsigned i=0;i<2;++i) {
        const Fireball *ball=&fireballs->slots[i];uint8_t *p=bytes+16*i;
        p[0]=ball->active;p[1]=(uint8_t)ball->y;p[2]=(uint8_t)(ball->y>>8);
        p[3]=(uint8_t)ball->x;p[4]=(uint8_t)(ball->x>>8);
        p[5]=(uint8_t)ball->direction;p[6]=ball->bounce;
    }
}
