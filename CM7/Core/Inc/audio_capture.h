/**
 * @file audio_capture.h
 * @brief Captura SAI2 A/B y diagnostico aislado de SAI1_B (SD_C).
 *
 * Mapeo validado:
 *   ch0 -> Mic2 -> SAI2_B slot impar -> left
 *   ch1 -> Mic4 -> SAI2_A slot impar -> top
 *   ch2 -> Mic3 -> SAI2_A slot par   -> back
 *   ch3 -> Mic1 -> SAI2_B slot par   -> right
 *
 * Esta version no entrega buffers al foreground. Cuando las dos mitades
 * A/B correspondientes estan listas, la callback publica el bloque directo
 * al productor SDRAM de audio_recorder.
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include "main.h"

#define AUDIO_SAMPLE_RATE       44100U
#define AUDIO_CHANNELS          4U
#define AUDIO_BUFFER_SIZE       512U
#define AUDIO_SLOTS_PER_FRAME   2U
#define AUDIO_DMA_HALVES        2U

/* C1/C2: solo recepcion RAW diagnostica. No cambia AUDIO_CHANNELS. */
#define AUDIO_CAPTURE_C_ENABLE  1U
#define AUDIO_C_RAW_FRAMES      16U

#define AUDIO_DMA_BUFFER_SIZE \
    (AUDIO_BUFFER_SIZE * AUDIO_SLOTS_PER_FRAME * AUDIO_DMA_HALVES)

typedef struct
{
    volatile uint32_t dma_a_generation[AUDIO_DMA_HALVES];
    volatile uint32_t dma_b_generation[AUDIO_DMA_HALVES];
    volatile uint32_t consumed_generation[AUDIO_DMA_HALVES];

    /* Estadisticas independientes de SAI1_B. No bloquean el par A/B. */
    volatile uint32_t dma_c_generation[AUDIO_DMA_HALVES];
    volatile uint32_t c_error_count;
    volatile uint32_t c_error_code;
    volatile uint8_t c_dma_active;

    volatile uint32_t paired_blocks;
    volatile uint32_t error_count;
    volatile uint32_t overrun_count;
    volatile uint32_t pair_mismatch_count;
    volatile uint32_t maximum_pair_skew;
    volatile uint8_t fatal_error;
} AudioCaptureContext;

typedef struct
{
    uint32_t paired_blocks;
    uint32_t errors;
    uint32_t overruns;
    uint32_t pair_mismatches;
    uint32_t maximum_pair_skew;
    uint32_t c_half_count;
    uint32_t c_full_count;
    uint32_t c_errors;
    uint32_t c_error_code;
    bool c_dma_active;
    bool fatal_error;
} AudioCaptureStats;

int audio_capture_init(AudioCaptureContext *ctx);
int audio_capture_start(AudioCaptureContext *ctx);
int audio_capture_stop(AudioCaptureContext *ctx);

bool audio_capture_has_fatal_error(const AudioCaptureContext *ctx);
void audio_capture_get_stats(
    const AudioCaptureContext *ctx,
    AudioCaptureStats *stats
);

extern AudioCaptureContext g_audio_ctx;
extern uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];
extern uint32_t dma_buf_b[AUDIO_DMA_BUFFER_SIZE];
#if AUDIO_CAPTURE_C_ENABLE
extern uint32_t dma_buf_c[AUDIO_DMA_BUFFER_SIZE];
#endif

#endif /* AUDIO_CAPTURE_H */
