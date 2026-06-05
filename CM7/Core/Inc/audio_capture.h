/**
 * @file audio_capture.h
 * @brief SAI + DMA Audio Capture Driver for STM32H7
 *
 * Captures 4-channel audio from 2 SAI blocks (SAI1_A + SAI1_B)
 * with hardware multiplexing via L/R pins.
 *
 * Pinout:
 *   SAI1_A: Mic1 (L) + Mic2 (R) on SD_A
 *   SAI1_B: Mic3 (L) + Mic4 (R) on SD_B
 *   Shared: SCK (PI5), FS (PI7)
 *   SAI2_A: SD_A (PI6) - Mic3/Mic4
 *   SAI2_B: SD_B (PG10) - Mic1/Mic2
 *
 * NOTA: Los buffers DMA (dma_buf_a, dma_buf_b) viven en audio_capture.c
 * en la seccion RAM_D2 para ser accesibles por DMA1. NO estan en el struct.
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>
#include <string.h>
#include "main.h"

/* ============ CONFIGURATION ============ */
#define AUDIO_SAMPLE_RATE       44100
#define AUDIO_CHANNELS          4
#define AUDIO_BUFFER_SIZE       512
#define AUDIO_DMA_BUFFER_SIZE   (AUDIO_BUFFER_SIZE * 2)  /* stereo pairs */

#define AUDIO_SAMPLE_BITS       32
#define AUDIO_SAMPLE_BYTES      (AUDIO_SAMPLE_BITS / 8)

/* ============ DATA STRUCTURES ============ */

typedef enum {
    AUDIO_BUFFER_EMPTY = 0,
    AUDIO_BUFFER_HALF  = 1,
    AUDIO_BUFFER_FULL  = 2
} AudioBufferState;

/**
 * @brief Audio capture context
 * Los buffers DMA NO estan aqui — viven en audio_capture.c en RAM_D2.
 * Aqui solo estan los canales desentrelazados y los flags de estado.
 */
typedef struct {
    /* Canales desentrelazados (salida procesada) */
    int32_t ch0[AUDIO_BUFFER_SIZE];  /* Microfono 1 */
    int32_t ch1[AUDIO_BUFFER_SIZE];  /* Microfono 2 */
    int32_t ch2[AUDIO_BUFFER_SIZE];  /* Microfono 3 */
    int32_t ch3[AUDIO_BUFFER_SIZE];  /* Microfono 4 */

    /* Flags de estado — seteados por callbacks DMA */
    volatile uint8_t dma_half_complete;
    volatile uint8_t dma_full_complete;
    volatile AudioBufferState buffer_state;

    /* Estadisticas */
    uint32_t frame_count;
    uint32_t error_count;

} AudioCaptureContext;

/* ============ PUBLIC API ============ */

int           audio_capture_init(AudioCaptureContext *ctx);
int           audio_capture_start(AudioCaptureContext *ctx);
int           audio_capture_stop(AudioCaptureContext *ctx);
AudioBufferState audio_capture_get_data(AudioCaptureContext *ctx);
int32_t*      audio_capture_get_channel(AudioCaptureContext *ctx, uint8_t channel);
void          audio_capture_deinterleave(AudioCaptureContext *ctx, uint8_t half);
void          audio_capture_get_stats(AudioCaptureContext *ctx,
                                      uint32_t *frames_processed,
                                      uint32_t *errors);

/* Contexto global */
extern AudioCaptureContext g_audio_ctx;

/* Buffers DMA globales (definidos en audio_capture.c, en RAM_D2) */
extern uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];
extern uint32_t dma_buf_b[AUDIO_DMA_BUFFER_SIZE];

#endif /* AUDIO_CAPTURE_H */
