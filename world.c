/* Original world navigation, castle gate and bonus machine translated from
 * banks 0C/0F/18. Routes, coordinates, stage IDs and graphics remain ROM assets.
 */
#include "world.h"
#include "sprites.h"
#include <string.h>

static const uint8_t *asset(const World *w,unsigned bank,unsigned address) {
    return w->rom+bank*0x4000u+(address&0x3fff);
}
static void save_now(World *w) {
    /* 00:2934 captures the record at the call, before later cursor remaps. */
    world_export_record(w,w->save_record);w->save_requested=true;
}
static void audio_immediate(World *w) {
    /* Source 00:2AAA consumes these queues immediately, independently of the
     * ordinary frame tick. Callers retain order, including same-ID restarts. */
    if(w->audio_event_count>=2){w->asset_error=true;return;}
    WorldAudioEvent *event=&w->audio_events[w->audio_event_count++];
    event->music_request=w->music_request;memcpy(event->sound_requests,w->sound_requests,3);event->after_tick=false;
    w->music_request=0;memset(w->sound_requests,0,3);
}
static void begin_fade(World *w,enum WorldFade continuation) {
    w->scene=WORLD_FADE;w->fade=continuation;w->scene_timer=0;
    w->audio_clock=WORLD_AUDIO_SKIP; /* Handler is suspended before main2089. */
}
static uint16_t word(const World *w,unsigned bank,unsigned address) {
    const uint8_t *p=asset(w,bank,address);return (uint16_t)(p[0]|p[1]<<8);
}
static uint8_t sample_divider(World *w) {
    if(w->read_divider)return w->read_divider(w->divider_context);
    return (uint8_t)((w->frames*70224u+w->entropy_samples++*128u)>>8);
}
static uint8_t mix_random(World *w) {
    uint8_t divider=sample_divider(w);
    w->random_state=(uint8_t)(w->random_state+divider+(uint8_t)((divider<<4)|(divider>>4))+w->global_frame);
    return w->random_state;
}
bool world_node_stage(const World *w,uint8_t node,uint8_t *stage) {
    if(node>=WORLD_NODE_COUNT)return false;
    uint8_t value=*asset(w,15,0x4218+node);
    if(value==0xfe)return false;
    if(stage)*stage=value;
    return true;
}
bool world_node_progress_slot(const World *w,uint8_t node,uint8_t *slot) {
    if(node>=WORLD_NODE_COUNT)return false;
    uint8_t value=*asset(w,15,0x650e + node);
    if(value>=WORLD_PROGRESS_COUNT)return false;
    if(slot)*slot=value;
    return true;
}
unsigned world_golden_coins(const World *w) {
    /* 00:2800: these six progress slots are the coin owners. */
    static const uint8_t owners[]={5,9,0x0e,0x11,0x17,0x1d};
    unsigned count=0;
    for(unsigned i=0;i<sizeof(owners);i++)count+=(w->progress[owners[i]]&0x80)!=0;
    return count;
}
static void node_coordinates(World *w) {
    /* 0F:6EA4, bank18:5EE6: big-endian cameras, then cursor X/Y. */
    const uint8_t *p=asset(w,24,0x5ee6+w->node*8);
    w->camera_x=(uint16_t)(p[0]*256+p[1]);w->camera_y=(uint16_t)(p[2]*256+p[3]);
    w->x=p[4];w->y=p[5];
}
static unsigned node_region(unsigned node) {
    if(node<0x1e)return 0;
    if(node<0x28)return 2;
    if(node<0x32)return 1;
    if(node<0x3c)return 3;
    if(node<0x46)return 4;
    if(node<0x50)return 5;
    if(node<0x5a)return 6;
    if(node<0x64)return 7;
    if(node>=0x6e&&node<0x78)return 8;
    if(node==0x6a||node==0x6b||node==0x6c)return 3;
    if(node==0x78||node==0x79||node==0x7a)return 1;
    return 0;
}
static void append_sprite(World *w,unsigned x,unsigned y,unsigned tile,unsigned flags) {
    if(w->sprite_count>=40)return;
    uint8_t *s=w->sprites+w->sprite_count++*4;
    s[0]=(uint8_t)y;s[1]=(uint8_t)x;s[2]=(uint8_t)tile;s[3]=(uint8_t)flags;
}
static void metasprite(World *w,unsigned bank,unsigned table,unsigned id,int x,int y) {
    unsigned address=word(w,bank,table+id*2);
    if(address<0x4000||address>0x7ffc){w->asset_error=true;return;}
    for(unsigned i=0;i<40&&address<=0x7ffc;i++,address+=4) {
        const uint8_t *p=asset(w,bank,address);
        if(p[2]==0xff)return;
        append_sprite(w,(unsigned)(x+(int8_t)p[1]),(unsigned)(y+(int8_t)p[0]),p[2],p[3]);
    }
}
static void player_sprite_palette(World *w,unsigned id,unsigned x,unsigned y,bool front,bool alternate_palette) {
    /* 01:5297 has an 80 Y terminator, unlike map objects' FF tile. */
    unsigned at=word(w,1,0x4000+id*2);
    for(unsigned count=0;count<40&&at<=0x7ffc;count++,at+=4) {
        const uint8_t *p=asset(w,1,at);if(p[0]==0x80)return;
        append_sprite(w,x+p[1],y+p[0],p[2],p[3]|(front?0:0x80)|(alternate_palette?0x10:0));
    }
    w->asset_error=true;
}
static void player_sprite(World *w,unsigned id,unsigned x,unsigned y,bool front) {
    player_sprite_palette(w,id,x,y,front,false);
}
static void apply_overlay_table(World *w,unsigned table) {
    /* 18:50DD/5147: dx,dy,tile triples share a format and distinct tables. */
    for(unsigned i=0;i<15;i++) {
        const uint8_t *o=w->overlays[i];if(o[2]==0xff)continue;
        unsigned at=word(w,24,table+o[2]*2);
        for(unsigned n=0;n<1024&&at<0x7ffd;n++,at+=3) {
            const uint8_t *p=asset(w,24,at);if(p[0]==0xff)break;
            unsigned x=(o[0]+p[0])&31,y=(o[1]+p[1])&31;
            w->map[y*32+x]=p[2];
        }
    }
}
static void apply_overlays(World *w) {apply_overlay_table(w,0x63ea);}
static void region_objects(World *w,unsigned address) {
    /* 0F:62A2 owns the terminated three-byte sprite and tile-patch lists. */
    memset(w->objects,0xff,sizeof(w->objects));memset(w->overlays,0xff,sizeof(w->overlays));
    unsigned i=0;
    for(;i<16;i++,address+=3) {
        const uint8_t *p=asset(w,15,address);if(p[0]==0xff)break;
        memcpy(w->objects[i],p,3);
    }
    address+=3;
    for(i=0;i<16;i++,address+=3) {
        const uint8_t *p=asset(w,15,address);if(p[0]==0xff)break;
        memcpy(w->overlays[i],p,3);
    }
    if(w->region<13)apply_overlays(w);
}
static void reveal_turtle_step(World *w) {
    /* 0F:5B12 is also the fall-through tail of the clear-marker routine. */
    if(w->decoration.secret_reveal&&!(w->frame_counter&7)&&w->overlays[0][2]<0x1f) {
        if(++w->overlays[0][2]>=0x1f){w->overlays[0][2]=0xff;w->sound_requests[0]=0x15;}
        else w->sound_requests[0]=6;
        apply_overlays(w);
    }
}
static void mark_object_clears(World *w,unsigned slot,unsigned object,unsigned count) {
    /* 0F:5B00 ends by falling through into the turtle reveal, even when
     * called during initialization. Keep that producer-owned behavior here. */
    for(unsigned i=0;i<count;i++)w->objects[object+i][2]=(w->progress[slot+i]&0x80)?0x0e:0xff;
    reveal_turtle_step(w);
}
static void rebuild_object_sprites(World *w);
static void rebuild_sprites(World *w);
static void animate_main_objects(World *w);
static void setup_return_assets(World *w,unsigned secret_slot,bool new_secret) {
    static const struct {uint8_t bank;uint16_t tiles,map,objects;uint8_t music;} region[9]={
        {13,0x4000,0x7000,0,0x06},
        {14,0x4200,0x7000,0x65de,0x1e},
        {14,0x5a00,0x7240,0x65ba,0x10},
        {13,0x5a00,0x76c0,0x662c,0x05},
        {16,0x4200,0x7480,0x65ff,0x1c},
        {14,0x5a00,0x7900,0x6680,0x12},
        {17,0x4200,0x7b40,0x664d,0x1b},
        {17,0x5a00,0x7d80,0x66aa,0x0e},
        {14,0x4200,0x7000,0x66bc,0x0d}
    };
    unsigned r=w->region;
    w->background_palette=r==7?0xe1:0xe4;w->sprite_palette[0]=0xd0;w->sprite_palette[1]=0x39;
    if(!r){w->music_request=0xff;audio_immediate(w);} /* 7156 */
    w->music_request=region[r].music;w->moving=false;w->segment_timer=8;
    memset(w->sprites,0,sizeof(w->sprites));
    if(!r) {
        w->cloud_counter=0;w->boat_counter=0;
        w->map_stride=64;
        w->object_camera_x=w->camera_x;w->object_camera_y=w->camera_y;
        memcpy(w->tiles,asset(w,13,0x4000),0x1800);
        for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++) {
            unsigned source=0x7000+(y/32)*0x800+(x/32)*0x400+(y%32)*32+x%32;
            w->map[y*64+x]=*asset(w,13,source);
        }
        memcpy(w->objects,asset(w,15,0x4bfd),72);
        animate_main_objects(w);rebuild_sprites(w); /* 4BE7 draws before 4E43 changes the coin markers. */
        w->fish_counter=0; /* 7156 resets the burst after that initial draw. */
        static const uint8_t slots[]={9,5,0x0e,0x11,0x17,0x1d};
        for(unsigned i=0;i<6;i++)w->objects[4+i][3]=(w->progress[slots[i]]&0x80)?2:0xff;
    } else {
        if(r==1){w->decoration.flight_command=0;w->decoration.secret_reveal=0;w->decoration.horizontal_x=w->decoration.horizontal_direction=0;}
        if(r==3)w->decoration.secret_reveal=0;
        if(r==2){w->decoration.orbit_step=0;w->decoration.bob_phase=16;}
        if(r==4){w->decoration.blink_timer=0;w->decoration.secret_action=0;}
        if(r==5)w->decoration.horizontal_x=0xa0;
        if(r==1||r==2)w->decoration.tile_timer=w->decoration.tile_phase=0;
        if(r==7){w->decoration.cloud_scroll[1]=0x17;w->decoration.satellite_direction=0;w->decoration.satellite_timer=1;}
        w->decoration.shake_x=w->decoration.shake_y=0;
        w->map_stride=32;
        memcpy(w->tiles,asset(w,13,0x4000),0x200);
        memcpy(w->tiles+0x200,asset(w,region[r].bank,region[r].tiles),0x1600);
        unsigned map_bank=r==8?16:14;
        memcpy(w->map,asset(w,map_bank,region[r].map),r==4?0x240:0x400);
        /* Castle's 7D80+0400 source crosses 8000 into its loaded VRAM,
         * not the next physical ROM bank. Those rows are offscreen. */
        if(r==7)memcpy(w->map+0x280,w->tiles,0x180);
        if(r==4)memset(w->map+0x300,0x7e,0xff);
        region_objects(w,region[r].objects);
        if(r==1&&(w->progress[9]&0x80))w->objects[2][2]=0xff;
        if(r==2&&(w->progress[5]&0x80))w->objects[0][2]=0xff;
        /* Region initializers own persistent secret-path artwork, separately
         * from the animation that first reveals each path. */
        if(r==1) {
            if(w->progress[7]&1) {
                for(unsigned id=0x1f;id<0x27;id++){w->overlays[0][2]=(uint8_t)id;apply_overlays(w);}
                if(w->progress[0x26]&0x80)memcpy(w->tiles+0x800,asset(w,14,0x4500),0x220);
            }
            if(w->progress[8]&1)
                for(unsigned id=0x27;id<0x2f;id++){w->overlays[0][2]=(uint8_t)id;apply_overlays(w);}
        }
        if(r==3&&(w->progress[0x0b]&1))
            for(unsigned id=0x10;id<0x1f;id++){w->overlays[0][2]=(uint8_t)id;apply_overlays(w);}
        if(r==4&&(w->progress[0x10]&1)) {
            w->overlays[0][2]=0x38;apply_overlays(w);
            w->overlays[0][2]=0x33;w->overlays[1][2]=0x34;apply_overlays(w);
            w->objects[7][2]=0x70;w->objects[9][2]=0x7b;
            w->decoration.secret_action=3;w->decoration.secret_timer=0;
            if(w->progress[0x29]&0x80){w->overlays[0][2]=0x39;apply_overlays(w);}
        }
        if(r==5&&(w->progress[0x16]&1))w->objects[10][2]=0x81;
        if(r==6) {
            w->objects[5][0]=0x4a;
            bool cleared=(w->progress[0x1d]&0x80)!=0;
            w->objects[2][0]=(uint8_t)(0x4c+(cleared?16:0));w->objects[2][1]=(uint8_t)(0x10+(cleared?3:0));
            w->overlays[2][2]=(uint8_t)(6+((w->progress[0x1b]&0x80)?2:0));
            w->overlays[3][2]=(uint8_t)(9+((w->progress[0x1c]&0x80)?2:0));apply_overlays(w);
        }
        if(secret_slot<WORLD_PROGRESS_COUNT) {
            if(r==1) {
                unsigned overlay=w->node==0x2c?0:1;
                w->overlays[overlay][2]=(uint8_t)(overlay?0x27:0x1f);apply_overlays(w);
                w->decoration.secret_reveal=new_secret?0xff:0;
            }
            else if(r==3){w->overlays[0][2]=0x10;w->decoration.secret_reveal=new_secret?0xff:0;}
            else if(r==4) {
                w->overlays[0][2]=0x2f;apply_overlays(w);
                if(new_secret){w->decoration.secret_action=1;w->decoration.secret_timer=60;w->objects[9][2]=0x75;}
            } else if(r==5&&new_secret)w->objects[10][2]=0x7d;
        }
        if(r==1)mark_object_clears(w,6,3,4);
        if(r==2)mark_object_clears(w,1,4,5);
        if(r==3){mark_object_clears(w,0x0b,4,4);mark_object_clears(w,0x23,0,1);}
        if(r==4){mark_object_clears(w,0x10,4,2);mark_object_clears(w,0x29,10,1);}
        if(r==5)mark_object_clears(w,0x15,4,3);
        if(r==6)mark_object_clears(w,0x1a,7,4);
        rebuild_object_sprites(w);
        /* 50FF draws OAM before 51C9/518E/51B5 reveal the tree secret.
         * That new object first appears when the animation owner redraws. */
        if(r==2&&(w->progress[2]&1))w->objects[9][2]=(w->progress[0x24]&0x80)?0x7c:0x6d;
        if(r==2&&new_secret)w->objects[9][2]=0x6a;
        w->frame_counter=0; /* A69A resets after initialization; FF97 does not. */
        audio_immediate(w);
    }
}
static void setup_assets(World *w) {setup_return_assets(w,WORLD_PROGRESS_COUNT,false);}
static void cursor_sprites(World *w) {
    /* 0F:4901/49B5: four 8x8 cursor tiles, original costume tables. */
    unsigned at=word(w,15,0x4ad7+(uint8_t)(w->pose*2)),style=w->segment>>4;
    if(style==8)at=0x4b77+(w->animation&8);
    else if(style&4)at=0x4b87;
    else if(style!=10&&(style&2)) {
        if(w->moving&&(w->segment&2))at=0x4b8f+(w->animation&12)*2;
        else if(w->moving&&(w->segment&1))at=0x4baf+(w->animation&12)*2;
        else at=0x4bcf+*asset(w,15,0x498c+((w->animation&24)>>3));
    }
    int x=w->x,y=w->y;
    if((w->segment&15)&&(w->segment&0x20)){x+=4;y+=8;}
    for(unsigned i=0;i<4;i++) {
        const uint8_t *p=at+i*2<0x4000?w->rom+at+i*2:asset(w,15,at+i*2);
        append_sprite(w,(unsigned)(x+(i&1)*8),(unsigned)(y+(i>>1)*8),p[0],p[1]);
    }
}
static void rebuild_object_sprites(World *w) {
    memset(w->sprites+16,0,sizeof(w->sprites)-16);w->sprite_count=4;
    for(unsigned i=0;i<16;i++) {
        const uint8_t *o=w->objects[i];if(o[2]==0xff)continue;
        metasprite(w,24,0x62e6,o[2],o[0],o[1]);
    }
}
static void rebuild_main_map_sprites(World *w) {
    memset(w->sprites+16,0,sizeof(w->sprites)-16);w->sprite_count=4;
    const uint8_t *hud=asset(w,15,0x7251);
    for(unsigned i=0;i<9;i++)append_sprite(w,hud[i*4+1],hud[i*4],hud[i*4+2],hud[i*4+3]);
    w->sprites[6*4+2]=(uint8_t)(0x70+w->lives/10%10);
    w->sprites[7*4+2]=(uint8_t)(0x70+w->lives%10);
    w->sprites[10*4+2]=(uint8_t)(0x70+w->coins/100%10);
    w->sprites[11*4+2]=(uint8_t)(0x70+w->coins/10%10);
    w->sprites[12*4+2]=(uint8_t)(0x70+w->coins%10);
    for(unsigned i=0;i<18;i++) {
        const uint8_t *o=w->objects[i];if(o[3]==0xff)continue;
        int x=((o[0]>>4)*256+o[1])-(int)w->object_camera_x+8;
        int y=((o[0]&15)*256+o[2])-(int)w->object_camera_y+16;
        if(x<= -40||x>216||y<= -40||y>216)continue;
        metasprite(w,15,0x4c4d,o[3],x,y);
    }
}
static void rebuild_sprites(World *w) {
    memset(w->sprites,0,sizeof(w->sprites));w->sprite_count=0;cursor_sprites(w);
    if(!w->region)rebuild_main_map_sprites(w);else rebuild_object_sprites(w);
}
static void animate_water(World *w) {
    /* 0F:6853/6929 rotates one row of the original 16-pixel water pattern.
     * Each pair lives in the tile buffer that owns its current pixels. */
    static const unsigned bases[]={0x8e0,0x9e0,0xce0,0xde0};
    unsigned phase=++w->water_counter,row=(phase&7)*2;
    for(unsigned i=0;i<4;i++)for(unsigned plane=0;plane<(i<2?1u:2u);plane++) {
        unsigned at=bases[i]+row+plane;
        uint16_t pair=(uint16_t)(w->tiles[at]|w->tiles[at+16]<<8);
        if(phase&1)pair=(uint16_t)((pair>>1)|(pair<<15));
        else pair=(uint16_t)((pair<<1)|(pair>>15));
        w->tiles[at]=(uint8_t)pair;w->tiles[at+16]=(uint8_t)(pair>>8);
    }
}
static void animate_main_objects(World *w) {
    /* 0F:4E6B: animate the six collected-coin markers and three clouds. */
    if(!(w->frame_counter&7)) {
        if(++w->coin_animation>=3)w->coin_animation=0;
        for(unsigned i=4;i<10;i++)if(w->objects[i][3]!=0xff)
            w->objects[i][3]=(uint8_t)(w->coin_animation+2);
    }
    if(!(w->frame_counter&3)) {
        ++w->cloud_counter;
        for(unsigned i=1;i<4;i++)w->objects[i][1]=(uint8_t)(w->objects[i][1]+((w->cloud_counter&64)?1:-1));
    }
    /* 0F:4EB7: two fish flash in alternating steps of a four-tick
     * burst. Each coordinate is selected by its own free-running DIV read. */
    if(!(w->frame_counter&3)) {
        if(!w->fish_counter&&!(sample_divider(w)&0x72))w->fish_counter=4;
        if(w->fish_counter) {
            w->objects[16][3]=w->objects[17][3]=0xff;
            if(--w->fish_counter&1) {
                w->objects[16][3]=w->objects[17][3]=0x0a;
                const uint8_t *left=asset(w,15,0x4fa8+(sample_divider(w)&0x0e));
                memcpy(w->objects[16]+1,left,2);
                const uint8_t *right=asset(w,15,0x4fb8+((sample_divider(w)&0xe0)>>4));
                memcpy(w->objects[17]+1,right,2);
            }
        }
    }
    /* 0F:4F16: the ferry owns its moving sprite and wake markers. */
    if(w->boat_counter) {
        unsigned counter=++w->boat_counter;
        if(counter>=0x58)w->objects[14][3]=5;
        else {
            unsigned pose=counter<0x10||counter>=0x2c?0:((counter-2)&0x18)>>3;
            w->objects[14][1]=(uint8_t)(w->objects[14][1]+(counter<0x2c?-1:1));
            w->objects[14][3]=(uint8_t)(pose+5);
            unsigned wake=((uint8_t)(w->objects[14][1]-0x58)>>3)&7;
            static const uint8_t objects[]={10,11,12,13,15};
            for(unsigned i=0;i<5;i++)w->objects[objects[i]][3]=wake<6-i?0x0b:0xff;
        }
    }
}
static void animate_main_map(World *w) {animate_water(w);animate_main_objects(w);}
static void animate_pumpkin_secret(World *w) {
    /* 0F:5C62: the falling pumpkin opens the secret path and shakes the map. */
    WorldDecoration *d=&w->decoration;uint8_t *pumpkin=w->objects[7],*trail=w->objects[8];
    if(!d->secret_action){pumpkin[2]=trail[2]=0xff;return;}
    if(d->secret_action==1) {
        if(!(w->frame_counter&1))return;
        pumpkin[2]=0x6f;trail[2]=(uint8_t)(0x71+((w->frame_counter&12)>>2));
        trail[0]=pumpkin[0]=(uint8_t)(pumpkin[0]+3);trail[1]=pumpkin[1]=(uint8_t)(pumpkin[1]-3);
        if(!--d->secret_timer){d->secret_action=2;pumpkin[0]=0xa8;pumpkin[1]=14;trail[2]=0xff;d->secret_timer=10;}
    } else if(d->secret_action==2) {
        pumpkin[0]-=2;pumpkin[1]+=2;pumpkin[2]=0x6f;
        if(!--d->secret_timer) {
            d->secret_action=3;pumpkin[0]=0x9c;pumpkin[1]=0x24;pumpkin[2]=0x70;trail[2]=0xff;
            w->overlays[0][2]=0x38;w->overlays[1][2]=0x34;apply_overlays(w);d->secret_timer=20;w->sound_requests[0]=0x18;
        }
    } else {
        pumpkin[0]=0x8c;pumpkin[1]=0x24;pumpkin[2]=trail[2]=0xff;d->shake_x=d->shake_y=0;
        if(d->secret_timer) {
            --d->secret_timer;
            if(w->frame_counter&2) {
                d->shake_x=d->secret_timer>>1;d->shake_y=(uint8_t)~d->shake_x;
                if(!d->secret_timer)w->sound_requests[0]=0x15;
            }
        }
    }
}
static void choose_satellite_direction(World *w) {
    WorldDecoration *d=&w->decoration;unsigned direction;
    for(;;) {
        unsigned divider=sample_divider(w),timer=w->read_timer?w->read_timer(w->divider_context):0;
        direction=(divider+timer)&7;
        if(direction==d->satellite_direction)continue;
        if(!direction)break;
        if(direction>=5)direction-=4;
        if(direction<3?d->satellite_direction<3:d->satellite_direction>=3)continue;
        break;
    }
    d->satellite_direction=(uint8_t)direction;d->satellite_timer=30;
}
static void animate_space(World *w) {
    /* 0F:5FE8: coin, lightning and satellite. TIMA is disabled (zero) in
     * captured gameplay; the callback retains the register's distinct role. */
    WorldDecoration *d=&w->decoration;uint8_t *satellite=w->objects[3];
    if(!(w->frame_counter&7)) {
        /* A796 can retain a stage metatile byte. 5FE8 clamps the incremented
         * byte at3; it does not take a modulo of that inherited value. */
        if(++d->space_coin_phase>=3)d->space_coin_phase=0;
        w->objects[1][2]=(uint8_t)(d->space_coin_phase+0x35);
    }
    if(!(w->frame_counter&3)) {
        w->objects[2][2]=0xff;
        if(!(sample_divider(w)&0x7e)) {
            w->objects[2][2]=0x38;d->lightning_position=(uint8_t)((d->lightning_position+1)&3);
            w->objects[2][0]=*asset(w,15,0x61d3+d->lightning_position);
        }
        if(w->scene==WORLD_MAP) {
            if(d->lightning_phase==1){d->lightning_phase=2;w->background_palette=0xe1;w->sprite_palette[1]=0x39;}
            else if(d->lightning_phase==2){d->lightning_phase=0;w->background_palette=0x1e;w->sprite_palette[1]=0xc6;}
            else {
                w->background_palette=0xe1;w->sprite_palette[1]=0x39;
                if(w->objects[2][2]==0x38){w->background_palette=0x1e;w->sprite_palette[1]=0xc6;d->lightning_phase=1;}
            }
        }
    }
    if(w->frame_counter&1)return;
    ++d->satellite_clock;
    if(d->satellite_direction==1||d->satellite_direction==2) {
        bool right=d->satellite_direction==2;
        satellite[2]=(uint8_t)(*asset(w,15,0x61cb+(d->satellite_clock&3))+(right?0x3c:0x39));
        if(right?satellite[0]>=0x78:satellite[0]<0x28)choose_satellite_direction(w);
        else {
            satellite[0]=(uint8_t)(satellite[0]+(right?1:-1)*(int)(d->satellite_clock&1));
            if(satellite[0]>=0x40&&satellite[0]<0x60&&satellite[1]>=0x50)satellite[2]=0xff;
        }
    } else if(d->satellite_direction==3||d->satellite_direction==4) {
        bool down=d->satellite_direction==4;satellite[2]=0x3f;
        if(down?satellite[1]>=0x62:satellite[1]<0x36) {
            d->satellite_direction=satellite[0]<0x32?2:1;if(d->satellite_direction==2)d->satellite_timer=30;
        } else {satellite[1]=(uint8_t)(satellite[1]+(down?4:-4));satellite[2]=0xff;}
    } else {
        satellite[2]=(uint8_t)(0x3f+*asset(w,15,0x61cf+((d->satellite_clock&12)>>2)));
        if(d->satellite_timer)--d->satellite_timer;else choose_satellite_direction(w);
    }
}
static void animate_region_tiles(World *w);
static void animate_region_objects(World *w) {
    WorldDecoration *d=&w->decoration;
    if(w->region==1) {
        /* 0F:58E6: original frame tables and packed dx/dy flight commands. */
        ++d->animation_clock;
        w->objects[0][2]=*asset(w,15,0x5a21+((d->animation_clock&0x38)>>3));
        w->objects[1][2]=*asset(w,15,0x5a29+((d->animation_clock&12)>>2));
        if(!(w->frame_counter&7))w->objects[1][1]=(uint8_t)(w->objects[1][1]+((w->frame_counter&32)?-1:1));
        if(d->scheduler&1) {
            if(++d->flight_step>=80)d->flight_step=0;
            d->flight_command=*asset(w,15,0x6227+d->flight_step);
            unsigned dx=(d->flight_command>>4)&7,dy=d->flight_command&7;
            w->objects[2][0]=(uint8_t)(w->objects[2][0]+((d->flight_command&0x80)?-(int)dx:(int)dx));
            w->objects[2][1]=(uint8_t)(w->objects[2][1]+((d->flight_command&8)?-(int)dy:(int)dy));
        }
        w->objects[2][2]=(uint8_t)(0x16+*asset(w,15,0x5a16+((d->flight_step&0x78)>>3))+((d->flight_command&0x80)?0:3));
        if(w->progress[9]&0x80)w->objects[2][2]=0xff;
        if(d->secret_reveal&&!(w->frame_counter&7)) {
            unsigned overlay=w->node==0x2c?0:1,limit=overlay?0x2f:0x27,slot=overlay?0x27:0x26;
            if((w->node==0x2c||w->node==0x2f)&&!(w->progress[slot]&0x80)&&w->overlays[overlay][2]<limit) {
                if(++w->overlays[overlay][2]>=limit){w->overlays[overlay][2]=0xff;w->sound_requests[0]=0x15;}
                else w->sound_requests[0]=6;
                apply_overlays(w);
            }
        }
    } else if(w->region==2) {
        /* 0F:5A2D: diagonal decoration, orbiting object and boss poses. */
        if(++d->bob_timer>=10) {
            d->bob_timer=0;++d->bob_phase;
            w->objects[2][2]=(uint8_t)(0x57+(d->bob_phase&3)+((d->bob_phase&16)?4:0));
            int delta=(d->bob_phase&16)?-1:1;
            w->objects[2][0]=(uint8_t)(w->objects[2][0]+delta);w->objects[2][1]=(uint8_t)(w->objects[2][1]-delta);
        }
        w->objects[1][2]=(uint8_t)(4+((d->scheduler&2)>>1));
        if(d->scheduler&1) {
            if(++d->orbit_step>=80)d->orbit_step=0;
            unsigned command=*asset(w,15,0x6227+d->orbit_step),dx=(command>>4)&7,dy=command&7;
            w->objects[1][0]=(uint8_t)(w->objects[1][0]+((command&0x80)?-(int)dx:(int)dx));
            w->objects[1][1]=(uint8_t)(w->objects[1][1]+((command&8)?-(int)dy:(int)dy));
        }
        w->objects[0][2]=(w->progress[5]&0x80)?0xff:(uint8_t)((d->random_pose&0x30)>>4);
        if(!(w->frame_counter&15)&&w->objects[9][2]<0x6d) {
            ++w->objects[9][2];w->sound_requests[0]=w->objects[9][2]>=0x6d?0x15:6;
        }
        mark_object_clears(w,1,4,5);
    } else if(w->region==3)reveal_turtle_step(w);
    else if(w->region==4) {
        if(!(w->frame_counter&3)) {
            if(d->blink_timer||!(mix_random(w)&7)) {
                if(!d->blink_timer)d->blink_timer=5;
                --d->blink_timer;w->objects[0][2]=*asset(w,15,0x5d58+((d->blink_timer-1)&3));
            }
        }
        unsigned phase=(w->frame_counter&24)>>3;
        w->objects[6][2]=*asset(w,15,0x5d5c+phase);--w->objects[6][0];
        if(!(w->frame_counter&3)&&!++w->objects[6][1])w->objects[6][0]=sample_divider(w);
        if(!(w->frame_counter&7))for(unsigned i=0;i<3;i++) {
            w->objects[1+i][2]=*asset(w,15,0x5d60+i*4+phase);
            if(phase==i) {
                unsigned at=0x5d6c+(sample_divider(w)&0x1e);
                w->objects[1+i][0]=*asset(w,15,at);w->objects[1+i][1]=*asset(w,15,at+1);
            }
        }
        animate_pumpkin_secret(w);
        if(w->objects[9][2]<0x7b&&!(w->frame_counter&7)){++w->objects[9][2];w->sound_requests[0]=6;}
    } else if(w->region==5) {
        unsigned phase=(w->frame_counter&0x30)>>4;
        for(unsigned i=1;i<=3;i++)w->objects[i][2]=(uint8_t)(phase+0x2a);
        w->objects[7][2]=w->objects[8][2]=(uint8_t)(phase+0x2e);
        unsigned y=(w->frame_counter&0x7c)>>2;
        w->objects[8][1]=(uint8_t)(0x50-y);w->objects[7][1]=(uint8_t)(0x40-(y&15));
        if(!(w->progress[0x25]&0x80))w->objects[0][2]=(uint8_t)(0x26+((w->frame_counter&0x60)>>5));
        else if(!(w->frame_counter&15)){w->overlays[0][2]=(uint8_t)(0x35+(mix_random(w)&1));apply_overlays(w);}
        unsigned distance=(w->frame_counter&3)==3;
        if(!(d->horizontal_direction&1)) {
            d->horizontal_x=(uint8_t)(d->horizontal_x-distance);
            if(d->horizontal_x==0xc0){d->horizontal_direction^=0xff;w->objects[9][1]=0x78;}
            w->objects[9][2]=0x32;
        } else {
            d->horizontal_x=(uint8_t)(d->horizontal_x+distance);
            if(d->horizontal_x==0xb0){d->horizontal_direction^=0xff;w->objects[9][1]=0x40;}
            w->objects[9][2]=0x33;
        }
        w->objects[9][0]=d->horizontal_x;
        if(!(w->frame_counter&15)&&w->objects[10][2]<0x81){++w->objects[10][2];w->sound_requests[0]=w->objects[10][2]==0x81?0x15:6;}
    } else if(w->region==6) {
        /* 0F:5EC7: Mario Zone's coin, belt and unfolding stage paths. */
        if(!(w->frame_counter&3)&&!(w->progress[0x1d]&0x80)&&(w->progress[0x1c]&0x80)&&w->objects[2][0]<0x5c) {
            ++w->objects[2][0];if(w->objects[2][1]<0x13)++w->objects[2][1];
        }
        if(!(w->frame_counter&1)&&w->x>=0x2e&&w->objects[5][0]<0x4a)++w->objects[5][0];
        unsigned phase=(w->frame_counter&0x70)>>4;
        w->objects[6][2]=*asset(w,15,0x5fa0+phase);
        w->objects[0][1]=*asset(w,15,0x5fa8+phase);
        w->objects[1][1]=*asset(w,15,0x5fb0+phase);
        w->objects[3][0]=*asset(w,15,0x5fb8+phase);
        w->objects[4][0]=*asset(w,15,0x5fc0+phase);
        if(!(w->frame_counter&15)) {
            phase=(w->frame_counter&0xe0)>>5;
            w->overlays[0][2]=*asset(w,15,0x5fc8+phase);w->overlays[1][2]=*asset(w,15,0x5fd0+phase);
            if(!(w->progress[0x1b]&0x80)&&(w->progress[0x1a]&0x80)&&w->overlays[2][2]<8)++w->overlays[2][2];
            if(!(w->progress[0x1c]&0x80)&&(w->progress[0x1b]&0x80)&&w->overlays[3][2]<11)++w->overlays[3][2];
            apply_overlays(w);
        }
    } else if(w->region==7)animate_space(w);
    else if(w->region==8) {
        /* 0F:5D8C and 64EB: floating signs and the coin digits. */
        if(!(w->frame_counter&7)) {
            int delta=(w->frame_counter&0x80)?1:-1;
            w->objects[0][0]=(uint8_t)(w->objects[0][0]+delta);
            w->objects[1][0]=(uint8_t)(w->objects[1][0]-delta);
        }
        w->objects[3][2]=(uint8_t)(0x60+w->coins/100%10);
        w->objects[4][2]=(uint8_t)(0x60+w->coins/10%10);
        w->objects[5][2]=(uint8_t)(0x60+w->coins%10);
    }
}
static void animate_region_tiles(World *w) {
    WorldDecoration *d=&w->decoration;
    /* 0F:6C7D/6CF5/6D35 own changing background tile bytes. */
    if((w->region==1&&(w->progress[0x27]&0x80))||(w->region==2&&(w->progress[0x24]&0x80))) {
        if(++d->tile_timer>=14) {
            d->tile_timer=0;++d->tile_phase;
            unsigned phase=*asset(w,15,w->region==1?0x6cb5+(d->tile_phase&63):0x6d2d+(d->tile_phase&7));
            unsigned at=(w->region==1?0x5300:0x5800)+phase*64,offset=w->region==1?0x8e0:0x820;
            memcpy(w->tiles+offset,asset(w,w->region==1?16:14,at),32);
            memcpy(w->tiles+offset+0x100,asset(w,w->region==1?16:14,at+32),32);
        }
    }
    if(w->region==5&&(w->frame_counter&15))memcpy(w->tiles+0x1000,asset(w,14,0x5c00+(w->frame_counter&0x30)*2),32);
    if(w->region==7) {
        /* 0F:67F2: the three cloud strips scroll at distinct rates. */
        ++w->water_counter;
        if(!(w->water_counter&3)) {
            --d->cloud_scroll[0];
            if(!(w->water_counter&4)) {
                if(!d->cloud_scroll[1])d->cloud_scroll[1]=24;
                --d->cloud_scroll[1];
                if(!(w->water_counter&8))--d->cloud_scroll[2];
            }
        }
    }
}
bool world_init_with_clock(World *w,const uint8_t *rom,size_t size,const uint8_t record[0x50],WorldClockRead divider,WorldClockRead timer,void *context) {
    if(!w||!rom||size!=524288||!record||record[0]>=WORLD_NODE_COUNT)return false;
    memset(w,0,sizeof(*w));w->rom=rom;w->rom_size=size;
    w->read_divider=divider;w->read_timer=timer;w->divider_context=context;
    memcpy(w->record,record,0x50);memcpy(w->progress,record+8,WORLD_PROGRESS_COUNT);
    w->node=record[0];w->movement_cadence=record[0x38];w->lives=(record[0x43]>>4)*10+(record[0x43]&15);
    w->coins=(record[0x40]>>4)*10+(record[0x40]&15)+(record[0x41]&15)*100;
    return world_return(w,WORLD_EXISTING);
}
bool world_init(World *w,const uint8_t *rom,size_t size,const uint8_t record[0x50]) {
    return world_init_with_clock(w,rom,size,record,NULL,NULL,NULL);
}
void world_inherit_metatiles(World *w,const uint8_t m[512]) {
    /* Preserve typed fields whose storage the original level loader reused.
     * Each subsequent scene initializer still owns its explicit resets. */
    w->water_counter=m[0x89];w->animation=m[0x8c];w->segment_timer=m[0x94];w->pose=m[0x95];w->frame_counter=m[0x9a];
    w->coin_animation=m[0x180];w->cloud_counter=m[0x181];w->boat_counter=m[0x189];w->fish_counter=m[0x1a9];w->random_state=m[0x1b7];
    WorldDecoration *d=&w->decoration;
    d->random_pose=m[0x96];d->scheduler=m[0x97];d->animation_clock=m[0x99];
    d->flight_step=m[0xa2];d->flight_command=m[0xa5];d->horizontal_x=m[0xa7];d->horizontal_direction=m[0xa8];d->orbit_step=m[0xb0];
    d->bob_phase=m[0x183];d->bob_timer=m[0x184];memcpy(d->cloud_scroll,m+0x18a,3);
    d->space_coin_phase=m[0x196];d->lightning_phase=m[0x197];d->satellite_clock=m[0x198];d->lightning_position=m[0x199];
    d->satellite_direction=m[0x19e];d->satellite_timer=m[0x19f];
    d->secret_reveal=m[0x1ba];d->secret_action=m[0x1bb];d->secret_timer=m[0x1bc];d->blink_timer=m[0x1bd];d->tile_timer=m[0x1be];d->tile_phase=m[0x1bf];
    WorldBonus *b=&w->bonus;
    b->arrow_cycle=m[0x196];b->lever_phase=m[0x197];b->lever_distance=m[0x198];b->jump_step=m[0x19a];
    b->reel_phase=m[0x1a5];b->action=m[0x1a6];b->timer=m[0x1a7];b->prize=m[0x1a8];b->loss_row=m[0x1aa];
    b->previous_power=m[0x1ab];b->awarded_power=m[0x1ac];b->reel_ticks=m[0x1ad];b->flash=m[0x1b9];
}
bool world_return(World *w,enum WorldReturn result) {
    if(w->node>=WORLD_NODE_COUNT)return false;
    uint8_t slot,previous_node=w->node,previous_progress[WORLD_PROGRESS_COUNT];
    memcpy(previous_progress,w->progress,sizeof(previous_progress));
    w->scene=WORLD_MAP;w->stage_requested=false;w->return_requested=false;w->peeking=false;w->audio_clock=WORLD_AUDIO_MAIN;
    w->return_processed=result==WORLD_EXISTING||result==WORLD_FAILED||result==WORLD_FAILED_CHECKPOINT;
    if(world_node_progress_slot(w,w->node,&slot)) {
        if(result==WORLD_CLEAR) {
            if(!(w->progress[slot]&0x80)) {
                unsigned count=(w->record[0x42]>>4)*10+(w->record[0x42]&15)+1;
                w->record[0x42]=(uint8_t)((count/10<<4)|(count%10));
            }
            w->progress[slot]=(uint8_t)((w->progress[slot]|0x80)&~8);
        }
        else if(result==WORLD_SECRET)w->progress[slot]&=(uint8_t)~8;
        else if(result==WORLD_FAILED_CHECKPOINT)w->progress[slot]|=8;
    }
    /* 0F:6F56/7003: original return destinations, including secret exits. */
    if(result==WORLD_SECRET) {
        switch(w->node) {
            case 0x48:w->node=0x47;break;
            case 0x1a:case 5:case 0x12:w->node=0x69;break;
            case 0x29:w->node=0x2c;break;case 0x2a:w->node=0x2f;break;
            case 0x3c:w->node=0x40;break;default:break;
        }
        /* Zone owners expose their secret path after the shared return
         * remap: 0F:518E,529F,5409,5533,569C. */
        unsigned region=node_region(w->node);
        if(region==2){w->node=0x25;w->progress[2]|=1;}
        if(w->node==0x2c)w->progress[7]|=1;
        if(w->node==0x2f)w->progress[8]|=1;
        if(region==4){w->node=0x43;w->progress[0x10]|=1;}
        if(region==5)w->progress[0x16]|=1;
        if(w->node==0x32){w->node=0x6b;w->progress[0x0b]|=1;}
    }
    if(world_node_progress_slot(w,w->node,NULL)&&
       (result==WORLD_CLEAR||result==WORLD_SECRET||result==WORLD_FAILED_CHECKPOINT))save_now(w); /* 6EF0 */
    bool exited_boss=false;uint8_t boss_segment=8;
    if(result==WORLD_CLEAR) {
        static const uint8_t bosses[]={5,9,14,17,23,29};
        static const uint8_t destinations[]={0x19,0x1c,0x1b,0x1a,0x1e,0x1d};
        static const uint8_t segments[]={8,8,0x22,2,4,2}; /* 46BD..477C */
        if(world_node_progress_slot(w,w->node,&slot))
            for(unsigned i=0;i<6;i++)if(slot==bosses[i]){w->node=destinations[i];boss_segment=segments[i];exited_boss=true;}
        if(w->node==0){w->progress[0]|=0x80;w->node=0x14;}
        else if(!exited_boss&&(w->node==5||w->node==0x1a))w->node=0x3f;
    }
    if(w->node==0x26)w->node=0x27;
    if(w->node==0x44)w->node=0x45;
    if(w->node==0x5b)w->node=0x5c;
    if(w->node==0x66){w->node=0x68;if(result==WORLD_CLEAR)w->progress[0x25]|=0x80;}
    if(w->node==0x78){w->node=0x2e;if(result==WORLD_CLEAR)w->progress[0x26]|=0x80;}
    if(w->node==0x7a){w->node=0x31;if(result==WORLD_CLEAR)w->progress[0x27]|=0x80;}
    if(w->node==0x6a&&result==WORLD_CLEAR){w->node=0x6c;w->progress[0x23]|=0x80;w->return_processed=true;}
    node_coordinates(w);w->region=exited_boss?0:(uint8_t)node_region(w->node);w->segment=exited_boss?boss_segment:w->region==3?0x21:8;
    w->movement_cadence=w->region?0x92:0;
    unsigned secret_slot=WORLD_PROGRESS_COUNT;
    if(result==WORLD_SECRET) {
        if(w->region==1&&(w->node==0x2c||w->node==0x2f))secret_slot=w->node==0x2c?7:8;
        if(w->region==2)secret_slot=2;
        if(w->region==3&&previous_node==0x32)secret_slot=0x0b;
        if(w->region==4)secret_slot=0x10;
        if(w->region==5)secret_slot=0x16;
    }
    bool new_secret=secret_slot<WORLD_PROGRESS_COUNT&&!(previous_progress[secret_slot]&1);
    /* Initializers draw previously opened paths before starting a new reveal.
     * Keep the progress change and the presentation order independently owned. */
    if(new_secret)w->progress[secret_slot]&=(uint8_t)~1;
    setup_return_assets(w,secret_slot,new_secret);
    if(new_secret)w->progress[secret_slot]|=1;
    /* Boss remaps jump through 47BD and retain 6EF0's earlier save. The
     * ordinary 703D/714C paths save again after their initializer. */
    if(!exited_boss)save_now(w);
    if(!w->region){w->background_palette=0;begin_fade(w,WORLD_FADE_MAP_READY);}
    return !w->asset_error;
}
static void move_cursor(World *w) {
    if(w->segment&4) {
        w->pose=(uint8_t)(8+((w->animation&12)>>2));
        if(w->y>=8)--w->y;
        if(!w->region&&w->y==0x4b&&w->camera_y){--w->camera_y;++w->y;}
    } else if(w->segment&8) {
        w->pose=(uint8_t)(12+((w->animation&12)>>2));
        if(w->y<0x8a)++w->y;
        if(!w->region&&w->y==0x4d&&w->camera_y<0x170){++w->camera_y;--w->y;}
    }
    if(w->segment&1) {
        if((w->animation&12)==12)w->animation=0;
        w->pose=(uint8_t)(4+((w->animation&12)>>2));
        if(w->x<0x94)++w->x;
        if(!w->region&&w->x==0x4c&&w->camera_x<0x160){++w->camera_x;--w->x;}
    } else if(w->segment&2) {
        if((w->animation&12)==12)w->animation=0;
        w->pose=(uint8_t)((w->animation&12)>>2);
        if(w->x>=8)--w->x;
        if(!w->region&&w->x==0x4a&&w->camera_x){--w->camera_x;++w->x;}
    } else if(!(w->segment&15))w->pose=(uint8_t)(12+((w->animation&24)>>3));
}
static void advance_path(World *w) {
    bool skip=w->movement_cadence&1;
    w->movement_cadence=(uint8_t)((w->movement_cadence>>1)|(w->movement_cadence<<7));
    if(skip)return;
    if(!--w->segment_timer) {
        w->segment_timer=8;w->path++;
        uint8_t command=*asset(w,24,w->path);
        if(!command) {
            uint8_t destination=*asset(w,24,w->path+1);
            if(destination>=WORLD_NODE_COUNT){w->asset_error=true;w->moving=false;return;}
            w->node=destination;w->moving=false;return;
        }
        w->segment=command;
        if(command&0x10)w->segment_timer=4;
    }
    ++w->animation;move_cursor(w);
}
static bool start_path(World *w,unsigned direction) {
    uint16_t at=word(w,24,0x563a+w->node*8+direction*2);
    if(at==0xffff)return false;
    if(at<0x5a3a||at>=0x5ee6){w->asset_error=true;return false;}
    w->path=at;w->segment=*asset(w,24,at);w->moving=true;
    w->segment_timer=(w->segment&0xf0)==0x10?5:9;
    advance_path(w);return true;
}
static void collected_coin_sprites(World *w) {
    /* 0C:47BC draws the six independently owned castle coins. */
    static const uint8_t coin_slots[]={5,9,0x1d,0x17,0x11,0x0e};
    static const uint8_t offsets[]={0,4,8,11,14,18};
    for(unsigned item=0;item<6;item++) {
        if(!(w->progress[coin_slots[item]]&0x80))continue;
        unsigned offset=offsets[item];
        unsigned x=*asset(w,12,0x498a+offset),y=*asset(w,12,0x49a2+offset),id=*asset(w,12,0x49ba+offset);
        player_sprite(w,id,x,y,true);w->scene_sprite=(uint8_t)id;
    }
}
static void castle_sprites(World *w) {
    w->sprite_count=0;memset(w->sprites,0,sizeof(w->sprites));
    player_sprite(w,w->scene_sprite,0x58,w->scene_y,true);collected_coin_sprites(w);
}
static void castle_assets(World *w) {
    /* 0C:45B6: six coin owners determine the door's original map patches. */
    w->map_stride=32;
    memcpy(w->tiles,asset(w,12,0x4c92),0x1800);
    memset(w->map,0,sizeof(w->map));memcpy(w->map,asset(w,12,0x6492),0x300);
    if(world_golden_coins(w)==6)
        for(unsigned y=10;y<=14;y++)for(unsigned x=8;x<12;x++)w->map[y*32+x]=0x74;
    w->background_palette=0xe4;w->sprite_palette[0]=0xd0;w->sprite_palette[1]=0x38;
}
bool world_castle_prepare(World *w) {
    if(!w||!w->rom||w->stage!=0xff)return false;
    w->scene=WORLD_CASTLE_GATE;w->scene_action=0;w->scene_timer=64;
    w->scene_y=0xa8;w->scene_sprite=0xbb;castle_assets(w);
    w->music_request=0xff;w->stage_requested=false;
    w->sprite_count=0;memset(w->sprites,0,sizeof(w->sprites));return true;
}
static void castle_tick(World *w,uint8_t pressed) {
    /* 0C:4693's four named states: arrive, leave, idle, enter castle. */
    switch(w->scene_action) {
        case 0:
            w->scene_sprite=*asset(w,12,0x4777+((w->global_frame&12)>>2));
            if(--w->scene_timer)w->scene_y=(uint8_t)(0x88+w->scene_timer/2);
            else w->scene_action=2;
            break;
        case 1:
            w->scene_sprite=*asset(w,12,0x4748+((w->global_frame&12)>>2));
            if(--w->scene_timer)w->scene_y=(uint8_t)(0xa8-w->scene_timer/2);
            else {w->return_result=WORLD_EXISTING;w->return_requested=true;}
            break;
        case 2:
            w->scene_sprite=0xbb;
            if(world_golden_coins(w)==6&&(pressed&(4|16|128))) {w->scene_action=3;w->scene_timer=64;}
            else if(pressed==8){w->scene_action=1;w->scene_timer=64;}
            break;
        case 3: {
            if(w->scene_timer)--w->scene_timer;
            const uint8_t *palette=asset(w,12,0x46d1+(w->scene_timer>>4)*4);
            w->background_palette=palette[0];w->sprite_palette[0]=palette[1];w->sprite_palette[1]=palette[2];
            w->scene_sprite=0xbb;
            if(!w->scene_timer){w->stage=0x18;w->stage_flags=0;w->stage_requested=true;}
            break;
        }
    }
    castle_sprites(w);
}
bool world_boss_prepare(World *w,uint8_t stage) {
    if(!w||!w->rom||stage>=24||!*asset(w,12,0x44f1+stage))return false;
    castle_assets(w);w->award.stage=stage;w->award.x=0x58;
    w->scene=WORLD_COIN_AWARD;w->scene_action=0;w->scene_timer=64;
    w->stage_requested=false;w->return_requested=false;w->save_requested=false;w->moving=false;w->peeking=false;
    w->sprite_count=0;memset(w->sprites,0,sizeof(w->sprites));return true;
}
bool world_boss_clear(World *w,uint8_t stage) {
    if(!w||!w->rom||stage>=24)return false;
    unsigned slot=*asset(w,12,0x44f1+stage);if(!slot)return false;
    /* 0C:441D counts a newly completed boss before its coin flies in. */
    if(!(w->progress[slot]&0x80)) {
        unsigned count=(w->record[0x42]>>4)*10+(w->record[0x42]&15)+1;
        w->record[0x42]=(uint8_t)(((count%100)/10<<4)|(count%10));
    }
    w->award.stage=stage;w->award.x=*asset(w,12,0x4985+stage);w->award.animation=0;
    w->scene=WORLD_COIN_AWARD;w->scene_action=0;w->scene_y=0xc0;
    return true;
}
static void coin_award_tick(World *w) {
    WorldCoinAward *a=&w->award;
    /* 0C:49CD's VBlank door patch belongs to the frame after its request. */
    if(a->pending_door) {
        unsigned source=0x4a2a+(a->pending_door-1)*4;
        for(unsigned row=0;row<5;row++)memcpy(w->map+(14-row)*32+8,asset(w,12,source+row*4),4);
        a->pending_door=0;
    }
    if(a->timer)--a->timer;
    w->sprite_count=0;memset(w->sprites,0,sizeof(w->sprites));
    switch(w->scene_action) {
        case 0: {
            player_sprite(w,0xc5+((w->global_frame&4)>>2),a->x,w->scene_y,true);
            a->animation=(uint8_t)(a->animation+8);if(a->animation>=0x50)a->animation=0;
            w->scene_sprite=(uint8_t)(0xbf+(a->animation>>4));
            unsigned target=*asset(w,12,0x499d+a->stage);
            if(w->scene_y!=target){if(!(w->global_frame&1))++w->scene_y;}
            else {w->music_request=a->coins==5?0x1a:0x0f;w->scene_action=1;a->timer=0x1ff;}
            player_sprite(w,w->scene_sprite,a->x,w->scene_y,true);break;
        }
        case 1:
            if(a->timer) {
                w->scene_y=*asset(w,12,0x499d+a->stage);w->scene_sprite=*asset(w,12,0x49b5+a->stage);
                w->sprite_palette[0]=(w->global_frame&31)<4?0:0xd0;
                player_sprite(w,w->scene_sprite,a->x,w->scene_y,true);
            } else {
                w->sprite_palette[0]=0xd0;w->progress[*asset(w,12,0x44f1+a->stage)]=0x80;
                if(a->coins!=5){w->return_requested=true;w->return_result=WORLD_CLEAR;}
                else {
                    w->scene_action=2;a->timer=64;a->door_step=5;
                    player_sprite(w,w->scene_sprite,a->x,w->scene_y,true);
                }
            }
            break;
        case 2:
            if(!(a->timer&255)) {
                a->pending_door=a->door_step;
                if(a->door_step){--a->door_step;a->timer=64;w->sound_requests[2]=0x0a;}
                else {w->scene_action=3;a->timer=255;}
            }
            break;
        case 3:if(!(a->timer&255)){w->return_requested=true;w->return_result=WORLD_CLEAR;}break;
        default:break;
    }
    collected_coin_sprites(w);a->coins=(uint8_t)world_golden_coins(w);
}
bool world_ending(World *w) {
    if(!w||!w->rom)return false;
    memset(&w->ending,0,sizeof(w->ending));w->scene=WORLD_ENDING;w->scene_action=0;
    w->stage_requested=false;w->save_requested=false;w->moving=false;w->peeking=false;
    return true;
}
static void ending_copy(World *w,unsigned bank,unsigned source,unsigned destination,unsigned size) {
    WorldEnding *e=&w->ending;e->copy_bank=(uint8_t)bank;e->copy_source=(uint16_t)source;
    e->copy_destination=(uint16_t)destination;e->copy_remaining=(uint16_t)size;
}
static void ending_vblank(World *w) {
    WorldEnding *e=&w->ending;e->render_scroll_y=e->scroll_y;
    /* 00:26D7 transfers 64 bytes per VBlank; it precedes the ending's text
     * writer, so graphics transfer and text writes cannot occur together. */
    if(e->copy_remaining) {
        unsigned count=e->copy_remaining<64?e->copy_remaining:64;
        uint8_t *destination=e->copy_destination<0x9800?w->tiles+e->copy_destination-0x8000:w->map+e->copy_destination-0x9800;
        memcpy(destination,asset(w,e->copy_bank,e->copy_source),count);
        e->copy_source=(uint16_t)(e->copy_source+count);e->copy_destination=(uint16_t)(e->copy_destination+count);
        e->copy_remaining=(uint16_t)(e->copy_remaining-count);return;
    }
    if(!e->text_rows) {
        if(e->patch_castle) {
            for(unsigned i=0;i<3;i++){w->map[0x148+i]=(uint8_t)(0x89+i);w->map[0x168+i]=(uint8_t)(0x99+i);}
            e->patch_castle=0;
        }
        return;
    }
    /* 1A:4572 streams the original twenty-character credit rows. FF is a
     * blank line, FE ends the credits; ordinary characters use tile c-30. */
    unsigned destination=0x220+(4-e->text_rows)*32;
    if(*asset(w,26,e->text_source)==0xff) {
        ++e->text_source;memset(w->map+destination,0xff,20);
    } else for(unsigned i=0;i<20;i++) {
        uint8_t tile=(uint8_t)(*asset(w,26,e->text_source++)-0x30);
        w->map[destination+i]=tile==0xf0?0xff:tile;
    }
    if(*asset(w,26,e->text_source)==0xfe)e->text_finished=0xff;
    if(!--e->text_rows)e->timer=256;
}
static void ending_castle_sprite(World *w) {
    player_sprite(w,0xeb,0x4f,(uint8_t)(w->ending.y+0x34),w->ending.sprite_front);
}
static void ending_mario_sprite(World *w,bool behind) {
    WorldEnding *e=&w->ending;e->sprite_front=!behind;
    if(++e->sprite_clock>=0x30)e->sprite_clock=0;
    player_sprite_palette(w,(behind?0xe5:0xe8)+(e->sprite_clock>>4),0x54,e->y,!behind,behind);
    if(!behind)ending_castle_sprite(w);
}
static void ending_wave(World *w) {
    /* 1A:4400 starts at LY16; its writes affect the following scanline.
     * The final zero write follows the terminal line's HBlank wait. */
    w->ending.wave_first=17;w->ending.wave_last=w->ending.wave_bottom<16?17:(uint8_t)(w->ending.wave_bottom+1);
}
static void ending_tick(World *w) {
    WorldEnding *e=&w->ending;
    if(e->timer)--e->timer;
    e->wave_first=e->wave_last=e->wave_hold=0;e->unsigned_lower_tiles=false;
    if(!e->phase) {
        const uint8_t *palettes=asset(w,0,0x03e1+((e->timer&0xc0)>>4));
        w->background_palette=palettes[0];w->sprite_palette[0]=palettes[1];w->sprite_palette[1]=palettes[2];
        if(!e->timer){e->timer=255;e->phase=1;}return;
    }
    if(e->phase==1){e->phase=2;w->scene_action=0;w->audio_clock=WORLD_AUDIO_SKIP;return;}
    w->sprite_count=0;memset(w->sprites,0,sizeof(w->sprites));
    ending_vblank(w);
    /* 1A:4016 dispatch is expressed as scene actions; no ROM instructions
     * or instruction addresses are executed by the native runtime. */
    switch(w->scene_action) {
        case 0:case 1:case 2:case 3:++w->scene_action;break;
        case 4:
            memcpy(w->tiles,asset(w,17,0x5800),0x1800);memcpy(w->map,asset(w,26,0x548e),0x400);w->map_stride=32;
            e->scroll_y=0;e->y=0x2c;e->timer=256;w->background_palette=0xe1;
            w->sprite_palette[0]=0xd2;w->sprite_palette[1]=0x39;++w->scene_action;break;
        case 5:
            ending_mario_sprite(w,true);
            if(!e->timer){e->timer=128;++w->scene_action;}break;
        case 6:
            if(!(e->timer&3))++e->y;
            ending_mario_sprite(w,true);
            if(!e->timer){e->timer=32;++w->scene_action;e->wave_bottom=13;}break;
        case 7:
            ending_wave(w);e->wave_bottom=(uint8_t)(e->wave_bottom+(w->global_frame&1));
            if(e->wave_bottom==0x8b){ending_copy(w,26,0x5096,0x9800,0x400);++w->scene_action;}break;
        case 8:
            ending_wave(w);
            if(!e->copy_remaining){ending_copy(w,16,0x6000,0x8800,0x1000);e->timer=512;++w->scene_action;}break;
        case 9:ending_wave(w);if(!e->copy_remaining)++w->scene_action;break;
        case 10:
            ending_wave(w);e->wave_bottom=(uint8_t)(e->wave_bottom-(w->global_frame&1));
            if(e->wave_bottom==13){e->timer=192;++w->scene_action;}break;
        case 11:if(!e->timer){e->timer=384;++w->scene_action;e->patch_castle=0xff;}break;
        case 12:
            if(w->global_frame&1)w->sound_requests[1]=0x18;
            e->sprite_front=true;player_sprite(w,*asset(w,26,0x4518+((w->global_frame&0x1c)>>2)),0x4f,0x60,true);
            if(!e->timer){e->timer=64;++w->scene_action;w->sound_requests[0]=7;e->y=0x2c;}break;
        case 13:ending_castle_sprite(w);if(!e->timer){e->timer=512;++w->scene_action;}break;
        case 14:
            ending_castle_sprite(w);
            if(!e->copy_remaining){ending_copy(w,26,0x4656,0x8000,0x800);++w->scene_action;}break;
        case 15:
            ending_castle_sprite(w);
            if(!e->copy_remaining){++w->scene_action;w->sound_requests[0]=6;e->timer=64;}break;
        case 16:
            e->y=0x2c;ending_mario_sprite(w,false);
            if(!e->timer){e->timer=384;++w->scene_action;}break;
        case 17:
            ending_mario_sprite(w,false);
            e->scroll_y=(uint8_t)(~((e->timer>>4)&255)+0x19);e->y=(uint8_t)(0x2c-e->scroll_y);
            if(e->scroll_y==0x18){e->text_source=0x56ce;e->text_finished=0;e->text_rows=4;++w->scene_action;}break;
        case 18:
            ending_mario_sprite(w,false);e->unsigned_lower_tiles=true;
            if((uint8_t)(e->timer+0x20)<0x40) {
                e->wave_first=112;e->wave_last=143;e->wave_hold=140;
                if(e->text_rows)break;
            }
            if(e->text_finished){++w->scene_action;e->finished=true;}
            else if(!e->timer){e->text_rows=4;e->timer=255;}
            break;
        case 19:ending_mario_sprite(w,false);e->unsigned_lower_tiles=true;e->finished=true;break;
        default:w->asset_error=true;break;
    }
}
static void game_over_sprites(World *w) {
    /* 0C:4884: retained coins rise from the castle as their owners clear. */
    static const uint8_t offsets[]={0,4,8,11,14,18};
    w->sprite_count=0;memset(w->sprites,0,sizeof(w->sprites));
    uint8_t displacement=w->scene_timer<0x80?(uint8_t)(w->scene_timer-0x80):0;
    for(unsigned i=0;i<6;i++)if(w->lost_coin_mask&(0x20u>>i)) {
        unsigned at=offsets[i],x=*asset(w,12,0x498a+at),y=(uint8_t)(*asset(w,12,0x49a2+at)+displacement);
        unsigned id=displacement?0xde + ((displacement&6)>>1):*asset(w,12,0x49ba+at);
        player_sprite(w,id,x,y,true);
    }
    unsigned y=w->scene_timer>=0xc0?0x20-(w->scene_timer-0xc0)/2:0x20;
    player_sprite(w,0xe3+((w->global_frame&0x10)>>4),0x44,y,true);
}
bool world_game_over(World *w) {
    if(!w||!w->rom)return false;
    static const uint8_t owners[]={5,9,0x1d,0x17,0x11,0x0e};
    w->lost_coin_mask=0;unsigned lost=0;
    for(unsigned i=0;i<6;i++) {
        if(w->progress[owners[i]]&0x80){w->lost_coin_mask|=(uint8_t)(0x20u>>i);++lost;}
        w->progress[owners[i]]=0;
    }
    unsigned count=((w->record[0x42]>>4)*10+(w->record[0x42]&15)+100-lost)%100;
    w->record[0x42]=(uint8_t)((count/10<<4)|(count%10));w->record[0x44]=0;
    w->lives=5;w->coins=0;w->scene=WORLD_GAME_OVER;w->scene_timer=0xff;
    w->stage_requested=false;w->title_requested=false;save_now(w);w->moving=false;
    memcpy(w->tiles,asset(w,12,0x4c92),0x1800);
    memset(w->map,0,sizeof(w->map));memcpy(w->map,asset(w,12,0x6492),0x300);
    w->map_stride=32;w->background_palette=0xe4;w->music_request=7;
    w->sprite_count=0;memset(w->sprites,0,sizeof(w->sprites));return true;
}
static void game_over_tick(World *w,uint8_t pressed) {
    if(w->scene_timer)--w->scene_timer;
    game_over_sprites(w);
    /* 00:2898 requires a fresh, solitary Start after the 255-frame timer. */
    if(!w->scene_timer&&pressed==128){w->title_requested=true;w->music_request=0xff;}
}
static unsigned bonus_pose(const World *w,unsigned power) {
    return *asset(w,24,0x4c4d+(power&3));
}
static void bonus_sprites(World *w) {
    w->sprite_count=0;memset(w->sprites,0,sizeof(w->sprites));
    for(unsigned i=0;i<16;i++) {
        const uint8_t *o=w->objects[i];if(o[2]==0xff)continue;
        metasprite(w,24,0x51b1,o[2],o[0],o[1]);
    }
    /* The first object reserves the player's OAM slots. 18:506D builds
     * objects, then 01:5297 overwrites those slots from the start of OAM. */
    unsigned objects=w->sprite_count;w->sprite_count=0;
    player_sprite(w,w->bonus.player_sprite,w->bonus.player_x,w->bonus.player_y,w->bonus.player_front);
    if(w->sprite_count<objects)w->sprite_count=objects;
}
static void bonus_reel_map(World *w) {
    /* 0F:6BD5 runs during VBlank. Each reel row is six consecutive tiles. */
    unsigned row=(w->bonus.reel_phase&0x7c)>>2;
    if(row>=30){w->bonus.reel_phase&=0x77;row=29;}
    unsigned at=word(w,15,0x6c75+(w->node-0x6e)*2)+row*6;
    for(unsigned y=0;y<9;y++)memcpy(w->map+0x8a+y*32,asset(w,16,at+y*6),6);
    w->map[0x25]=(uint8_t)(0x80+w->coins%10);
    w->map[0x24]=(uint8_t)(0x80+w->coins/10%10);
    w->map[0x23]=(uint8_t)(0x80+w->coins/100%10);
    w->map[0x64]=(uint8_t)(0x80+w->lives%10);
    w->map[0x63]=(uint8_t)(0x80+w->lives/10%10);
    if(w->bonus.loss_row&0x80) {
        unsigned at=0x8c+(w->bonus.loss_row&15)*32;
        w->map[at]=0xd4;w->map[at+1]=0xd5;w->map[at+32]=0xd8;w->map[at+33]=0xd9;
    }
}
static bool bonus_enter(World *w) {
    if(w->region!=8||w->node<0x6e||w->node>0x71)return false;
    static const unsigned costs[]={30,50,200,999};
    unsigned machine=w->node-0x6e;
    if(w->coins<costs[machine]){w->sound_requests[0]=0x13;return true;}
    w->coins-=costs[machine];
    save_now(w); /* 0F:738F saves after the successful debit. */
    uint8_t phase=w->bonus.reel_phase,ticks=w->bonus.reel_ticks,flash=w->bonus.flash;
    memset(&w->bonus,0,sizeof(w->bonus));w->bonus.reel_phase=phase;w->bonus.reel_ticks=ticks;
    w->bonus.flash=flash; /* 7307 leaves the A7B9 prize-lamp cycle intact. */
    w->bonus.action=4;w->bonus.player_x=0x28;w->bonus.player_y=0x9c;
    w->bonus.player_sprite=(uint8_t)(bonus_pose(w,w->powerup)+0x10);
    w->scene=WORLD_BONUS;w->frame_counter=0;w->moving=false;
    w->region=(uint8_t)(12-machine);w->map_stride=32;
    memcpy(w->tiles,asset(w,6,0x4000),0x800);
    memcpy(w->tiles+0x800,asset(w,25,0x4000),0x1000);
    static const unsigned maps[]={0x7900,0x76c0,0x7480,0x7240};
    static const unsigned objects[]={0x66ec,0x6704,0x671c,0x66d4};
    memcpy(w->map,asset(w,16,maps[machine]),0x400);region_objects(w,objects[machine]);
    w->background_palette=0xe1;w->sprite_palette[0]=0xd0;w->sprite_palette[1]=0x39;
    w->music_request=0xff;audio_immediate(w);
    w->music_request=0x15;w->sound_requests[0]=5;bonus_reel_map(w);bonus_sprites(w);return true;
}
static void bonus_jump(World *w) {
    WorldBonus *b=&w->bonus;b->player_sprite=(uint8_t)bonus_pose(w,w->powerup);
    int8_t delta=(int8_t)*asset(w,24,0x4fa8+b->jump_step);
    if(delta==(int8_t)0x80)return;
    ++b->jump_step;b->player_y=(uint8_t)(b->player_y+delta);b->player_sprite+=8;
}
static void bonus_spin(World *w,bool aligned) {
    WorldBonus *b=&w->bonus;
    if(!aligned||(w->objects[1][1]&7)!=5) {
        ++b->arrow_cycle;
        if(b->arrow_cycle>=80)b->arrow_cycle=0;
        w->objects[1][1]=(uint8_t)(w->objects[1][1]+(b->arrow_cycle>=40?-1:1));
    }
    if(!aligned&&!(w->frame_counter&7)&&w->objects[2][2]!=7)++w->objects[2][2];
    b->reel_phase-=2;++b->reel_ticks;
}
static void bonus_prize(World *w) {
    /* 18:4D32: timing chooses a reel cell, then the machine owns its prize. */
    WorldBonus *b=&w->bonus;unsigned row=(b->reel_phase&0x7c)>>2;
    if(row>=30){b->reel_phase&=0x77;row=29;}
    unsigned arrow=((uint8_t)(w->objects[1][1]-0x28)>>3)&15;
    unsigned cell=*asset(w,24,0x4f7e + row+arrow);
    unsigned prizes=word(w,24,0x4f76+(w->node-0x6e)*2);
    b->prize=*asset(w,24,prizes+cell);
    if(b->prize)w->music_request=0x16;
}
static void bonus_award(World *w) {
    WorldBonus *b=&w->bonus;b->previous_power=b->awarded_power=w->powerup;
    if(!b->prize)return;
    if(b->prize<4)w->powerup=b->prize;
    else {
        static const unsigned lives[]={1,3,5,50,99,0,0,20};
        if(b->prize<=11)w->lives+=lives[b->prize-4];
        if(w->lives>99)w->lives=99;
        if(b->prize==9)w->coins+=300;
        if(b->prize==10)w->coins+=500;
        if(w->coins>999)w->coins=999;
    }
    b->awarded_power=w->powerup;
    uint8_t sound=*asset(w,24,0x4f6a+b->prize);
    w->sound_requests[sound==0x14?1:0]=sound;
    save_now(w); /* 18:4EAB saves nonempty prizes. */
}
static void bonus_tick(World *w,uint8_t pressed) {
    WorldBonus *b=&w->bonus;
    switch(b->action) {
        case 0:
            if(w->objects[2][2]==7&&(pressed&16)) {
                b->action=5;b->jump_step=0;w->sound_requests[0]=1;
            } else {bonus_spin(w,false);break;}
            /* The original dispatch continues directly into the jump. */
            /* fall through */
        case 5:
            bonus_jump(w);
            if(b->player_y>=0x6c||(w->frame_counter&1)){bonus_spin(w,false);break;}
            if(w->objects[3][2]<10){++w->objects[3][2];break;}
            b->action=1;b->timer=150;w->music_request=8;
            /* fall through */
        case 1:
            if(!--b->timer) {
                b->action=2;b->timer=90;w->objects[1][2]=2;bonus_prize(w);w->objects[4][1]=0x38;
            } else {
                bonus_jump(w);
                if(b->timer>=120)bonus_spin(w,true);
                else {
                    if(b->timer&1) {
                        if(++b->lever_phase>=3){b->lever_phase=3;w->objects[1][2]=2;}
                        else w->objects[1][2]=(uint8_t)(b->lever_phase+2);
                    }
                    unsigned speed=*asset(w,24,0x4fcd+b->timer);
                    if(speed) {
                        b->reel_phase=(uint8_t)(b->reel_phase-speed);++b->reel_ticks;
                        b->lever_distance=(uint8_t)(b->lever_distance+speed);
                        if(b->lever_distance>=3){b->lever_distance-=3;b->lever_phase=0;w->objects[1][2]=2;}
                    }
                }
                break;
            }
            /* fall through */
        case 2:
            if(w->frame_counter&1)break;
            w->objects[4][2]=0xff;
            if(b->timer<60) {
                ++w->objects[4][1];
                if(w->objects[4][1]==0x4e&&!b->prize) {
                    unsigned arrow=((uint8_t)(w->objects[1][1]-0x28)>>3)&15;
                    unsigned loss=((uint8_t)(w->objects[1][1]-0x30)>>3)&15;
                    b->loss_row=(uint8_t)(0x80+loss+2-((b->reel_phase>>2)+arrow)%3);w->sound_requests[0]=0x11;
                }
                w->objects[4][0]=0x20;w->objects[4][2]=(uint8_t)(b->prize+0x0b);
            }
            if(!--b->timer){b->timer=100;b->action=6;w->objects[4][2]=0xff;bonus_award(w);}
            break;
        case 4:
            b->player_sprite=(uint8_t)(bonus_pose(w,w->powerup)+0x10);
            if(--b->player_y<0x7d){b->action=0;b->player_sprite=(uint8_t)bonus_pose(w,w->powerup);b->player_front=true;}
            break;
        case 6: {
            unsigned power=b->awarded_power;
            if(b->timer>=25&&b->prize>0&&b->prize<4) {
                unsigned phase=w->frame_counter&12;
                if(phase==4||phase==12)power=0;
                else if(phase==8&&b->previous_power)power=b->previous_power;
            }
            b->player_sprite=(uint8_t)bonus_pose(w,power);
            if(!--b->timer)b->action=3;
            break;
        }
        default:break;
    }
    /* 18:4EE6/4EB5: prize lamps and reel glint retain original tile sources. */
    if(b->prize&&!(w->frame_counter&3)) {
        if(++b->flash>=3)b->flash=0; /* 18:4EE6 also accepts inherited bytes. */
        static const unsigned offsets[]={0xd30,0xe70,0x1550};
        for(unsigned i=0;i<3;i++)memcpy(w->tiles+offsets[i],asset(w,25,b->flash==i?0x4530:0x4d50),16);
    }
    w->objects[5][0]=*asset(w,24,0x4f5a+(w->node-0x6e)*4+((b->reel_ticks&12)>>2));
    if(b->action==3) {
        w->region=8;w->scene=WORLD_MAP;node_coordinates(w);setup_assets(w);return;
    }
    bonus_reel_map(w);bonus_sprites(w);
}
bool world_finish_stage(World *w,enum WorldReturn result,bool goal_bell,unsigned remaining_seconds) {
    if(!goal_bell||(result!=WORLD_CLEAR&&result!=WORLD_SECRET))return world_return(w,result);
    if(w->node>=WORLD_NODE_COUNT)return false;
    uint8_t flash=w->bonus.flash;
    memset(&w->bonus,0,sizeof(w->bonus));memset(&w->bell,0,sizeof(w->bell));
    w->bonus.flash=flash; /* Neither74B1 nor75A4 writes A7B9. */
    w->bell.result=result;w->scene=w->node&1?WORLD_BELL_LADDER:WORLD_BELL_CLAW;
    w->region=w->node&1?14:13;w->stage_requested=false;w->moving=false;w->peeking=false;
    w->frame_counter=0;w->map_stride=32;w->background_palette=0xe4;w->save_requested=false;
    w->sprite_palette[0]=0xd0;w->sprite_palette[1]=0x39;
    memcpy(w->tiles,asset(w,6,0x4000),0x800);
    memcpy(w->tiles+0x800,asset(w,25,0x5000),0x1000);
    memcpy(w->map,asset(w,17,w->region==13?0x7800:0x7a40),w->region==13?0x400:0x240);
    region_objects(w,w->region==13?0x6734:0x6761);
    WorldBonus *b=&w->bonus;b->player_x=0xf0;b->player_y=0x7c;
    b->player_sprite=(uint8_t)bonus_pose(w,w->powerup);b->player_front=true;
    if(w->region==13) {
        unsigned hundreds=(remaining_seconds/100)&15;
        w->bell.sequence=w->bell.sequence_start=hundreds==0?0x4a1a:hundreds==1?0x4a2f:0x4a58;
        for(unsigned i=0;i<5;i++)w->bell.items[i]=(WorldBellItem){3,0x90,0x60,(uint8_t)(2+i*24),0};
    }
    w->music_request=0xff;audio_immediate(w);
    w->music_request=0x15;w->sound_requests[0]=5;bonus_sprites(w);return !w->asset_error;
}
static void bell_walk(World *w,int direction,unsigned cadence) {
    WorldBonus *b=&w->bonus;
    b->player_x=(uint8_t)(b->player_x+direction);
    if(cadence?!(w->frame_counter&cadence):(w->frame_counter&2)!=0)
        w->bell.walk_phase=(uint8_t)((w->bell.walk_phase+1)%3);
    b->player_sprite=(uint8_t)(bonus_pose(w,w->powerup)+w->bell.walk_phase+(direction<0?5:1));
}
static void bell_prize(World *w) {
    /* 18:4917 awards lives immediately; power changes finish after flashing. */
    WorldBonus *b=&w->bonus;b->previous_power=b->awarded_power=w->powerup;
    if(b->prize<3){b->awarded_power=*asset(w,24,0x4c51+b->prize);w->sound_requests[1]=0x14;}
    else {w->lives+=b->prize==3?1:3;if(w->lives>99)w->lives=99;w->sound_requests[0]=9;}
}
static void bell_award_tick(World *w) {
    /* 18:46AA serves both bell games. */
    WorldBonus *b=&w->bonus;unsigned power=b->awarded_power;
    if(b->prize<3) {
        unsigned phase=w->frame_counter&12;
        if(phase==4||phase==12)power=0;
        else if(phase==8&&b->previous_power)power=b->previous_power;
    } else {
        unsigned object=b->prize==3&&w->region==14?15:12;
        uint8_t *o=w->objects[object];o[2]=b->prize==3?0x1d:0x1e;
        if(o[1]>=(object==15?0x62:0x60)) {
            --o[1];if(object==15)o[0]=(uint8_t)(b->player_x-4);
        } else {o[2]=0xff;if(object==15)o[0]=0xb0;}
    }
    b->player_sprite=(uint8_t)bonus_pose(w,power);
    if(!--b->timer) {
        w->powerup=b->awarded_power;b->player_sprite=(uint8_t)bonus_pose(w,w->powerup);
        b->action=9;b->timer=20;save_now(w);
    }
}
static void claw_items(World *w) {
    WorldBell *c=&w->bell;WorldBonus *b=&w->bonus;
    for(unsigned i=0;i<5;i++) {
        WorldBellItem *item=&c->items[i];uint8_t previous_state=item->state;
        if(item->state==0&&!(w->frame_counter&1)) {
            if(--item->x<0x32){item->x=0x8e;item->state=3;item->delay=28;}
            else if(b->action==3&&item->prize<5) {
                unsigned left=(uint8_t)(w->objects[2][0]-12);
                if(left<item->x&&(uint8_t)(left+16)>=item->x){item->state=1;c->grabbed=1;}
            }
        } else if(item->state==1) {
            item->x=(uint8_t)(w->objects[2][0]-4);item->y=(uint8_t)(w->objects[2][1]+8);
            if(b->action==7)item->state=2;
        } else if(item->state==2) {
            if(++item->y>=0x68) {
                item->state=3;item->delay=0xff;b->action=8;b->timer=20;
                b->prize=(uint8_t)(w->objects[7+i][2]-0x18);bell_prize(w);
            }
        }
        /* State transitions to waiting from falling do not run waiting until
         * the next frame; recycling from the conveyor does so immediately. */
        bool waiting=previous_state==3||(previous_state==0&&item->state==3);
        if(waiting) {
            item->x=0x8e;item->y=0x60;
            if(*asset(w,24,c->sequence)>=6)c->sequence=c->sequence_start;
            item->prize=*asset(w,24,c->sequence);w->objects[7+i][2]=0xff;
            if(!(w->frame_counter&1)&&item->delay!=0xff) {
                if(item->delay)--item->delay;
                else {
                    if(item->prize!=5)w->objects[7+i][2]=(uint8_t)(item->prize+0x18);
                    item->state=0;++c->sequence;
                }
            }
        }
        w->objects[7+i][0]=item->x;w->objects[7+i][1]=item->y;
    }
    c->item_cursor=5;
}
static void bell_claw_tick(World *w,uint8_t held,uint8_t pressed) {
    /* Original 18:4512 state machine, with explicit same-frame transitions. */
    WorldBonus *b=&w->bonus;bool advance=true;
    for(unsigned transitions=0;advance&&transitions<4;transitions++) {
        advance=false;
        switch(b->action) {
            case 0:
                if(!(w->frame_counter&1))break;
                if(b->player_x==0x20){b->action=1;b->player_sprite=(uint8_t)bonus_pose(w,w->powerup);w->bell.grabbed=0;advance=true;}
                else bell_walk(w,1,0); /* Animation advances on bit 1 below. */
                break;
            case 1:
                if(pressed&16){b->action=2;b->timer=24;w->music_request=8;advance=true;}
                else if(w->frame_counter&3) {
                    uint8_t x=w->objects[1][0];
                    if(!(b->timer&1)){if(x>=0x78)b->timer^=0xff;else ++x;}
                    else {if(x<0x50)b->timer^=0xff;else --x;}
                    for(unsigned i=1;i<=6;i++)w->objects[i][0]=x;
                }
                break;
            case 2:
                if(w->frame_counter&3)break;
                if(!b->timer){b->action=3;advance=true;}else {--b->timer;++w->objects[2][1];}
                break;
            case 3:
                if(w->frame_counter&3)break;
                if(w->objects[2][2]>=0x23){b->action=4;b->timer=32;advance=true;}else ++w->objects[2][2];
                break;
            case 4:
                if(w->frame_counter&3)break;
                if(!b->timer){b->action=5;advance=true;}
                else {
                    if(!--b->timer&&w->bell.grabbed==1)w->music_request=0x16;
                    if(--w->objects[2][1]==0x48&&w->bell.grabbed!=1){b->action=9;w->sound_requests[0]=0x11;}
                }
                break;
            case 5:
                if(w->frame_counter&1)break;
                if(w->objects[1][0]<0x1d){b->action=6;advance=true;}
                else {--w->objects[1][0];w->objects[2][0]=w->objects[1][0];}
                break;
            case 6:
                if(w->frame_counter&3)break;
                if(w->objects[2][2]==0x21){b->action=7;b->timer=25;advance=true;}else --w->objects[2][2];
                break;
            case 7:
                if(!(w->frame_counter&3)){if(b->timer)--b->timer;else b->action=9;}
                break;
            case 8:if(!(w->frame_counter&3))bell_award_tick(w);break;
            case 9:
                if(w->frame_counter&1) {
                    if(b->player_x==0xe8)b->action=10;
                    else bell_walk(w,-1,0);
                }
                break;
            default:break;
        }
    }
    for(unsigned i=0;i<4;i++)w->objects[3+i][2]=w->objects[2][1]>=0x3e + i*8?0x20:0xff;
    claw_items(w);bonus_sprites(w);
    if(b->action>=4&&b->action<7&&(held&2))b->action=9;
    if(b->action==10)begin_fade(w,WORLD_FADE_BELL_RETURN);
}
static void ladder_gates(World *w) {
    /* 18:446A samples DIV repeatedly until two distinct gate indices <9
     * result. Gameplay supplies the host oscillator; the fallback advances
     * a deterministic fixture clock so rejection sampling always progresses. */
    WorldBell *c=&w->bell;
    if(!(w->frame_counter&7))for(unsigned gate=0;gate<2;gate++) {
        w->objects[5+c->gates[gate]][2]=0xff;
        unsigned candidate;
        do {
            candidate=mix_random(w)&15;
        } while(candidate>=9||candidate==c->gates[gate^1]);
        c->gates[gate]=(uint8_t)candidate;w->objects[5+candidate][2]=0x25;
    } else for(unsigned i=0;i<2;i++)w->objects[5+c->gates[i]][2]=0x25;
}
static void ladder_flash_gates(World *w) {
    for(unsigned i=0;i<2;i++)w->objects[5+w->bell.gates[i]][2]=(w->frame_counter&8)?0x26:0x25;
}
static void ladder_ball(World *w) {
    WorldBell *c=&w->bell;uint8_t *ball=w->objects[14];
    if(c->ball_direction==3) {
        --ball[1];
        if(!(ball[1]&7)) {
            unsigned row=((uint8_t)((ball[1]&0xf8)-0x10))*4;
            unsigned column=(uint8_t)(ball[0]-12)>>3;
            if(w->map[(row+column+1)&0x3ff]==0x3d){c->ball_direction=1;c->ball_distance=32;}
            else if(w->map[(row+column)&0x3ff]==0x3c){c->ball_direction=2;c->ball_distance=32;}
        }
        if(ball[1]<0x2a) {
            w->bonus.action=6;w->objects[15][0]=(uint8_t)(ball[0]-4);
            w->bonus.prize=(uint8_t)(((uint8_t)(ball[0]-0x24)&0x60)>>5);
            ball[2]=0xff;w->objects[15][2]=0x27;
        }
    }
    /* These are independent calls in 18:424B: a newly found turn starts
     * moving horizontally during the same frame. */
    if(c->ball_direction==1){++ball[0];if(!--c->ball_distance)c->ball_direction=3;}
    if(c->ball_direction==2){--ball[0];if(!--c->ball_distance)c->ball_direction=3;}
}
static void bell_ladder_tick(World *w,uint8_t held) {
    /* 18:4028: enter, choose a column, jump, follow ladder, collect, exit. */
    WorldBonus *b=&w->bonus;bool advance=true;
    for(unsigned transitions=0;advance&&transitions<4;transitions++) {
        advance=false;
        switch(b->action) {
            case 0:
                ladder_gates(w);
                if(!(w->frame_counter&1))break;
                if(b->player_x==0x28){b->action=1;b->player_sprite=(uint8_t)bonus_pose(w,w->powerup);advance=true;}
                else bell_walk(w,1,0);
                break;
            case 1:
                ladder_gates(w);
                if(held&16){b->action=4;b->timer=16;advance=true;}
                else if(held&2){b->action=2;b->timer=32;advance=true;}
                else if(held&1){b->action=3;b->timer=32;advance=true;}
                break;
            case 2:case 3: {
                ladder_gates(w);if(!(w->frame_counter&3))break;
                int direction=b->action==2?-1:1;
                if(b->player_x==(direction<0?0x28:0x88)||!b->timer) {
                    b->action=1;b->player_sprite=(uint8_t)bonus_pose(w,w->powerup);
                } else {--b->timer;bell_walk(w,direction,0);}
                break;
            }
            case 4:
                ladder_flash_gates(w);if(!(w->frame_counter&3))break;
                if(b->timer) {
                    --b->timer;b->player_y=(uint8_t)(b->player_y+(int8_t)*asset(w,24,0x421a+b->timer));
                    b->player_sprite=(uint8_t)(bonus_pose(w,w->powerup)+8);
                    if(b->timer==8) {
                        unsigned column=((uint8_t)(b->player_x-0x28)&0x60)>>5;
                        w->overlays[9+column][2]=1;apply_overlay_table(w,0x5213);w->music_request=8;
                    }
                } else {
                    b->action=5;b->player_sprite=(uint8_t)bonus_pose(w,w->powerup);
                    for(unsigned i=0;i<2;i++)w->overlays[w->bell.gates[i]][2]=0;
                    apply_overlay_table(w,0x5213);w->objects[14][0]=(uint8_t)(b->player_x-4);
                    w->objects[14][2]=0x2a;w->bell.ball_direction=3;advance=true;
                }
                break;
            case 5:
                if(!(w->frame_counter&1))break;
                w->sound_requests[2]=4;
                if(w->frame_counter&2) {
                    if(++w->objects[14][2]>=0x2d)w->objects[14][2]=0x2a;
                }
                ladder_ball(w);ladder_flash_gates(w);break;
            case 6: {
                ladder_flash_gates(w);uint8_t *effect=w->objects[15];
                if(effect[2]!=0xff) {
                    if(w->frame_counter&15)break;
                    if(effect[2]==0x27) {
                        unsigned column=(b->prize&3)*4+3;
                        w->map[0x40+column]=w->map[0x41+column]=0xff;
                        w->map[0x60+column]=w->map[0x61+column]=0xff;w->music_request=0x16;
                    }
                    if(effect[2]==0x29)effect[2]=0xff;
                    if(effect[2]!=0xff){++effect[2];break;}
                }
                uint8_t *prize=w->objects[1+b->prize];
                if(++prize[1]>=0x70) {
                    b->action=7;prize[2]=0xff;bell_prize(w);b->timer=20;effect[1]=0x74;advance=true;
                } else {
                    b->player_sprite=(uint8_t)bonus_pose(w,w->powerup);
                    uint8_t target=(uint8_t)(prize[0]+8);
                    if(b->player_x!=target)bell_walk(w,b->player_x<target?1:-1,3);
                }
                break;
            }
            case 7:if(!(w->frame_counter&3))bell_award_tick(w);break;
            case 9:if((w->frame_counter&1)&&!--b->timer)b->action=10;break;
            default:break;
        }
    }
    bonus_sprites(w);
    if(b->action==5&&(held&2)){b->action=9;b->timer=20;}
    if(b->action==10)begin_fade(w,WORLD_FADE_BELL_RETURN);
}
static uint8_t forced_buttons(const World *w,uint8_t pressed) {
    /* 0F:438C: route waypoints advance without requiring another key. */
    static const uint8_t right[]={0x68,0x25,0x6c,0x43,0x79,0x75,0x3f,0x64,0x72};
    static const uint8_t left[]={0x17,0x65,0x45,0x27,0x73,0x74};
    static const uint8_t up[]={0x14,0x4a,0x2f,0x2e,0x6b,0x47,0x5a};
    static const uint8_t down[]={0x4b,0x2c,0x31,0x38,0x69};
    static const uint8_t enter[]={0x67,0x5b,0x11,2,0x0c,0x0b,9,6,0x15};
    for(unsigned i=0;i<sizeof(right);i++)if(w->node==right[i])return 1;
    for(unsigned i=0;i<sizeof(left);i++)if(w->node==left[i])return 2;
    for(unsigned i=0;i<sizeof(up);i++)if(w->node==up[i])return 4;
    for(unsigned i=0;i<sizeof(down);i++)if(w->node==down[i])return 8;
    for(unsigned i=0;i<sizeof(enter);i++)if(w->node==enter[i])return 128;
    return pressed;
}
static bool enter_region(World *w) {
    /* Original entry routines give starting positions; the 48 movement
     * steps belong to 63BA/6426. Entry positions need not equal node-table
     * positions used when returning from a stage. */
    static const struct{uint8_t portal,node,region,x,y,direction;} entries[]={
        {2,0x23,2,0x30,0x9e,4},{0x19,0x23,2,0x30,0x9e,4},
        {9,0x28,1,0x53,0x89,4},{0x1c,0x28,1,0x53,0x89,4},
        {6,0x32,3,0xe2,0x76,1},{0x1b,0x32,3,0xe2,0x76,1},
        {0x12,0x3c,4,0x10,0x64,1},{0x0d,0x46,5,0x4b,0xf2,8},
        {0x67,0x4b,5,0x92,0xea,8},{0x0b,0x50,6,0x0c,0x70,1},
        {0x1d,0x50,6,0x0c,0x70,1},{0x15,0x72,8,0xd4,0x83,1},
        {0x11,0x5a,7,0x81,0x89,2}
    };
    for(unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(w->node==entries[i].portal) {
        w->fade_region=entries[i].region;
        /* Bonus island's wrapper installs coordinates after its fade. The
         * other wrappers update logical position while retaining old OAM. */
        if(w->fade_region!=8){w->node=entries[i].node;w->x=entries[i].x;w->y=entries[i].y;}
        w->scene_direction=entries[i].direction;w->return_processed=true;
        if(w->fade_region==3)w->segment=0x21;
        if(w->fade_region==5)w->segment&=15;
        if(w->fade_region==1||w->fade_region==7||w->fade_region==8){w->music_request=0xff;audio_immediate(w);}
        begin_fade(w,WORLD_FADE_REGION_ENTRY);
        return true;
    }
    return false;
}
static void zone_transition_prepare(World *w);
static void complete_region_entry(World *w) {
    if(w->fade_region!=1&&w->fade_region!=7&&w->fade_region!=8){w->music_request=0xff;audio_immediate(w);}
    w->region=w->fade_region;
    if(w->region==8){w->node=0x72;w->x=0xd4;w->y=0x83;w->segment&=15;}
    w->movement_cadence=0x92;setup_assets(w);
    w->moving=w->region==3;w->scene=WORLD_ZONE_ENTRY;w->scene_timer=0;
    w->sprite_palette[0]=0;
    if(w->region!=7)w->background_palette=w->sprite_palette[1]=0;
    /* 3E46 clears the cursor until 6426 moves it; regional initializers
     * own object OAM and may update object state after their final draw. */
    if(w->region==6)w->objects[5][0]=(uint8_t)(0x38+((w->progress[0x1a]&0x80)?0x18:0));
    if(w->region==7)w->sprite_palette[0]=0xd0; /* 64C1 */
    zone_transition_prepare(w);
}
static bool exit_region(World *w,uint8_t pressed) {
    /* 0F:466C–47BD: fixed zone exits owned by the original dispatcher. */
    static const struct{uint8_t node,button,destination,segment;} exits[]={
        {0x23,8,0x19,8},{0x28,8,0x1c,8},{0x2d,0,0x79,8},{0x30,0,0x7a,8},
        {0x32,2,0x1b,0x22},{0x40,0,0x1a,2},{0x46,4,0x1e,4},
        {0x4a,0,0x65,4},{0x50,2,0x1d,2},{0x5c,1|8,0x64,1},{0x74,2,0x17,2}
    };
    for(unsigned i=0;i<sizeof(exits)/sizeof(exits[0]);i++)if(w->node==exits[i].node) {
        if(exits[i].button&&!(pressed&exits[i].button))return false;
        w->scene=WORLD_ZONE_EXIT;w->scene_timer=0;w->scene_destination=exits[i].destination;
        w->audio_clock=WORLD_AUDIO_SKIP;
        w->scene_direction=exits[i].segment&15;w->scene_segment=exits[i].segment;
        zone_transition_prepare(w);return true;
    }
    return false;
}
static void map_clock(World *w) {
    ++w->frame_counter;
    /* 0F:413E is called by animation work, not by a palette-only wait. */
    if(!--w->decoration.scheduler) {
        w->decoration.scheduler=40;w->decoration.random_pose^=sample_divider(w);
    }
}
static void zone_transition_prepare(World *w) {
    bool entry=w->scene==WORLD_ZONE_ENTRY;
    unsigned limit=entry&&w->region==7?96:100;
    if(w->scene_timer>=limit) {
        w->scene=WORLD_MAP;w->moving=false;
        if(!entry) {
            w->node=w->scene_destination;node_coordinates(w);w->region=0;w->segment=w->scene_segment;
            w->movement_cadence=0;setup_assets(w);w->background_palette=0;begin_fade(w,WORLD_FADE_MAP_READY);
        }
        return;
    }
    unsigned tick=w->scene_timer++;
    bool palette_wait=entry?w->region!=7&&tick<20&&tick%5==0:tick>=80&&(tick-80)%5==0;
    bool move;
    if(entry&&w->region==7)move=!(tick&1);
    else if(entry)move=tick<20?(tick%5==1||tick%5==3):!(tick&1);
    else move=tick<80?!(tick&1):((tick-80)%5==1||(tick-80)%5==3);
    if(move) {
        ++w->animation;
        switch(w->scene_direction) {
            case 1:++w->x;w->pose=(uint8_t)(4+((w->animation&6)>>1));break;
            case 2:--w->x;w->pose=(uint8_t)((w->animation&6)>>1);break;
            case 4:--w->y;w->pose=(uint8_t)(8+((w->animation&12)>>2));break;
            case 8:++w->y;w->pose=(uint8_t)(12+((w->animation&6)>>1));break;
        }
    }
    if(entry&&w->region!=7&&tick<20&&tick%5==1) {
        unsigned step=tick/5;
        w->background_palette=(uint8_t)((w->background_palette>>2)|(((0xe4>>(step*2))&3)<<6));
        w->sprite_palette[0]=w->sprite_palette[1]=*asset(w,15,0x6a54+step);
    }
    if(!entry&&tick>=80&&(tick-80)%5==1) {
        unsigned step=(tick-80)/5;
        w->background_palette=(uint8_t)(0xe4<<(step*2));
        w->sprite_palette[0]=w->sprite_palette[1]=*asset(w,15,0x6a57-step);
    }
    if(move){w->sprite_count=0;cursor_sprites(w);}
    if(!palette_wait){map_clock(w);animate_region_objects(w);rebuild_object_sprites(w);}
}
static void zone_transition_tick(World *w) {
    animate_region_tiles(w); /* VBlank precedes the resumed 69EC caller. */
    zone_transition_prepare(w);
    for(unsigned i=0;i<w->audio_event_count;++i)w->audio_events[i].after_tick=true;
    w->audio_clock=w->scene==WORLD_MAP?WORLD_AUDIO_WAIT_THEN_MAIN:WORLD_AUDIO_MAIN;
}
static void fade_tick(World *w) {
    if(!w->region)animate_water(w);else animate_region_tiles(w);
    bool fade_in=w->fade==WORLD_FADE_MAP_READY;
    unsigned interval=fade_in?3:12,limit=4*interval;
    if(++w->scene_timer%interval==0) {
        unsigned step=w->scene_timer/interval-1;
        if(fade_in) {
            w->background_palette=(uint8_t)((w->background_palette>>2)|(((0xe4>>(step*2))&3)<<6));
            w->sprite_palette[0]=*asset(w,15,0x6a5c+step*2);w->sprite_palette[1]=*asset(w,15,0x6a5d+step*2);
        } else {
            w->background_palette=(uint8_t)(0xe4<<(step*2));
            w->sprite_palette[0]=w->sprite_palette[1]=*asset(w,15,0x6a57-step);
        }
    }
    if(w->scene_timer!=limit)return;
    if(!fade_in)memset(w->map,0xff,0x400); /* 69FB clears the visible tile map. */
    switch(w->fade) {
        case WORLD_FADE_STAGE:
            save_now(w);
            w->stage_requested=true;
            break;
        case WORLD_FADE_BELL_RETURN:world_return(w,w->bell.result);break;
        case WORLD_FADE_REGION_ENTRY:complete_region_entry(w);break;
        case WORLD_FADE_MAP_READY:w->scene=WORLD_MAP;break;
    }
    for(unsigned i=0;i<w->audio_event_count;++i)w->audio_events[i].after_tick=true;
    /* The final 69EC consumes audio before its continuation; returning to the
     * dispatcher consumes it again through main2089. Chained waits do not. */
    bool waiting=(w->scene==WORLD_FADE&&!w->stage_requested)||w->scene==WORLD_ZONE_ENTRY||w->scene==WORLD_ZONE_EXIT;
    w->audio_clock=waiting?WORLD_AUDIO_MAIN:WORLD_AUDIO_WAIT_THEN_MAIN;
}
static void rebuild_navigation_sprites(World *w) {
    memset(w->sprites,0,16);w->sprite_count=0;cursor_sprites(w);
    if(w->region)rebuild_object_sprites(w);
}
/* 0F404D animates the main map after438C input, before4169 selection. */
static uint8_t navigate_map(World *w,uint8_t held,uint8_t pressed) {
    if(w->moving){advance_path(w);rebuild_navigation_sprites(w);return 0;}
    if(w->node==0&&(w->progress[0]&0x80))w->node=0x14;
    pressed=forced_buttons(w,pressed);
    /* 0F:42B8–4315: secret branches retain their own source node identity. */
    if(w->node==0x35&&(pressed&1)&&(w->progress[0x0d]&0x80))w->node=0x36;
    else if(w->node==0x35&&(pressed&2)&&(w->progress[0x23]&0x80))w->node=0x37;
    if(w->node==0x32&&(pressed&4)&&(w->progress[0x0b]&1))w->node=0x39;
    if(w->node==0x6a&&(pressed&1)&&(w->progress[0x23]&0x80))w->node=0x6c;
    uint8_t slot,stage;
    if(w->region&&!w->return_processed) {
        w->return_processed=true;
        if(world_node_progress_slot(w,w->node,&slot)) {
            unsigned previous=*asset(w,15,0x4364+slot);
            if(previous<WORLD_PROGRESS_COUNT&&!(w->progress[previous]&0x80))
                pressed=(uint8_t)(*asset(w,15,0x6591+slot)>>4);
        }
    }
    bool movement_allowed=true;
    if(w->region&&world_node_stage(w,w->node,&stage)&&world_node_progress_slot(w,w->node,&slot)) {
        uint8_t blocked=(uint8_t)(*asset(w,15,0x6591+slot)>>4);
        if((held&blocked)&&!(w->progress[slot]&0x80))movement_allowed=false;
    }
    if(movement_allowed&&(w->node==0x0c||w->node==0x1e)&&
       (pressed&(16|128))&&!(pressed&(1|2|4|8|32|64))) {
        /* 0F:44E7's ferry path is explicitly outside the node route table. */
        w->boat_counter=1;w->music_request=0x19;w->path=0x5bf6;
        w->segment=*asset(w,24,w->path);w->moving=true;w->segment_timer=9;
        advance_path(w);rebuild_navigation_sprites(w);return 0;
    }
    if(!w->region&&movement_allowed) {
        /* 0F:45B2: hold B to inspect the whole map with its arrow cursor.
         * Releasing B restores the selected node's camera and graphics. */
        if(held&32) {
            w->peeking=true;
            if((held&1)&&w->camera_x<0x160)++w->camera_x;
            if((held&2)&&w->camera_x)--w->camera_x;
            if((held&4)&&w->camera_y)--w->camera_y;
            if((held&8)&&w->camera_y<0x170)++w->camera_y;
            rebuild_navigation_sprites(w);memcpy(w->sprites,asset(w,15,(w->global_frame&16)?0x4a21:0x4a11),16);return 0;
        }
        if(w->peeking){w->peeking=false;node_coordinates(w);setup_assets(w);}
    }
    if(movement_allowed&&(pressed&15)) {
        unsigned direction=(pressed&2)?1:(pressed&4)?2:(pressed&8)?3:0;
        if(start_path(w,direction)){rebuild_navigation_sprites(w);return 0;}
    }
    if(w->node==0x0d&&w->boat_counter)pressed=128;
    if((pressed&(16|128))&&bonus_enter(w))return 0;
    if((pressed&(16|128))&&(!w->region||w->region==8)&&enter_region(w)) {
        return 0;
    }
    if(w->region&&exit_region(w,pressed))return 0;
    ++w->animation;w->pose=(uint8_t)(12+((w->animation&24)>>3));
    rebuild_navigation_sprites(w);
    return pressed;
}
void world_step(World *w,uint8_t held,uint8_t pressed) {
    w->save_requested=false;w->audio_event_count=0;w->audio_clock=WORLD_AUDIO_MAIN;
    if(w->scene==WORLD_ENDING&&w->ending.phase==2)
        w->audio_clock=w->scene_action>=5?WORLD_AUDIO_BEFORE_REQUESTS:WORLD_AUDIO_SKIP;
    if(w->stage_requested||w->asset_error)return;
    ++w->frames;w->entropy_samples=0;memset(w->sound_requests,0,sizeof(w->sound_requests));
    /* 69EC waits inside a scene routine; it does not execute main02FF.
     * FF97 stays frozen while VBlank-owned graphics continue changing. */
    if(w->scene!=WORLD_ZONE_ENTRY&&w->scene!=WORLD_ZONE_EXIT&&w->scene!=WORLD_FADE)
        ++w->global_frame;
    if(w->scene==WORLD_FADE) {
        fade_tick(w);
        return;
    }
    if(w->scene==WORLD_CASTLE_GATE){castle_tick(w,pressed);return;}
    if(w->scene==WORLD_GAME_OVER){game_over_tick(w,pressed);return;}
    if(w->scene==WORLD_COIN_AWARD){coin_award_tick(w);return;}
    if(w->scene==WORLD_ENDING){ending_tick(w);return;}
    if(w->scene==WORLD_ZONE_ENTRY||w->scene==WORLD_ZONE_EXIT){zone_transition_tick(w);return;}
    /* 404D calls6D4C before438C moves the camera. Object composition uses
     * that snapshot; scrolling background coordinates advance with the path. */
    if(w->scene==WORLD_MAP&&!w->region) {
        w->object_camera_x=w->camera_x;w->object_camera_y=w->camera_y;
    }
    /* VBlank runs6C7D/6CF5/6D35 before main413E advances A69A. */
    if(w->scene==WORLD_MAP&&w->region)animate_region_tiles(w);
    if(w->region||w->scene!=WORLD_MAP)map_clock(w);
    if(w->scene==WORLD_BONUS){bonus_tick(w,pressed);return;}
    if(w->scene==WORLD_BELL_CLAW){bell_claw_tick(w,held,pressed);return;}
    if(w->scene==WORLD_BELL_LADDER){bell_ladder_tick(w,held);return;}
    bool main_map=!w->region;
    if(!main_map)animate_region_objects(w);
    pressed=navigate_map(w,held,pressed);
    if(main_map&&!w->region&&w->scene==WORLD_MAP){map_clock(w);animate_main_map(w);rebuild_main_map_sprites(w);}
    if(w->scene!=WORLD_MAP)return;
    uint8_t slot,stage;
    if(pressed&(16|128)) {
        if(world_node_stage(w,w->node,&stage)) {
            bool cleared_boss=false;
            if(world_node_progress_slot(w,w->node,&slot)) {
                static const uint8_t bosses[]={5,9,14,17,23,29};
                for(unsigned i=0;i<6;i++)if(slot==bosses[i]&&(w->progress[slot]&0x80))cleared_boss=true;
                w->stage_flags=w->progress[slot];
            } else w->stage_flags=0;
            if(!cleared_boss){
                w->stage=stage;begin_fade(w,WORLD_FADE_STAGE);
                w->music_request=0xff;audio_immediate(w);w->sound_requests[0]=0x16;
            }
        }
    }
    if(w->scene==WORLD_FADE&&w->region!=3) {
        static const uint8_t selected[]={0x1b,0,0x0a,0x20,0x1c,0,0x0b,0x20};
        for(unsigned i=0;i<4;++i)memcpy(w->sprites+i*4+2,selected+i*2,2);
    }
}
static uint8_t pixel(const World *w,unsigned tile,unsigned x,unsigned y,bool sprite) {
    unsigned at=sprite?tile*16:(unsigned)(0x1000+(int8_t)tile*16);
    at+=y*2;unsigned shift=7-x;
    return (uint8_t)(((w->tiles[at]>>shift)&1)|(((w->tiles[at+1]>>shift)&1)<<1));
}
void world_render(const World *w,uint8_t shades[160*144]) {
    uint8_t colors[160*144];
    bool scrolling=(w->scene==WORLD_MAP||w->scene==WORLD_FADE)&&!w->region;
    for(unsigned y=0;y<144;y++)for(unsigned x=0;x<160;x++) {
        unsigned px=x+(scrolling?w->camera_x:0),py=y+(scrolling?w->camera_y:0);
        if(w->region==4&&(w->scene==WORLD_MAP||w->scene==WORLD_ZONE_ENTRY||w->scene==WORLD_ZONE_EXIT||w->scene==WORLD_FADE)) {
            px+=w->decoration.shake_x;py+=w->decoration.shake_y;
        }
        if(w->scene==WORLD_ENDING) {
            py+=w->ending.render_scroll_y;
            if(w->ending.wave_first&&y>=w->ending.wave_first&&y<=w->ending.wave_last) {
                unsigned wave_y=w->ending.wave_hold&&y>w->ending.wave_hold?w->ending.wave_hold:y;
                px=(uint8_t)(px+*asset(w,26,0x444f+((((w->global_frame&0x3c)>>2)+wave_y-1)&15)));
            }
        }
        if(w->region==7&&(w->scene==WORLD_MAP||w->scene==WORLD_ZONE_ENTRY||w->scene==WORLD_ZONE_EXIT||w->scene==WORLD_FADE)&&y<31)
            px+=w->decoration.cloud_scroll[y<15?0:y<23?1:2];
        unsigned tile=w->map[((py/8)%w->map_stride)*w->map_stride+(px/8)%w->map_stride];
        if(scrolling&&y>=136&&x>=73)tile=0xe1;
        bool unsigned_tiles=w->scene==WORLD_ENDING&&w->ending.unsigned_lower_tiles&&y>=112;
        uint8_t color=pixel(w,tile,px&7,py&7,unsigned_tiles);
        colors[y*160+x]=color;shades[y*160+x]=(w->background_palette>>(color*2))&3;
    }
    SpriteList sprites={.count=w->sprite_count};
    memcpy(sprites.bytes,w->sprites,sizeof(w->sprites));
    sprites_render(&sprites,w->tiles,colors,shades,w->sprite_palette[0],w->sprite_palette[1]);
}
void world_export_record(const World *w,uint8_t record[0x50]) {
    memcpy(record,w->record,0x50);record[0]=w->node;record[2]=w->x;record[3]=w->y;record[4]=w->segment;
    memcpy(record+8,w->progress,WORLD_PROGRESS_COUNT);record[0x38]=w->movement_cadence;
    record[0x40]=(uint8_t)(((w->coins/10%10)<<4)|(w->coins%10));
    record[0x41]=(uint8_t)(w->coins/100%10);
    record[0x43]=(uint8_t)(((w->lives/10%10)<<4)|(w->lives%10));
    uint8_t checksum=0;for(unsigned i=0;i<0x45;i++)checksum=(uint8_t)(checksum+record[i]);
    record[0x45]=checksum;
}
