/**
 * @file audio_capture.h
 * @brief SAI + DMA Audio Capture Driver for STM32H7
 *
 * Captures 4-channel audio from 2 SAI blocks (SAI2_A + SAI2_B)
 * with hardware multiplexing via L/R pins.
 *
 * Pinout:
 *   SAI2_A: SD_A (PI6)  - Mic3/Mic4  (Master RX)
 *   SAI2_B: SD_B (PG10) - Mic1/Mic2  (Slave  RX, sync to A)
 *   Shared: SCK (PI5), FS (PI7)
 *
 * FIX v2: Callbacks separados para Block_A y Block_B.
 * Solo se procesa cuando AMBOS DMAs completaron la misma mitad,
 * evitando race condition entre DMA1_Stream0 y DMA1_Stream1.
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
 *
 * FLAGS SEPARADOS por bloque SAI:
 *   dma_a_half / dma_a_full → Block_A (Mic3/Mic4, maestro)
 *   dma_b_half / dma_b_full → Block_B (Mic1/Mic2, esclavo)
 * Se procesa solo cuando ambos flags del mismo tipo están activos.
 */
typedef struct {
    /* Canales desentrelazados (salida procesada) */
    int32_t ch0[AUDIO_BUFFER_SIZE];  /* Microfono 1 */
    int32_t ch1[AUDIO_BUFFER_SIZE];  /* Microfono 2 */
    int32_t ch2[AUDIO_BUFFER_SIZE];  /* Microfono 3 */
    int32_t ch3[AUDIO_BUFFER_SIZE];  /* Microfono 4 */

    /* Flags separados por bloque SAI — seteados por callbacks DMA */
    volatile uint8_t dma_a_half;   /* Block_A (maestro) mitad */
    volatile uint8_t dma_a_full;   /* Block_A (maestro) completo */
    volatile uint8_t dma_b_half;   /* Block_B (esclavo) mitad */
    volatile uint8_t dma_b_full;   /* Block_B (esclavo) completo */

    /* Flags legacy — mantenidos por compatibilidad pero no se usan */
    volatile uint8_t dma_half_complete;
    volatile uint8_t dma_full_complete;

    volatile AudioBufferState buffer_state;

    /* Estadisticas */
    uint32_t frame_count;
    uint32_t error_count;

} AudioCaptureContext;

/* ============ PUBLIC API ============ */

int              audio_capture_init(AudioCaptureContext *ctx);
int              audio_capture_start(AudioCaptureContext *ctx);
int              audio_capture_stop(AudioCaptureContext *ctx);
AudioBufferState audio_capture_get_data(AudioCaptureContext *ctx);
int32_t*         audio_capture_get_channel(AudioCaptureContext *ctx, uint8_t channel);
void             audio_capture_deinterleave(AudioCaptureContext *ctx, uint8_t half);
void             audio_capture_get_stats(AudioCaptureContext *ctx,
                                         uint32_t *frames_processed,
                                         uint32_t *errors);

/* Contexto global */
extern AudioCaptureContext g_audio_ctx;

/* Buffers DMA globales (definidos en audio_capture.c, en RAM_D2) */
extern uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];
extern uint32_t dma_buf_b[AUDIO_DMA_BUFFER_SIZE];

#endif /* AUDIO_CAPTURE_H */
