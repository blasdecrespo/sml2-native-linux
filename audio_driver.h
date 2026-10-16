#ifndef SML2_AUDIO_DRIVER_H
#define SML2_AUDIO_DRIVER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bank04's four 16-byte music records, expressed as musical state. */
typedef struct AudioMusicChannel {
    uint16_t pattern;       /* A410: pointer to a sequence of phrase pointers */
    uint8_t delay,duration; /* A412/3 */
    uint16_t note;          /* A414: next phrase command or note */
    uint8_t instrument[3]; /* A416..8: envelope/gate/duty, or wave/volume */
    uint16_t frequency;     /* A419/A */
    uint8_t rest;           /* A41B */
    uint16_t loop_start;    /* A41C/D */
    uint8_t age,loop_count; /* A41E/F: bit7 of loop_count reserves SFX voice */
} AudioMusicChannel;
typedef struct AudioDriverPair { uint8_t state,step; } AudioDriverPair;
typedef struct AudioDriverWork {
    uint8_t delay[4];       /* A500..3: pulse1/pulse2/wave/noise timers */
    uint8_t step[5];        /* A504..8: original counter offsets, 507 unused */
    uint8_t hurry;          /* A50D */
    uint8_t wave_envelope;  /* A50E */
    uint16_t mod_frequency[3]; /* A514..9 */
    AudioDriverPair rise[3];   /* A51A..F; rise[2].step also wave envelope age */
    AudioDriverPair vibrato[3];/* A520..5 */
    AudioDriverPair fall[3];   /* A526..B */
    uint8_t auxiliary;      /* A52C */
    uint8_t subtimer[4];    /* A52D..530 */
    uint16_t frequency[2];  /* A531..4: pulse/wave effect sweeps */
    uint8_t priority[5];    /* A535..9 */
    uint8_t star_chain,volume_duck; /* A53E/F */
} AudioDriverWork;
typedef void (*AudioRegisterWrite)(void *context,uint8_t reg,uint8_t value);
typedef struct AudioDriver {
    const uint8_t *rom;size_t rom_size;
    AudioRegisterWrite emit;void *emit_context;
    uint8_t registers[0x30];
    AudioMusicChannel channel[4];
    AudioDriverWork work;
    uint8_t transpose;uint16_t duration_table;uint8_t gate_time[4];
    uint8_t current_channel,modulation;
    uint8_t stereo_mode,stereo_clock,stereo_period,stereo_phase,stereo_masks[4];
    uint8_t pause_request;bool paused;
    uint8_t request[3],active_sfx[3],music_request,music;
    uint8_t player_star_chain; /* A279 gameplay input for pulse effect16. */
    uint8_t suppress_instrument;
    bool error;
} AudioDriver;

bool audio_driver_init(AudioDriver *driver,const uint8_t *rom,size_t size,AudioRegisterWrite emit,void *context);
/* Queue bytes mirror original source events and are consumed on every tick.
 * Repeating a music ID is a new request and restarts it, as in 04:4078. */
void audio_driver_step(AudioDriver *driver);
void audio_driver_write(AudioDriver *driver,uint8_t reg,uint8_t value);
void audio_driver_stop_music(AudioDriver *driver);
/* Bootstrap RAM clear and 04:4000, preserving the existing APU register state. */
void audio_driver_reset(AudioDriver *driver);
uint8_t audio_driver_byte(AudioDriver *driver,unsigned address);
uint16_t audio_driver_word(AudioDriver *driver,unsigned address);
#endif
