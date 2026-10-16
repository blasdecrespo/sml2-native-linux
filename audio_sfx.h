#ifndef SML2_AUDIO_SFX_H
#define SML2_AUDIO_SFX_H
#include "audio_driver.h"
/* Source queues A460/A470/A478: pulse1, wave and noise, in that order. */
void audio_sfx_step(AudioDriver *driver,unsigned queue);
#endif
