#include "demo.h"
#include <string.h>

bool demo_init(Demo *d,const uint8_t *rom,size_t size,unsigned index) {
    if(!d||!rom||size!=524288||index>=4) return false;
    memset(d,0,sizeof(*d));d->index=(uint8_t)index;
    d->level=rom[5*0x4000+0x121+index];
    memcpy(d->records,rom+5*0x4000+0x3f00-index*256,256);
    return true;
}

bool demo_step(Demo *d,uint8_t *held,uint8_t *pressed) {
    *held=*pressed=0;
    if(d->finished||d->error) return false;
    /* Each iteration examines a distinct two-byte record. The page's FF
     * terminator ends playback; malformed data cannot wrap forever. */
    for(unsigned n=0;n<128;++n) {
        uint8_t buttons=d->records[d->cursor];
        if(buttons==0xff) {d->finished=true;return false;}
        uint8_t *duration=&d->records[(uint8_t)(d->cursor+1)];
        if(!*duration) {d->cursor=(uint8_t)(d->cursor+2);continue;}
        --*duration;
        *held=(uint8_t)((buttons<<4)|(buttons>>4));
        *pressed=*held&(uint8_t)~d->held;
        d->held=*held;
        return true;
    }
    d->error=true;return false;
}
