#include "audio_sfx.h"

/* The ROM contains instruments and frequency curves, not runtime bytecode.
 * These helpers implement the named register/data operations shared by the
 * original59 effect handlers in04:40DE..5228. */
static unsigned timer_index(unsigned queue) { return queue ? queue+1 : 0; }
static unsigned step_index(unsigned queue) { return queue==2 ? 4 : queue==1 ? 2 : 0; }
static uint8_t data(AudioDriver *d,unsigned address) {return audio_driver_byte(d,address);}
static unsigned sequence(AudioDriver *d,unsigned table,unsigned phase) {
    return audio_driver_word(d,table+(uint8_t)(2*(phase-1)));
}
static void write_data(AudioDriver *d,unsigned queue,unsigned address,unsigned reg,unsigned count) {
    for(unsigned i=0;i<count;++i)audio_driver_write(d,(uint8_t)(reg+i),data(d,address+i));
    d->work.delay[timer_index(queue)]=data(d,address+count);
}
static void instrument(AudioDriver *d,unsigned queue,unsigned address) {
    write_data(d,queue,address,queue==0?0x10:queue==1?0x1a:0x20,queue==2?4:5);
}
static void pitch(AudioDriver *d,unsigned queue,unsigned address) {
    write_data(d,queue,address,queue==0?0x13:queue==1?0x1d:0x22,2);
}
static void wave(AudioDriver *d,unsigned address) {
    audio_driver_write(d,0x1a,0);
    for(unsigned i=0;i<16;++i)audio_driver_write(d,(uint8_t)(0x30+i),data(d,address+i));
}
static void save_frequency(AudioDriver *d,unsigned queue,unsigned instrument_address) {
    d->work.frequency[queue]=(uint16_t)(data(d,instrument_address+3)|((data(d,instrument_address+4)&15)<<8));
}
static void bend(AudioDriver *d,unsigned queue,unsigned curve) {
    unsigned phase=d->work.step[step_index(queue)];
    unsigned address=curve+(uint8_t)(2*(phase-1));
    uint16_t delta=(uint16_t)((unsigned)data(d,address)*256+data(d,address+1));
    uint16_t frequency=(uint16_t)(d->work.frequency[queue]+delta);
    d->work.frequency[queue]=frequency;
    audio_driver_write(d,queue?0x1d:0x13,(uint8_t)frequency);
    audio_driver_write(d,queue?0x1e:0x14,(uint8_t)(frequency>>8));
}
static bool elapsed(AudioDriver *d,unsigned queue) {return --d->work.delay[timer_index(queue)]==0;}
static uint8_t next(AudioDriver *d,unsigned queue) {return ++d->work.step[step_index(queue)];}
static bool repeat_finished(AudioDriver *d,unsigned queue) {return --d->work.subtimer[timer_index(queue)]==0;}
static void finish(AudioDriver *d,unsigned queue,bool silence) {
    if(silence)instrument(d,queue,queue==0?0x46c6:queue==1?0x4e3c:0x519a);
    d->active_sfx[queue]=0;
    d->work.delay[timer_index(queue)]=0;d->work.step[step_index(queue)]=0;
    d->channel[queue?queue+1:0].loop_count&=0x7f;
    if(queue==0) {
        audio_driver_write(d,0x10,0);d->work.subtimer[0]=0;
        d->work.vibrato[0].state=0;d->work.priority[1]=0;
    } else if(queue==1) {
        audio_driver_write(d,0x1a,0);d->work.priority[3]=0;
        unsigned address=d->channel[2].instrument[0]|(unsigned)d->channel[2].instrument[1]<<8;
        for(unsigned i=0;i<16;++i)audio_driver_write(d,(uint8_t)(0x30+i),data(d,address+i));
    } else {d->work.subtimer[3]=0;d->work.priority[4]=0;}
}
static void begin(AudioDriver *d,unsigned queue,unsigned address,bool priority,bool gate_music) {
    if(priority)d->work.priority[queue?queue+2:1]=1;
    if(gate_music)d->work.vibrato[0].state=1;
    d->active_sfx[queue]=d->request[queue];
    d->work.delay[timer_index(queue)]=0;d->work.step[step_index(queue)]=0;
    d->channel[queue?queue+1:0].loop_count|=0x80;
    if(queue==1)audio_driver_write(d,0x1a,0);
    instrument(d,queue,address);
}

