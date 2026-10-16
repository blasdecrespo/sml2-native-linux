#ifndef SML2_SPRITES_H
#define SML2_SPRITES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The game composes into A100..A1FF. The display uses its first 40 objects. */
typedef struct SpriteList {
    uint8_t bytes[256];
    unsigned count;
    bool overflow;
} SpriteList;

void sprites_clear(SpriteList *list);
bool sprites_add(SpriteList *list, uint8_t y, uint8_t x, uint8_t tile, uint8_t flags);
/* Origins are the original OAM coordinates, including the hardware +8/+16. */
bool sprites_mario(SpriteList *list, const uint8_t *assets, size_t size,
                   unsigned frame, uint8_t y, uint8_t x,
                   bool second_palette, bool behind_background);
bool sprites_actor(SpriteList *list, const uint8_t *assets, size_t size,
                   unsigned frame, bool alternate_tileset,
                   uint8_t y, uint8_t x, uint8_t flags_xor);
bool sprites_boss(SpriteList *list,const uint8_t *assets,size_t size,
                  unsigned frame,uint8_t y,uint8_t x,uint8_t flags_xor);
/* bg_colors holds unpaletted colors: priority depends on color, not shade. */
void sprites_render(const SpriteList *list, const uint8_t tiles[0x1800],
                    const uint8_t bg_colors[160 * 144], uint8_t shades[160 * 144],
                    uint8_t palette0, uint8_t palette1);

#endif
