/* Readable bank04 music/command driver. ROM bytes are music data and tables;
 * the native runtime does not decode or execute Game Boy instructions. */
#include "audio_driver.h"
#include "audio_sfx.h"
#include <string.h>

uint8_t audio_driver_byte(AudioDriver *d,unsigned address) {
    size_t offset=address<0x4000?address:4u*0x4000u+(address&0x3fff);
    if(address>=0x8000||offset>=d->rom_size){d->error=true;return 0;}
    return d->rom[offset];
}
uint16_t audio_driver_word(AudioDriver *d,unsigned address) {
    uint8_t lo=audio_driver_byte(d,address),hi=audio_driver_byte(d,address+1);
    return (uint16_t)(lo|hi<<8);
}
void audio_driver_write(AudioDriver *d,uint8_t reg,uint8_t value) {
    if(reg<0x10||reg>0x3f){d->error=true;return;}
    d->registers[reg-0x10]=value;if(d->emit)d->emit(d->emit_context,reg,value);
}
static void silence(AudioDriver *d) {
    audio_driver_write(d,0x12,8);audio_driver_write(d,0x17,8);audio_driver_write(d,0x21,8);
    audio_driver_write(d,0x14,0x80);audio_driver_write(d,0x19,0x80);audio_driver_write(d,0x23,0x80);
    audio_driver_write(d,0x10,0);audio_driver_write(d,0x1a,0);
}
static void clear_voices(AudioDriver *d) {
    memset(d->active_sfx,0,sizeof(d->active_sfx));
    for(unsigned i=0;i<4;i++)d->channel[i].loop_count=0;
    d->pause_request=0;d->work.wave_envelope=0;d->work.auxiliary=0;
    memset(d->work.rise,0,sizeof(d->work.rise));memset(d->work.fall,0,sizeof(d->work.fall));
    d->work.vibrato[0]=(AudioDriverPair){0};d->work.vibrato[1]=(AudioDriverPair){0};
    d->work.step[0]=d->work.step[1]=d->work.step[2]=d->work.step[4]=0;
    memset(d->work.delay,0,sizeof(d->work.delay));memset(d->work.priority,0,sizeof(d->work.priority));
    d->work.volume_duck=0;silence(d);
}
void audio_driver_stop_music(AudioDriver *d) {
    audio_driver_write(d,0x25,0xff);d->stereo_mode=3;d->music=0;clear_voices(d);
}
bool audio_driver_init(AudioDriver *d,const uint8_t *rom,size_t size,AudioRegisterWrite emit,void *context) {
    if(!d||!rom||size<5*0x4000u)return false;
    memset(d,0,sizeof(*d));d->rom=rom;d->rom_size=size;d->emit=emit;d->emit_context=context;
    audio_driver_reset(d);return true;
}
void audio_driver_reset(AudioDriver *d) {
    const uint8_t *rom=d->rom;size_t size=d->rom_size;
    AudioRegisterWrite emit=d->emit;void *context=d->emit_context;
    uint8_t registers[48];memcpy(registers,d->registers,sizeof(registers));
    memset(d,0,sizeof(*d));d->rom=rom;d->rom_size=size;d->emit=emit;d->emit_context=context;
    memcpy(d->registers,registers,sizeof(registers));
    audio_driver_write(d,0x26,0x80);audio_driver_write(d,0x24,0x77);audio_driver_write(d,0x25,0xff);
}
static void music_start(AudioDriver *d) {
    if(d->music_request==0xff){audio_driver_stop_music(d);return;}
    if(d->music_request>=0x2b)return;
    unsigned data=audio_driver_word(d,0x5244+2*(d->music_request-1));
    clear_voices(d);
    switch(d->music_request) {
        case 2:case 3:case 12:case 15:case 24:d->work.hurry=0;break;
        case 9:d->work.hurry=1;break;
        default:break;
    }
    d->transpose=(uint8_t)(audio_driver_byte(d,data)+(d->work.hurry?4:0));
    d->duration_table=audio_driver_word(d,data+(d->work.hurry?3:1));
    for(unsigned i=0;i<4;i++) {
        AudioMusicChannel *c=&d->channel[i];c->pattern=audio_driver_word(d,data+5+2*i);
        c->note=audio_driver_word(d,c->pattern);c->delay=1;
        if(i<3)c->age=0;
    }
    unsigned stereo=0x52f7+6*(d->music-1);
    d->stereo_mode=audio_driver_byte(d,stereo);d->stereo_clock=0;
    d->stereo_period=audio_driver_byte(d,stereo+1);d->stereo_phase=0;
    for(unsigned i=0;i<4;i++)d->stereo_masks[i]=audio_driver_byte(d,stereo+2+i);
    audio_driver_write(d,0x25,d->stereo_masks[0]);
}
static void wave_upload(AudioDriver *d,unsigned source) {
    for(unsigned i=0;i<16;i++)audio_driver_write(d,(uint8_t)(0x30+i),audio_driver_byte(d,source+i));
}
static void modulation_write(AudioDriver *d,unsigned channel,uint16_t frequency) {
    unsigned reg=channel==0?0x13:channel==1?0x18:0x1d;
    d->work.mod_frequency[channel]=(uint16_t)(frequency&0xfff);
    audio_driver_write(d,(uint8_t)reg,(uint8_t)frequency);audio_driver_write(d,(uint8_t)(reg+1),(uint8_t)((frequency>>8)&15));
}
static void pitch_modulation(AudioDriver *d,unsigned channel) {
    AudioMusicChannel *c=&d->channel[channel];unsigned kind=c->instrument[2]&15;
    if(kind)d->modulation=(uint8_t)kind;
    if(channel==3)return;
    if(kind) {
        if(kind==1) {
            AudioDriverPair *v=&d->work.vibrato[channel];++v->state;
            if(v->state==25)v->step=1;
            else if(v->state==45)v->state=25;
            if(v->step) {
                unsigned at=0x5acc+2*(uint8_t)(v->state-25);
                int16_t delta=(int16_t)((audio_driver_byte(d,at)<<8)|audio_driver_byte(d,at+1));
                modulation_write(d,channel,(uint16_t)((c->frequency&0xfff)+delta));
            }
        } else modulation_write(d,channel,(uint16_t)((c->frequency&0xfff)-1));
    }
    AudioDriverPair *rise=&d->work.rise[channel],*fall=&d->work.fall[channel];
    if(rise->state) {
        if(rise->state==2)return;
        if(!rise->step){rise->step=1;modulation_write(d,channel,(uint16_t)((c->frequency&0xfff)-4));}
        else if(rise->step==5){rise->state=2;rise->step=0;}
        else {++rise->step;modulation_write(d,channel,(uint16_t)(d->work.mod_frequency[channel]+1));}
    } else if(fall->state) {
        unsigned frequency=fall->step?d->work.mod_frequency[channel]:(c->frequency&0xfff);
        fall->step=1;modulation_write(d,channel,(uint16_t)(frequency-100));
    } else if(channel==2&&d->work.wave_envelope&&d->work.wave_envelope!=2) {
        unsigned age=++d->work.rise[2].step,volume=0;
        if(age==3)volume=0x40;
        else if(age==5||age==10)volume=0x60;
        else if(age!=7&&age!=13)return;
        if(age==13)d->work.wave_envelope=2;
        audio_driver_write(d,0x1c,(uint8_t)volume);
    }
}
static void gate_updates(AudioDriver *d) {
    /* 04:5899 checks both pulse voices whenever any eligible voice sustains. */
    for(unsigned i=0;i<2;i++) {
        AudioMusicChannel *c=&d->channel[i];unsigned envelope=c->instrument[1]&15;
        if(c->rest||!c->instrument[1]||c->age!=d->gate_time[i]||(c->loop_count&0x80))continue;
        audio_driver_write(d,(uint8_t)(i?0x17:0x12),audio_driver_byte(d,0x5cac+(uint8_t)(envelope-1)));
        audio_driver_write(d,(uint8_t)(i?0x19:0x14),(uint8_t)((c->frequency>>8)|0x80));
    }
}
static void sustain(AudioDriver *d,unsigned channel) {
    AudioMusicChannel *c=&d->channel[channel];
    if(channel==2&&(c->instrument[2]&0x80)&&c->delay==6)audio_driver_write(d,0x1c,0x40);
    if(c->rest||(c->loop_count&0x80))return;
    pitch_modulation(d,channel);
    gate_updates(d);
}
static void play_note(AudioDriver *d,unsigned channel,uint8_t note) {
    AudioMusicChannel *c=&d->channel[channel];
    if(channel<3) {
        if(d->work.rise[channel].state)d->work.rise[channel]=(AudioDriverPair){1,0};
        d->work.vibrato[channel]=(AudioDriverPair){0};d->work.fall[channel].step=0;
        if(channel==2&&d->work.wave_envelope){d->work.wave_envelope=1;d->work.rise[2].step=0;}
        c->rest=note==1;
        if(!c->rest)c->frequency=audio_driver_word(d,(unsigned)(0x5af4+note+(int8_t)d->transpose));
    } else if(note==0x1f) {
        if(!(c->loop_count&0x80)){d->request[2]=3;audio_sfx_step(d,2);}
        c->delay=c->duration;return;
    } else {
        unsigned at=0x5c06+note;
        for(unsigned i=0;i<3;i++)c->instrument[i]=audio_driver_byte(d,at+i);
        c->frequency=audio_driver_word(d,at+3);
    }
    unsigned reg=channel==0?0x11:channel==1?0x16:channel==2?0x1b:0x20;
    unsigned envelope=c->instrument[0],duty=c->instrument[2];
    if(channel==2) {
        if(!(c->loop_count&0x80)){audio_driver_write(d,0x1a,0);audio_driver_write(d,0x1a,0x80);}
        envelope=c->instrument[2];duty=0;
    }
    if(c->rest)envelope=8;
    c->age=0;
    if(!(c->loop_count&0x80)) {
        audio_driver_write(d,(uint8_t)reg,(uint8_t)duty);audio_driver_write(d,(uint8_t)(reg+1),(uint8_t)envelope);
        audio_driver_write(d,(uint8_t)(reg+2),(uint8_t)c->frequency);
        audio_driver_write(d,(uint8_t)(reg+3),(uint8_t)((c->frequency>>8)|0x80));
    }
    c->delay=c->duration;
}
static bool next_note(AudioDriver *d,unsigned channel) {
    AudioMusicChannel *c=&d->channel[channel];
    for(unsigned commands=0;commands<0x4000&&!d->error;commands++) {
        uint8_t command=audio_driver_byte(d,c->note);
        if(!command) {
            c->pattern=(uint16_t)(c->pattern+2);
            for(unsigned links=0;links<0x2000;links++) {
                c->note=audio_driver_word(d,c->pattern);
                if(!(c->note>>8)){audio_driver_stop_music(d);return false;}
                if((c->note>>8)!=0xff)break;
                c->pattern=audio_driver_word(d,c->pattern+2);
                if(links==0x1fff){d->error=true;return false;}
            }
            continue;
        }
        ++c->note;
        switch(command) {
            case 0x9d: {
                uint8_t instrument[3];for(unsigned i=0;i<3;i++)instrument[i]=audio_driver_byte(d,c->note++);
                if(!d->suppress_instrument)memcpy(c->instrument,instrument,3);
                if(channel==2&&!d->active_sfx[1]){audio_driver_write(d,0x1a,0);wave_upload(d,instrument[0]+256*instrument[1]);}
                continue;
            }
            case 0x9e:d->duration_table=audio_driver_word(d,c->note);c->note+=2;continue;
            case 0x9f:d->transpose=audio_driver_byte(d,c->note++);continue;
            case 0x9b:c->loop_count|=audio_driver_byte(d,c->note++);c->loop_start=c->note;continue;
            case 0x9c:if(--c->loop_count&0x7f)c->note=c->loop_start;continue;
            case 0x99:d->work.wave_envelope=1;continue;
            case 0x9a:d->work.wave_envelope=0;continue;
            case 0x97:d->work.fall[channel].state=1;continue;
            case 0x98:d->work.fall[channel].state=0;continue;
            case 0x95:d->work.rise[channel].state=1;continue;
            case 0x96:d->work.rise[channel].state=0;continue;
            default:break;
        }
        if((command&0xf0)==0xa0) {
            c->duration=audio_driver_byte(d,d->duration_table+(command&15));
            unsigned shape=c->instrument[1]&0xf0;
            if(shape) {
                unsigned threshold=shape&0x80?c->duration/2:shape&0x40?c->duration/4:c->duration/2+c->duration/4;
                d->gate_time[channel]=(uint8_t)(threshold?threshold:2);
            }
            command=audio_driver_byte(d,c->note++);
        }
        play_note(d,channel,command);return true;
    }
    d->error=true;return false;
}
static void music_tick(AudioDriver *d) {
    if(!d->music)return;
    if(d->stereo_mode!=1&&++d->stereo_clock==d->stereo_period) {
        d->stereo_clock=0;++d->stereo_phase;audio_driver_write(d,0x25,d->stereo_masks[d->stereo_phase&3]);
    }
    for(unsigned i=0;i<4;i++) {
        AudioMusicChannel *c=&d->channel[i];d->current_channel=(uint8_t)(i+1);
        if(!(c->pattern>>8))continue;
        if(--c->delay)sustain(d,i);else if(!next_note(d,i))return;
    }
    for(unsigned i=0;i<3;i++)++d->channel[i].age;
}
static void pause_note(AudioDriver *d,unsigned at) {
    for(unsigned i=0;i<4;i++)audio_driver_write(d,(uint8_t)(0x16+i),audio_driver_byte(d,at+i));
    d->work.delay[1]=audio_driver_byte(d,at+4);
}
void audio_driver_step(AudioDriver *d) {
    if(!d||d->error)return;
    if(d->pause_request==1) {
        silence(d);memset(d->active_sfx,0,sizeof(d->active_sfx));d->work.step[1]=0;d->paused=true;
        for(unsigned i=0;i<4;i++)d->channel[i].loop_count&=0x7f;
        pause_note(d,0x5970);
    } else {
        if(d->pause_request==2) {
            d->paused=false;d->work.priority[1]=d->work.priority[3]=d->work.priority[4]=0;
        }
        if(d->paused) {
            if(!--d->work.delay[1]) {
                if(++d->work.step[1]==6){d->work.delay[1]=0xf0;d->work.step[1]=5;}
                else pause_note(d,audio_driver_word(d,0x5966+2*(d->work.step[1]-1)));
            }
        } else {
            if(d->music_request){d->music=d->music_request;music_start(d);}
            else {
                audio_driver_write(d,0x24,d->work.volume_duck?0x33:0x77);
                for(unsigned i=0;i<3;i++)audio_sfx_step(d,i);
            }
            music_tick(d);
        }
    }
    memset(d->request,0,sizeof(d->request));d->music_request=0;d->pause_request=0;
}
