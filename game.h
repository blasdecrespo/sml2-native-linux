#ifndef SML2_GAME_H
#define SML2_GAME_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { GAME_WIDTH = 160, GAME_HEIGHT = 144, GAME_SAVE_SIZE = 8192 };
enum GameButton {
    BUTTON_RIGHT=1, BUTTON_LEFT=2, BUTTON_UP=4, BUTTON_DOWN=8,
    BUTTON_A=16, BUTTON_B=32, BUTTON_SELECT=64, BUTTON_START=128
};
typedef struct Game Game;
Game *game_create(bool start_in_level);
void game_destroy(Game *game);
void game_step(Game *game, uint8_t held_buttons);
void game_render(const Game *game, uint8_t shades[GAME_WIDTH*GAME_HEIGHT]);
void game_write_state(const Game *game, FILE *output);
uint8_t game_music(const Game *game);
uint8_t game_sound_request(const Game *game,unsigned channel);
/* Attach before the first step; replays the driver's initialization and title-stop writes.
 * Driver state always advances independently of host audio availability. */
void game_set_audio_output(Game *game,void (*emit)(void *context,uint8_t reg,uint8_t value),void *context);
struct AudioDriver;
const struct AudioDriver *game_audio_state(const Game *game);
/* Import before the first game_step. Original record validation belongs to
 * screens; bytes outside its three records survive the round trip unchanged. */
bool game_import_save(Game *game,const uint8_t *bytes,size_t size);
void game_export_save(const Game *game,uint8_t bytes[GAME_SAVE_SIZE]);
uint64_t game_save_revision(const Game *game);
/* Host timer supplies the free-running hardware-divider input to native AI.
 * Reference callers can inject captured readings instead. */
void game_set_timer_readers(Game *game,uint8_t (*divider)(void *context),
                            uint8_t (*timer)(void *context),void *context);
bool game_paused(const Game *game);

#endif
