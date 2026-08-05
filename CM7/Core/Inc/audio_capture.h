/**
 * @file audio_capture.h
 * @brief Captura sincronizada SAI2_A + SAI2_B para cuatro microfonos.
 *
 * Mapeo validado:
 *   ch0 -> Mic2 -> SAI2_B slot impar -> left
 *   ch1 -> Mic4 -> SAI2_A slot impar -> top
 *   ch2 -> Mic3 -> SAI2_A slot par   -> back
 *   ch3 -> Mic1 -> SAI2_B slot par   -> right
 *
 * Cada evento HALF/FULL representa 512 muestras por microfono.
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>
#include "main.h"

#define AUDIO_SAMPLE_RATE       44100U
#define AUDIO_CHANNELS          4U
#define AUDIO_BUFFER_SIZE       512U
#define AUDIO_SLOTS_PER_FRAME   2U
#define AUDIO_DMA_HALVES        2U

#define AUDIO_DMA_BUFFER_SIZE \
    (AUDIO_BUFFER_SIZE * AUDIO_SLOTS_PER_FRAME * AUDIO_DMA_HALVES)

typedef enum
{
    AUDIO_BUFFER_EMPTY   = 0,
    AUDIO_BUFFER_HALF    = 1,
    AUDIO_BUFFER_FULL    = 2,
    AUDIO_BUFFER_OVERRUN = 3
} AudioBufferState;

typedef struct
{
    int32_t ch0[AUDIO_BUFFER_SIZE];
    int32_t ch1[AUDIO_BUFFER_SIZE];
    int32_t ch2[AUDIO_BUFFER_SIZE];
    int32_t ch3[AUDIO_BUFFER_SIZE];

    volatile uint32_t dma_a_half_produced;
    volatile uint32_t dma_a_full_produced;
    volatile uint32_t dma_b_half_produced;
    volatile uint32_t dma_b_full_produced;

    uint32_t dma_half_consumed;
    uint32_t dma_full_consumed;

    uint8_t expected_half;
    volatile AudioBufferState buffer_state;

    uint32_t blocks_processed;
    uint32_t error_count;
    uint32_t overrun_count;
} AudioCaptureContext;

int audio_capture_init(AudioCaptureContext *ctx);
int audio_capture_start(AudioCaptureContext *ctx);
int audio_capture_stop(AudioCaptureContext *ctx);

AudioBufferState audio_capture_get_data(AudioCaptureContext *ctx);

int32_t *audio_capture_get_channel(
    AudioCaptureContext *ctx,
    uint8_t channel
);

void audio_capture_deinterleave(
    AudioCaptureContext *ctx,
    uint8_t half
);

void audio_capture_get_stats(
    AudioCaptureContext *ctx,
    uint32_t *blocks_processed,
    uint32_t *errors,
    uint32_t *overruns
);

extern AudioCaptureContext g_audio_ctx;
extern uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];
extern uint32_t dma_buf_b[AUDIO_DMA_BUFFER_SIZE];

#endif /* AUDIO_CAPTURE_H */
