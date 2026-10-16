/* Approximate native DMG sound synthesis and optional ALSA output.
 * The readable C game sound driver emits registers through audio_write_register.
 * Writes have frame precision; hardware quirks and analogue behavior differ.
 * Register meanings: https://gbdev.io/pandocs/Audio_Registers.html
 */
#include "audio.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool enabled,length_enabled;
    unsigned length,volume,envelope_timer,envelope_period;
    int envelope_direction;
    unsigned frequency,phase;
    double timer;
} Voice;
typedef struct {
    uint8_t reg[0x30];
    Voice voice[4];
    double sequencer_timer;
    unsigned sequencer_step,sweep_timer,sweep_shadow;
    bool sweep_enabled;
    uint16_t noise_bits;
} Synth;
static Synth synth;
static uint64_t sample_remainder;
static double previous_input[2],previous_output[2];
static snd_pcm_t *pcm;
static int16_t pending[8192*2];
static size_t pending_frames;

static void synth_reset(Synth *s) {
    memset(s,0,sizeof(*s));
    s->sequencer_timer=8192;
    s->noise_bits=0x7fff;
    for(unsigned i=0;i<4;i++)s->voice[i].timer=1;
}

static unsigned sweep_result(Synth *s) {
    unsigned shift=s->reg[0]&7;
    unsigned amount=s->sweep_shadow>>shift;
    return (s->reg[0]&8)?s->sweep_shadow-amount:s->sweep_shadow+amount;
}

static double voice_period(const Synth *s,unsigned channel) {
    if(channel<2)return 4.0*(2048-s->voice[channel].frequency);
    if(channel==2)return 2.0*(2048-s->voice[channel].frequency);
    unsigned polynomial=s->reg[0x12];
    unsigned divisor=polynomial&7;
    return (double)((divisor?divisor*16:8)<<(polynomial>>4));
}

static void trigger(Synth *s,unsigned channel) {
    static const uint8_t envelopes[4]={2,7,12,17};
    Voice *v=&s->voice[channel];
    uint8_t envelope=s->reg[envelopes[channel]];
    v->enabled=channel==2?(s->reg[10]&0x80)!=0:(envelope&0xf8)!=0;
    if(!v->length)v->length=channel==2?256:64;
    v->volume=envelope>>4;
    v->envelope_period=envelope&7;
    v->envelope_timer=v->envelope_period?v->envelope_period:8;
    v->envelope_direction=(envelope&8)?1:-1;
    v->timer=voice_period(s,channel);
    if(channel==2)v->phase=0;
    if(channel==3)s->noise_bits=0x7fff;
    if(channel==0) {
        unsigned period=(s->reg[0]>>4)&7;
        s->sweep_shadow=v->frequency;
        s->sweep_timer=period?period:8;
        s->sweep_enabled=period||(s->reg[0]&7);
        if((s->reg[0]&7)&&sweep_result(s)>2047)v->enabled=false;
    }
}

static void synth_write(Synth *s,uint8_t reg,uint8_t value) {
    if(reg<0x10||reg>0x3f)return;
    unsigned r=reg-0x10;
    s->reg[r]=value;
    if(reg==0x26) {
        if(!(value&0x80))synth_reset(s);
        return;
    }
    unsigned channel;
    switch(reg) {
        case 0x11: s->voice[0].length=64-(value&63);break;
        case 0x16: s->voice[1].length=64-(value&63);break;
        case 0x1b: s->voice[2].length=256-value;break;
        case 0x20: s->voice[3].length=64-(value&63);break;
        case 0x12: if(!(value&0xf8))s->voice[0].enabled=false;break;
        case 0x17: if(!(value&0xf8))s->voice[1].enabled=false;break;
        case 0x1a: if(!(value&0x80))s->voice[2].enabled=false;break;
        case 0x21: if(!(value&0xf8))s->voice[3].enabled=false;break;
        case 0x13: case 0x18: case 0x1d:
            channel=(reg-0x13)/5;
            s->voice[channel].frequency=value|((s->reg[r+1]&7)<<8);
            break;
        case 0x14: case 0x19: case 0x1e: case 0x23:
            channel=(reg-0x14)/5;
            s->voice[channel].length_enabled=(value&0x40)!=0;
            if(channel<3)s->voice[channel].frequency=s->reg[r-1]|((value&7)<<8);
            if(value&0x80)trigger(s,channel);
            break;
        default: break;
    }
}

static void sequencer_tick(Synth *s) {
    unsigned step=s->sequencer_step;
    if(!(step&1)) {
        for(unsigned i=0;i<4;i++) {
            Voice *v=&s->voice[i];
            if(v->length_enabled&&v->length&&!--v->length)v->enabled=false;
        }
    }
    if((step==2||step==6)&&s->sweep_enabled) {
        if(s->sweep_timer&&!--s->sweep_timer) {
            unsigned period=(s->reg[0]>>4)&7;
            s->sweep_timer=period?period:8;
            if(period) {
                unsigned frequency=sweep_result(s);
                if(frequency>2047)s->voice[0].enabled=false;
                else if(s->reg[0]&7) {
                    s->sweep_shadow=s->voice[0].frequency=frequency;
                    if(sweep_result(s)>2047)s->voice[0].enabled=false;
                }
            }
        }
    }
    if(step==7) {
        for(unsigned i=0;i<4;i++) {
            if(i==2)continue;
            Voice *v=&s->voice[i];
            if(v->enabled&&v->envelope_period&&v->envelope_timer&&!--v->envelope_timer) {
                int volume=(int)v->volume+v->envelope_direction;
                if(volume>=0&&volume<=15)v->volume=(unsigned)volume;
                v->envelope_timer=v->envelope_period;
            }
        }
    }
    s->sequencer_step=(step+1)&7;
}

