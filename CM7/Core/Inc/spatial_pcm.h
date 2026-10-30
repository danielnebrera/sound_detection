#ifndef SPATIAL_PCM_H
#define SPATIAL_PCM_H
#include <stdbool.h>
#include <stdint.h>
#include "audio_recorder.h"

/* Captures one 100ms PCM16 planar window, channels CH0..CH3. */
void spatial_pcm_reset(void);
bool spatial_pcm_capture(uint32_t record_order, const RecorderChunkView *view);
bool spatial_pcm_has_capture(void);
/* Call only AFTER audio capture has stopped, never from ISR. */
bool spatial_pcm_transmit_after_stop(void);
#endif
