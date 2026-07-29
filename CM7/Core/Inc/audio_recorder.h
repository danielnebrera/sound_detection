/* =================================================================
 * audio_recorder.h
 *
 * Graba exactamente 1 segundo de 4 canales simultáneos en int16.
 *
 * Layout RAM D1:
 *   int16_t[44100][4] = 352,800 bytes
 *
 * Flujo por chunk:
 *   1. audio_recorder_accumulate() en el loop DMA → llena el buffer
 *   2. audio_recorder_is_ready() → true cuando hay 44100 frames
 *   3. audio_recorder_emit_and_reset() → transmite por UART y reinicia
 *
 * Protocolo UART:
 *   [CHUNK_START] 44100
 *   [CH0_START] ... [CH0_END]
 *   [CH1_START] ... [CH1_END]
 *   [CH2_START] ... [CH2_END]
 *   [CH3_START] ... [CH3_END]
 *   [CHUNK_END]
 * ================================================================= */

#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#define RECORD_SECONDS      1U
#define RECORD_SAMPLE_RATE  44100U
#define RECORD_FRAMES       (RECORD_SECONDS * RECORD_SAMPLE_RATE)
#define RECORD_CHANNELS     4U
#define RECORD_N_CH         RECORD_CHANNELS

typedef struct {
    int16_t samples[RECORD_FRAMES][RECORD_CHANNELS];
} RecorderChunk;

void audio_recorder_init(void);
void audio_recorder_accumulate(void);
bool audio_recorder_is_ready(void);
bool audio_recorder_emit_and_reset(void);
bool audio_recorder_stop_requested(void);

#endif