static void synth_sample(Synth *s,unsigned mask,double out[2]) {
    static const uint8_t duty_pattern[4]={0x80,0x81,0xe1,0x7e};
    const double clocks=4194304.0/AUDIO_SAMPLE_RATE;
    if(!(s->reg[0x16]&0x80))return;
    s->sequencer_timer-=clocks;
    while(s->sequencer_timer<=0) {s->sequencer_timer+=8192;sequencer_tick(s);}
    for(unsigned i=0;i<4;i++) {
        Voice *v=&s->voice[i];
        if(!v->enabled)continue;
        v->timer-=clocks;
        while(v->timer<=0) {
            v->timer+=voice_period(s,i);
            if(i<2)v->phase=(v->phase+1)&7;
            else if(i==2)v->phase=(v->phase+1)&31;
            else {
                unsigned bit=(s->noise_bits^(s->noise_bits>>1))&1;
                s->noise_bits=(uint16_t)((s->noise_bits>>1)|(bit<<14));
                if(s->reg[0x12]&8)s->noise_bits=(uint16_t)((s->noise_bits&~0x40)|(bit<<6));
            }
        }
        if(!(mask&(1u<<i)))continue;
        double sample;
        if(i<2) {
            unsigned duty=s->reg[i?6:1]>>6;
            sample=((duty_pattern[duty]>>v->phase)&1)?v->volume:0;
        } else if(i==2) {
            unsigned level=(s->reg[12]>>5)&3;
            unsigned wave=s->reg[0x20+v->phase/2];
            wave=(v->phase&1)?wave&15:wave>>4;
            sample=level?(wave>>(level-1)):0;
        } else sample=(s->noise_bits&1)?0:v->volume;
        /* The high-pass stage below removes the DAC's steady component. */
        sample=(sample-7.5)*256;
        if(s->reg[0x15]&(1u<<(i+4)))out[0]+=sample*(((s->reg[0x14]>>4)&7)+1)/8;
        if(s->reg[0x15]&(1u<<i))out[1]+=sample*((s->reg[0x14]&7)+1)/8;
    }
}

void audio_reset(void) {
    synth_reset(&synth);sample_remainder=0;pending_frames=0;
    memset(previous_input,0,sizeof(previous_input));
    memset(previous_output,0,sizeof(previous_output));
    if(pcm){snd_pcm_drop(pcm);snd_pcm_prepare(pcm);}
}

void audio_write_register(void *context,uint8_t reg,uint8_t value) {
    (void)context;synth_write(&synth,reg,value);
}

size_t audio_render_frame(int16_t *samples) {
    sample_remainder+=(uint64_t)AUDIO_SAMPLE_RATE*70224;
    size_t count=(size_t)(sample_remainder/4194304);
    sample_remainder%=4194304;
    for(size_t i=0;i<count;i++) {
        double out[2]={0,0};synth_sample(&synth,15,out);
        for(unsigned side=0;side<2;side++) {
            double filtered=out[side]-previous_input[side]+0.996*previous_output[side];
            previous_input[side]=out[side];previous_output[side]=filtered;
            if(filtered>32767)filtered=32767;
            if(filtered< -32768)filtered= -32768;
            samples[i*2+side]=(int16_t)filtered;
        }
    }
    return count;
}

bool audio_open(void) {
    if(pcm)return true;
    int result=snd_pcm_open(&pcm,"default",SND_PCM_STREAM_PLAYBACK,SND_PCM_NONBLOCK);
    if(result>=0)result=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S16_LE,SND_PCM_ACCESS_RW_INTERLEAVED,
                                          2,AUDIO_SAMPLE_RATE,1,60000);
    if(result<0) {
        fprintf(stderr,"Audio unavailable: %s\n",snd_strerror(result));
        if(pcm)snd_pcm_close(pcm);
        pcm=NULL;return false;
    }
    return true;
}

void audio_close(void) {
    if(pcm) {snd_pcm_drop(pcm);snd_pcm_close(pcm);pcm=NULL;}
    pending_frames=0;
}

void audio_frame(void) {
    int16_t samples[AUDIO_MAX_FRAME_SAMPLES*2];
    size_t count=audio_render_frame(samples);
    if(!pcm)return;
    if(pending_frames+count>8192) {
        pending_frames=0;snd_pcm_drop(pcm);snd_pcm_prepare(pcm);
    }
    memcpy(pending+pending_frames*2,samples,count*2*sizeof(*samples));
    pending_frames+=count;
    while(pending_frames) {
        snd_pcm_sframes_t written=snd_pcm_writei(pcm,pending,pending_frames);
        if(written==-EAGAIN||written==0)break;
        if(written==-EPIPE) {snd_pcm_prepare(pcm);break;}
        if(written<0) {
            fprintf(stderr,"Audio output stopped: %s\n",snd_strerror((int)written));
            audio_close();break;
        }
        pending_frames-=(size_t)written;
        memmove(pending,pending+written*2,pending_frames*2*sizeof(*pending));
    }
}