static void begin_pulse(AudioDriver *d,unsigned id) {
    unsigned address=0;bool priority=false,gate=false;
    switch(id) {
    case 1:address=0x4134;break;
    case 2:address=0x4152;break;
    case 3:if(d->active_sfx[0]==3)return;address=0x417c;break;
    case 4:d->work.subtimer[0]=5;address=0x41a3;break;
    case 5:d->work.subtimer[0]=2;address=0x46c6;priority=true;break;
    case 6:address=0x420b;break;
    case 7:address=0x422f;break;
    case 8:address=0x4259;priority=true;break;
    case 9:d->work.subtimer[0]=3;address=0x42aa;priority=true;save_frequency(d,0,address);break;
    case 10:return;
    case 11:address=0x42d3;break;
    case 12:audio_driver_stop_music(d);d->work.subtimer[0]=6;address=0x4343;priority=true;break;
    case 13:d->work.subtimer[0]=12;address=0x4388;priority=true;save_frequency(d,0,address);break;
    case 14:d->work.subtimer[0]=4;address=0x43ca;break;
    case 15:audio_driver_stop_music(d);address=0x4402;priority=gate=true;break;
    case 16:
        if(!d->player_star_chain||d->player_star_chain>=5)return;
        address=sequence(d,0x4431,d->player_star_chain);priority=gate=true;break;
    case 17:audio_driver_stop_music(d); /* Shares effect19's downward bend. */
        /* fall through */
    case 19:d->work.subtimer[0]=10;address=0x44d2;priority=true;save_frequency(d,0,address);break;
    case 18:address=0x448a;break;
    case 20:address=0x4542;priority=true;break;
    case 21:d->work.subtimer[0]=4;address=0x4595;priority=true;save_frequency(d,0,address);break;
    case 22:address=0x45dc;break;
    case 23:d->work.subtimer[0]=4;address=0x4626;priority=true;save_frequency(d,0,address);break;
    case 24:d->work.subtimer[0]=8;address=0x467a;priority=true;save_frequency(d,0,address);break;
    default:d->error=true;return;
    }
    begin(d,0,address,priority,gate);
}
static void pulse_step(AudioDriver *d,unsigned id) {
    unsigned phase,address;
    if(id==10)return;
    if(id!=13&&id!=17&&id!=19&&id!=24&&!elapsed(d,0))return;
    if(id==1){finish(d,0,false);return;}
    if(id==4) {
        if(repeat_finished(d,0)){finish(d,0,false);return;}
        instrument(d,0,0x41a3);return;
    }
    if(id==12) {
        phase=++d->work.priority[0];
        if(phase==3) {
            if(repeat_finished(d,0)){finish(d,0,false);return;}
            phase=d->work.priority[0]=1;
        }
        address=sequence(d,0x433f,phase);
        if(phase==1)write_data(d,0,address,0x16,4);else instrument(d,0,address);
        phase=next(d,0);d->work.delay[0]=data(d,0x4333+phase-1);return;
    }
    phase=next(d,0);
    switch(id) {
    case 2:case 3:case 6:case 7:case 16:
        if(phase==2){finish(d,0,false);return;}
        if(id==2)instrument(d,0,0x4158);
        else if(id==3)instrument(d,0,0x4182);
        else if(id==6)instrument(d,0,0x4211);
        else if(id==7)pitch(d,0,0x4235);
        else pitch(d,0,sequence(d,0x4439,d->player_star_chain));
        return;
    case 5:
        if(phase==5) {
            if(repeat_finished(d,0)){finish(d,0,false);return;}
            phase=d->work.step[0]=1;
        }
        instrument(d,0,sequence(d,0x41d3,phase));return;
    case 8:case 11:case 15:case 18:
        if(phase==(id==8||id==11?4:id==15?5:3)){finish(d,0,id==11);return;}
        address=sequence(d,id==8?0x4253:id==11?0x42cd:id==15?0x43fa:0x4486,phase);
        if(id==11)pitch(d,0,address);else instrument(d,0,address);
        return;
    case 9:case 13:case 17:case 19:case 21:case 23:case 24: {
        unsigned last=id==9?7:id==13?11:id==17||id==19?9:id==24?21:5;
        if(id==9)d->work.delay[0]=5;
        if(id==21||id==23)d->work.delay[0]=4;
        if(phase==last) {
            if(repeat_finished(d,0)){finish(d,0,false);return;}
            d->work.step[0]=id==24?9:1;
        }
        bend(d,0,id==9?0x429e:id==13?0x4374:id==17||id==19?0x44c2:id==21?0x458d:id==23?0x461e:0x4652);
        return;
    }
    case 14:
        if(phase==2)phase=d->work.step[0]=repeat_finished(d,0)?2:1;
        else if(phase==6){finish(d,0,false);return;}
        instrument(d,0,sequence(d,0x43c0,phase));return;
    case 20:
        if(phase==24){finish(d,0,false);return;}
        audio_driver_write(d,0x12,phase<8?0xa0:phase<16?0x40:0x20);
        audio_driver_write(d,0x10,0x2f);audio_driver_write(d,0x11,0xbb);
        pitch(d,0,sequence(d,0x4514,phase));return;
    case 22:
        if(phase==9){finish(d,0,true);return;}
        address=sequence(d,0x45cc,phase);
        if(phase==8)instrument(d,0,address);
        else {audio_driver_write(d,0x12,phase&1?0x30:0xb0);pitch(d,0,address);}
        return;
    default:d->error=true;return;
    }
}

