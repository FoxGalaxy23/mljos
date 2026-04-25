#ifndef SOUND_H
#define SOUND_H

#include "common.h"

// Plays a beep with given frequency (Hz) for a given duration (ms)
void sound_beep(uint32_t freq, uint32_t duration_ms);

// Low-level sound control
void sound_play(uint32_t freq);
void sound_stop(void);

#endif
