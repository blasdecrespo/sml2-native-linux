#ifndef SML2_SAVEFILE_H
#define SML2_SAVEFILE_H

#include "game.h"

/* Host file persistence only. Record checksums and repair belong to the game. */
typedef struct SaveFile {
    const char *path;
    uint8_t bytes[GAME_SAVE_SIZE];
    bool exists,backed_up;
} SaveFile;

/* An absent file is a successful load with exists=false. Other failures set
 * errno. Stores use an atomic rename, retaining the first original in .bak. */
bool savefile_load(SaveFile *save,const char *path);
bool savefile_store(SaveFile *save,const uint8_t bytes[GAME_SAVE_SIZE]);

#endif
