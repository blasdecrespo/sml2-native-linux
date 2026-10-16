#ifndef SML2_ASSETS_H
#define SML2_ASSETS_H
#include <stdint.h>

/* Internal coordinates used by the translated data readers. Only the asset
 * ranges listed in assets/index.json are populated; unused space is zero. */
enum { GAME_ASSET_SPACE_SIZE = 0x80000 };
extern const uint8_t game_assets[GAME_ASSET_SPACE_SIZE];
#endif
