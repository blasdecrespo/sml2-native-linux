#ifndef SML2_DEMO_H
#define SML2_DEMO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Recorded controller runs from05:7F00,7E00,7D00,7C00. Source00:38B9
 * consumes and decrements a private copy, preserving the original ROM data. */
typedef struct Demo {
    uint8_t records[256],cursor,held,index,level;
    bool finished,error;
} Demo;
bool demo_init(Demo *demo,const uint8_t *rom,size_t size,unsigned index);
bool demo_step(Demo *demo,uint8_t *held,uint8_t *pressed);
#endif