static void begin_wave(AudioDriver *d,unsigned id) {
    unsigned address=0,waveform=0,repeat=0;bool save=false,priority=false;
    switch(id) {
    case 1:repeat=2;waveform=0x5bd6;address=0x47d1;break;
    case 2:waveform=0x5be6;address=0x4866;break;
    case 3:repeat=2;waveform=0x5be6;address=0x48e2;break;
    case 4:repeat=0x24;waveform=0x5bf6;address=0x4926;save=true;break;
    case 5:repeat=2;waveform=0x5be6;address=0x4988;save=true;break;
    case 6:
        if(d->active_sfx[1]==6)return;
        waveform=0x5b96;
        if(d->work.star_chain){--d->work.star_chain;address=0x49e1;}
        else {++d->work.star_chain;address=0x49ed;}
        break;
    case 7:repeat=0x30;waveform=0x5b96;address=0x4a28;save=true;break;
    case 8:repeat=2;waveform=0x5be6;address=0x4aa3;save=true;break;
    case 9:repeat=0x14;waveform=0x5be6;address=0x4ade;save=priority=true;break;
    case 10:case 11:case 12:
        if(id==12)repeat=2;
        waveform=0x5b96;address=0x4b14;save=true;break;
    case 13:repeat=2;waveform=0x5b86;address=0x4c13;save=true;break;
    case 14:waveform=0x4c1f;address=0x4c42;break;
    case 15:repeat=3;waveform=0x5bf6;address=0x4b83;save=true;break;
    case 16:case 17:repeat=8;waveform=0x5b96;address=id==16?0x4c6c:0x4c89;save=true;break;
    case 18:waveform=0x5be6;address=0x4cb9;break;
    case 19:return;
    case 20:repeat=7;waveform=0x4cd1;address=0x4d32;save=priority=true;break;
    case 21:case 24:
        if(id==24&&d->active_sfx[1])return;
        waveform=0x5b96;address=0x4d60;break;
    case 22:repeat=10;waveform=0x5b96;address=0x4da8;save=true;break;
    case 23:repeat=7;waveform=0x5ba6;address=0x4de5;save=true;break;
    default:d->error=true;return;
    }
    if(repeat)d->work.subtimer[2]=(uint8_t)repeat;
    wave(d,waveform);
    if(save)save_frequency(d,1,address);
    begin(d,1,address,priority,false);
}
static void wave_step(AudioDriver *d,unsigned id) {
    unsigned phase,address;
    if(id==19)return;
    bool timed=id==1||id==2||id==3||id==5||id==6||id==8||id==13||id==14||id==15||id==18||id==21||id==23||id==24;
    if(timed&&!elapsed(d,1))return;
    if(id==14){finish(d,1,false);return;}
    if(id==4||id==7||id==9||id==16||id==17) {
        if(repeat_finished(d,1)){finish(d,1,id==4||id==7||id==9);return;}
        unsigned halfway=id==4?18:id==7?24:id==9?16:256;
        if(d->work.subtimer[2]==halfway) {
            address=id==4?0x492c:id==7?0x4a2e:0x4ae4;
            save_frequency(d,1,address);instrument(d,1,address);
        } else {
            d->work.step[2]=1;
            bend(d,1,id==4?0x4924:id==7?0x4a26:id==9?0x4adc:id==16?0x4c6a:0x4c87);
        }
        return;
    }
    phase=next(d,1);
    switch(id) {
    case 1:
        if(phase==24) {
            if(repeat_finished(d,1)){finish(d,1,false);return;}
            phase=d->work.step[2]=12;
        }
        address=sequence(d,0x47a3,phase);
        if(phase==4||phase==8||phase==12||phase==16||phase==20)instrument(d,1,address);
        else pitch(d,1,address);
        return;
    case 2:
        if(phase==18){finish(d,1,false);return;}
        address=sequence(d,0x4844,phase);
        if(phase%3==0)instrument(d,1,address);else pitch(d,1,address);
        return;
    case 3:
        if(phase==8) {
            if(repeat_finished(d,1)){finish(d,1,false);return;}
            d->work.step[2]=0;instrument(d,1,0x48e2);return;
        }
        address=sequence(d,0x48d4,phase);
        if(phase==4)instrument(d,1,address);else pitch(d,1,address);
        return;
    case 5:case 8:case 13:
        if(phase==(id==5?17:id==8?29:11)) {
            if(repeat_finished(d,1)){finish(d,1,true);return;}
            d->work.step[2]=0;address=id==5?0x498e:id==8?0x4aa9:0x4c19;
            save_frequency(d,1,address);instrument(d,1,address);return;
        }
        d->work.delay[2]=id==8?1:2;
        bend(d,1,id==5?0x4968:id==8?0x4a6b:0x4bfb);return;
    case 6:
        if(phase==3){finish(d,1,false);return;}
        instrument(d,1,sequence(d,d->work.star_chain?0x49dd:0x49d9,phase));return;
    case 10:case 11:case 12:
        if(phase==11) {
            if(id!=12||repeat_finished(d,1)){finish(d,1,true);return;}
            d->work.step[2]=0;save_frequency(d,1,0x4bbc);instrument(d,1,0x4bbc);return;
        }
        bend(d,1,0x4b04);return;
    case 15:
        d->work.delay[2]=2;
        if(phase==21) {
            if(repeat_finished(d,1)){finish(d,1,true);return;}
            address=d->work.subtimer[2]==1?0x4b8f:0x4b89;
            d->work.step[2]=4;save_frequency(d,1,address);instrument(d,1,address);return;
        }
        bend(d,1,0x4b5b);return;
    case 18:case 21:case 24:
        if(phase==(id==18?7:6)){finish(d,1,false);return;}
        instrument(d,1,sequence(d,id==18?0x4cad:0x4d56,phase));return;
    case 20:
        if(phase==9&&!repeat_finished(d,1))d->work.step[2]=1;
        if(phase==17){finish(d,1,true);return;}
        bend(d,1,0x4d12);return;
    case 22:
        if(phase==3) {
            if(repeat_finished(d,1)){finish(d,1,false);return;}
            if(d->work.subtimer[2]==5) {
                d->work.step[2]=0;save_frequency(d,1,0x4da8);instrument(d,1,0x4da8);return;
            }
            d->work.step[2]=1;
        }
        bend(d,1,0x4da4);return;
    case 23:
        d->work.delay[2]=2;
        if(phase==3) {
            if(repeat_finished(d,1)){finish(d,1,true);return;}
            d->work.step[2]=1;
        }
        bend(d,1,0x4de1);return;
    default:d->error=true;return;
    }
}

