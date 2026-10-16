#ifndef SML2_AUDIO_H
#define SML2_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { AUDIO_SAMPLE_RATE = 48000, AUDIO_MAX_FRAME_SAMPLES = 804 };

/* Optional host output. Failure leaves deterministic synthesis available. */
bool audio_open(void);
void audio_close(void);
/* Reset synthesis before connecting this callback to the Game sound driver.
 * No audio-device state participates in the game's music/effect decisions. */
void audio_reset(void);
void audio_write_register(void *context,uint8_t reg,uint8_t value);
void audio_frame(void);

/* One 70224/4194304-second game frame; interleaved signed 16-bit stereo.
 * The caller supplies AUDIO_MAX_FRAME_SAMPLES * 2 elements.
 * This deterministic route never opens ALSA and permits offline verification.
 */
size_t audio_render_frame(int16_t *samples);

#endif
