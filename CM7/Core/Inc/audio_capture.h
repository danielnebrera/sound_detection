/**
 * @file audio_capture.h
 * @brief Captura SAI2_A + SAI2_B sincronizada para 4 microfonos.
 *
 * Mapeo real entregado a drone_detection:
 *   ch0 -> Mic2 -> SAI2_B slot impar -> SEL=VCC
 *   ch1 -> Mic4 -> SAI2_A slot impar -> SEL=VCC
 *   ch2 -> Mic3 -> SAI2_A slot par   -> SEL=GND
 *   ch3 -> Mic1 -> SAI2_B slot par   -> SEL=GND
 *
 * Cada mitad DMA contiene AUDIO_BUFFER_SIZE muestras POR MICROFONO.
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>
#include <string.h>
#include "main.h"

/* ================= CONFIGURACION ================= */

#define AUDIO_SAMPLE_RATE       44100U
#define AUDIO_CHANNELS          4U

/* Muestras por microfono entregadas en cada HALF/FULL callback emparejado. */
#define AUDIO_BUFFER_SIZE       512U

/* Cada SAI recibe dos slots: par e impar. */
#define AUDIO_SLOTS_PER_FRAME   2U

/* DMA circular: primera mitad + segunda mitad. */
#define AUDIO_DMA_HALVES        2U

/*
 * 512 muestras/canal
 * x 2 slots por trama
 * x 2 mitades DMA
 * = 2048 palabras de 32 bits por bloque SAI.
 */
#define AUDIO_DMA_BUFFER_SIZE \
    (AUDIO_BUFFER_SIZE * AUDIO_SLOTS_PER_FRAME * AUDIO_DMA_HALVES)

#define AUDIO_SAMPLE_BITS       32U
#define AUDIO_SAMPLE_BYTES      (AUDIO_SAMPLE_BITS / 8U)

/* ================= ESTRUCTURAS ================= */

typedef enum
{
    AUDIO_BUFFER_EMPTY = 0,
    AUDIO_BUFFER_HALF  = 1,
    AUDIO_BUFFER_FULL  = 2
} AudioBufferState;

typedef struct
{
    /* Canales desentrelazados: 512 muestras nuevas en cada evento. */
    int32_t ch0[AUDIO_BUFFER_SIZE];  /* Mic2: B impar, SEL=VCC */
    int32_t ch1[AUDIO_BUFFER_SIZE];  /* Mic4: A impar, SEL=VCC */
    int32_t ch2[AUDIO_BUFFER_SIZE];  /* Mic3: A par,   SEL=GND */
    int32_t ch3[AUDIO_BUFFER_SIZE];  /* Mic1: B par,   SEL=GND */

    /*
     * Flags separados por bloque.
     * Solo se publica una mitad cuando A y B completaron la misma mitad.
     */
    volatile uint8_t dma_a_half;
    volatile uint8_t dma_a_full;
    volatile uint8_t dma_b_half;
    volatile uint8_t dma_b_full;

    /* Compatibilidad con codigo anterior. */
    volatile uint8_t dma_half_complete;
    volatile uint8_t dma_full_complete;

    volatile AudioBufferState buffer_state;

    uint32_t frame_count;
    uint32_t error_count;

} AudioCaptureContext;

/* ================= API PUBLICA ================= */

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
    uint32_t *frames_processed,
    uint32_t *errors
);

extern AudioCaptureContext g_audio_ctx;

extern uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];
extern uint32_t dma_buf_b[AUDIO_DMA_BUFFER_SIZE];

#endif /* AUDIO_CAPTURE_H */