static void begin_noise(AudioDriver *d,unsigned id) {
    static const unsigned instruments[]={0,0x4edc,0x4f34,0x4f60,0x4f7d,0x4fab,0x4fea,0x5026,0x506d,0x50c4,0x5108,0x5145};
    if(id>11){d->error=true;return;}
    if((id==4||id==7)&&d->active_sfx[2]==id)return;
    if(id==3) {
        d->active_sfx[2]=3;instrument(d,2,0x4f60);return;
    }
    if(id==2)d->work.subtimer[3]=3;
    if(id==10)d->work.subtimer[3]=5;
    begin(d,2,instruments[id],false,false);
}
static void noise_step(AudioDriver *d,unsigned id) {
    if(id==3) {d->active_sfx[2]=0;instrument(d,2,0x4f65);return;}
    if(!elapsed(d,2))return;
    if(id==4){finish(d,2,false);return;}
    unsigned phase=next(d,2),address;
    static const uint8_t last[]={0,8,12,0,0,8,6,2,10,12,7,8};
    if(id>11){d->error=true;return;}
    if(phase==last[id]){finish(d,2,id==8||id==10);return;}
    switch(id) {
    case 1:instrument(d,2,sequence(d,0x4ece,phase));return;
    case 2:
        audio_driver_write(d,0x21,phase&1?0x20:0x49);audio_driver_write(d,0x20,0x2f);
        pitch(d,2,sequence(d,0x4f1e,phase));return;
    case 5:pitch(d,2,sequence(d,0x4f9d,phase));return;
    case 6:instrument(d,2,sequence(d,0x4fe0,phase));return;
    case 7:instrument(d,2,0x502b);return;
    case 8:
        address=sequence(d,0x505b,phase);
        if(phase<5)instrument(d,2,address);
        else {audio_driver_write(d,0x21,0x20);pitch(d,2,address+2);}
        return;
    case 9:
        address=sequence(d,0x50ae,phase);
        if(phase==4||phase==8)instrument(d,2,address);else pitch(d,2,address);
        return;
    case 10:pitch(d,2,sequence(d,0x50fc,phase));return;
    case 11:instrument(d,2,sequence(d,0x5137,phase));return;
    default:d->error=true;return;
    }
}

void audio_sfx_step(AudioDriver *d,unsigned queue) {
    if(!d||queue>=3)return;
    unsigned request=d->request[queue];
    if(request) {
        if(queue==0) {
            bool bypass=request==9||request==12||request==15||request==16||request==17||request==19||request==20||request==23;
            if(!bypass&&!d->work.vibrato[0].state&&d->work.priority[1])return;
            begin_pulse(d,request);
        } else if(queue==1) {
            if(request!=20&&d->work.priority[3])return;
            begin_wave(d,request);
        } else {
            if(d->work.priority[4])return;
            begin_noise(d,request);
        }
    } else if(d->active_sfx[queue]) {
        if(queue==0)pulse_step(d,d->active_sfx[queue]);
        else if(queue==1)wave_step(d,d->active_sfx[queue]);
        else noise_step(d,d->active_sfx[queue]);
    }
}
