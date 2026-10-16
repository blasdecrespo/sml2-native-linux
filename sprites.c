#include "sprites.h"
#include <string.h>

static const uint8_t *bank_bytes(const uint8_t *assets, size_t size,
                                  unsigned bank, unsigned address, unsigned length) {
    if (address < 0x4000 || address >= 0x8000 || length > 0x8000 - address) return NULL;
    size_t offset = (size_t)bank * 0x4000 + address - 0x4000;
    if (offset > size || length > size - offset) return NULL;
    return assets + offset;
}

static bool composition(const uint8_t *assets, size_t size, unsigned bank,
                        unsigned table, unsigned frame, unsigned *address) {
    if (frame > 255) return false;
    const uint8_t *p = bank_bytes(assets, size, bank, table + 2 * frame, 2);
    if (!p) return false;
    *address = p[0] | (unsigned)p[1] << 8;
    return *address >= 0x4000 && *address < 0x8000;
}

void sprites_clear(SpriteList *list) { memset(list, 0, sizeof(*list)); }

bool sprites_add(SpriteList *list, uint8_t y, uint8_t x, uint8_t tile, uint8_t flags) {
    if (list->count >= 64) { list->overflow = true; return false; }
    uint8_t *p = list->bytes + 4 * list->count++;
    p[0] = y; p[1] = x; p[2] = tile; p[3] = flags;
    return true;
}

bool sprites_mario(SpriteList *list, const uint8_t *assets, size_t size,
                   unsigned frame, uint8_t y, uint8_t x,
                   bool second_palette, bool behind_background) {
    /* 01:5297, including modulo-256 coordinates and attribute ORs. */
    unsigned address;
    if (!composition(assets, size, 1, 0x4000, frame, &address)) return false;
    for (unsigned part = 0; part < 64; ++part, address += 4) {
        const uint8_t *p = bank_bytes(assets, size, 1, address, 1);
        if (!p) return false;
        if (p[0] == 0x80) return true;
        p = bank_bytes(assets, size, 1, address, 4);
        if (!p) return false;
        uint8_t flags = p[3] | (second_palette ? 0x10 : 0) | (behind_background ? 0x80 : 0);
        if (!sprites_add(list, (uint8_t)(y + p[0]), (uint8_t)(x + p[1]), p[2], flags)) return false;
    }
    return false;
}

static bool clipped_metasprite(SpriteList *list,const uint8_t *assets,size_t size,
                               unsigned bank,unsigned address,uint8_t y,uint8_t x,uint8_t flags_xor) {
    /* Shared original03:402C /19:6039 clipped-part representation. */
    for (unsigned part = 0; part < 64; ++part, address += 4) {
        const uint8_t *p = bank_bytes(assets, size, bank, address, 1);
        if (!p) return false;
        if (p[0] == 0x80) return true;
        p = bank_bytes(assets, size, bank, address, 4);
        if (!p) return false;
        int dy = flags_xor & 0x40 ? -(int)p[0] - 8 : p[0];
        int dx = flags_xor & 0x20 ? -(int)p[1] - 8 : p[1];
        uint8_t sy = (uint8_t)(y + dy), sx = (uint8_t)(x + dx);
        if (sy >= 0xa0) continue;
        if (list->count >= 64) { list->overflow = true; return false; }
        if (sx >= 0xa8) {
            /* 03:406E clears the tentative Y without consuming an OAM slot. */
            list->bytes[4 * list->count] = 0;
            continue;
        }
        if (!sprites_add(list, sy, sx, p[2], p[3] ^ flags_xor)) return false;
    }
    return false;
}

bool sprites_actor(SpriteList *list, const uint8_t *assets, size_t size,
                   unsigned frame, bool alternate_tileset,
                   uint8_t y, uint8_t x, uint8_t flags_xor) {
    /* 03:4000..408F; lifecycle/visibility gates belong to the actor owner. */
    if (x >= 0xb8 || y >= 0xb8) return true;
    unsigned address;
    if (!composition(assets, size, 3, alternate_tileset ? 0x4f11 : 0x40b1, frame, &address)) return false;
    return clipped_metasprite(list,assets,size,3,address,y,x,flags_xor);
}

bool sprites_boss(SpriteList *list,const uint8_t *assets,size_t size,
                  unsigned frame,uint8_t y,uint8_t x,uint8_t flags_xor) {
    unsigned address;
    if(!composition(assets,size,0x19,0x6092,frame,&address)) return false;
    const uint8_t *p=bank_bytes(assets,size,0x19,address,1);
    if(!p) return false;
    /* 19:6026: optional88 header supplies an anchor only when flipped in Y. */
    if(*p==0x88) {
        p=bank_bytes(assets,size,0x19,address,2);
        if(!p) return false;
        if(flags_xor&0x40) y=(uint8_t)(y+p[1]);
        address+=2;
    }
    return clipped_metasprite(list,assets,size,0x19,address,y,x,flags_xor);
}

void sprites_render(const SpriteList *list, const uint8_t tiles[0x1800],
                    const uint8_t bg[160 * 144], uint8_t fb[160 * 144],
                    uint8_t palette0, uint8_t palette1) {
    unsigned count = list->count < 40 ? list->count : 40;
    for (int y = 0; y < 144; ++y) {
        unsigned line[10], used = 0;
        /* OAM scan selects the first ten vertical matches, even if X is hidden. */
        for (unsigned i = 0; i < count && used < 10; ++i) {
            const uint8_t *p = list->bytes + i * 4;
            if (y + 16 >= p[0] && y + 16 < p[0] + 8) {
                unsigned j = used++;
                /* DMG: smaller X wins, then earlier OAM index. */
                while (j && list->bytes[line[j - 1] * 4 + 1] > p[1]) {
                    line[j] = line[j - 1]; --j;
                }
                line[j] = i;
            }
        }
        for (int x = 0; x < 160; ++x) {
            for (unsigned j = 0; j < used; ++j) {
                const uint8_t *p = list->bytes + line[j] * 4;
                int tx = x + 8 - p[1], ty = y + 16 - p[0];
                if (tx < 0 || tx >= 8) continue;
                if (p[3] & 0x20) tx = 7 - tx;
                if (p[3] & 0x40) ty = 7 - ty;
                const uint8_t *tile = tiles + p[2] * 16 + ty * 2;
                unsigned bit = 7 - tx;
                unsigned color = ((tile[0] >> bit) & 1) | (((tile[1] >> bit) & 1) << 1);
                if (!color) continue;
                if (!(p[3] & 0x80) || !bg[y * 160 + x]) {
                    uint8_t palette = p[3] & 0x10 ? palette1 : palette0;
                    fb[y * 160 + x] = (palette >> (color * 2)) & 3;
                }
                break; /* A hidden winning pixel still occludes lower priority sprites. */
            }
        }
    }
}
