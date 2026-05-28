/**
 * @file audio_capture.c
 * @brief SAI + DMA Audio Capture Implementation for STM32H7
 */

#include "audio_capture.h"

/* ============================================================
 * Buffers DMA en RAM_D2 (0x30000000) — accesibles por DMA1
 * NO deben estar en el struct AudioCaptureContext
 * ============================================================ */
__attribute__((section(".RAM_D2_bss")))
uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];

__attribute__((section(".RAM_D2_bss")))
uint32_t dma_buf_b[AUDIO_DMA_BUFFER_SIZE];

/* Contexto global */
AudioCaptureContext g_audio_ctx = {0};

/* SAI handles (definidos en sai.c) */
extern SAI_HandleTypeDef hsai_BlockA1;
extern SAI_HandleTypeDef hsai_BlockB1;

/* Contadores de debug */
volatile uint32_t sai_half_count = 0;
volatile uint32_t sai_full_count = 0;

/* ============================================================
 * INIT / START / STOP
 * ============================================================ */

int audio_capture_init(AudioCaptureContext *ctx)
{
    if (!ctx) return -1;

    memset(dma_buf_a, 0, sizeof(dma_buf_a));
    memset(dma_buf_b, 0, sizeof(dma_buf_b));
    memset(ctx->ch0, 0, sizeof(ctx->ch0));
    memset(ctx->ch1, 0, sizeof(ctx->ch1));
    memset(ctx->ch2, 0, sizeof(ctx->ch2));
    memset(ctx->ch3, 0, sizeof(ctx->ch3));

    ctx->dma_half_complete = 0;
    ctx->dma_full_complete = 0;
    ctx->buffer_state      = AUDIO_BUFFER_EMPTY;
    ctx->frame_count       = 0;
    ctx->error_count       = 0;

    return 0;
}

int audio_capture_start(AudioCaptureContext *ctx)
{
    if (!ctx) return -1;

    if (HAL_SAI_Receive_DMA(&hsai_BlockA1,
                            (uint8_t *)dma_buf_a,
                            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
        ctx->error_count++;
        return -1;
    }

    if (HAL_SAI_Receive_DMA(&hsai_BlockB1,
                            (uint8_t *)dma_buf_b,
                            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
        ctx->error_count++;
        return -2;
    }

    return 0;
}

int audio_capture_stop(AudioCaptureContext *ctx)
{
    if (!ctx) return -1;
    HAL_SAI_DMAStop(&hsai_BlockA1);
    HAL_SAI_DMAStop(&hsai_BlockB1);
    return 0;
}

/* ============================================================
 * DEINTERLEAVE
 *
 * dma_buf_a contiene pares estereo: [L0, R0, L1, R1, ...]
 * half=0 → procesa primera mitad  (indices 0   .. BUFFER_SIZE-1)
 * half=1 → procesa segunda mitad  (indices BUFFER_SIZE .. 2*BUFFER_SIZE-1)
 *
 * Dentro de cada mitad hay BUFFER_SIZE muestras stereo,
 * es decir BUFFER_SIZE*2 uint32_t en el buffer DMA.
 * ============================================================ */
void audio_capture_deinterleave(AudioCaptureContext *ctx, uint8_t half)
{
    if (!ctx) return;

    /* Offset de destino en los arrays ch0..ch3 — siempre 0..BUFFER_SIZE-1
     * porque solo guardamos la mitad activa (ping-pong simple)           */
    uint32_t src_offset = (half == 0) ? 0 : AUDIO_BUFFER_SIZE;

    for (uint32_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
    {
        uint32_t idx = src_offset + i * 2;

        /* Verificar que no nos salimos del buffer DMA */
        if (idx + 1 >= AUDIO_DMA_BUFFER_SIZE * 2) break;

        ctx->ch0[i] = (int32_t)dma_buf_a[idx];         /* Mic1 L */
        ctx->ch1[i] = (int32_t)dma_buf_a[idx + 1];     /* Mic2 R */
        ctx->ch2[i] = (int32_t)dma_buf_b[idx];         /* Mic3 L */
        ctx->ch3[i] = (int32_t)dma_buf_b[idx + 1];     /* Mic4 R */
    }
}

/* ============================================================
 * POLL
 * ============================================================ */
AudioBufferState audio_capture_get_data(AudioCaptureContext *ctx)
{
    if (!ctx) return AUDIO_BUFFER_EMPTY;

    if (ctx->dma_half_complete)
    {
        ctx->dma_half_complete = 0;
        audio_capture_deinterleave(ctx, 0);
        ctx->buffer_state = AUDIO_BUFFER_HALF;
        ctx->frame_count++;
        return AUDIO_BUFFER_HALF;
    }

    if (ctx->dma_full_complete)
    {
        ctx->dma_full_complete = 0;
        audio_capture_deinterleave(ctx, 1);
        ctx->buffer_state = AUDIO_BUFFER_FULL;
        ctx->frame_count++;
        return AUDIO_BUFFER_FULL;
    }

    return AUDIO_BUFFER_EMPTY;
}

/* ============================================================
 * GETTERS
 * ============================================================ */
int32_t* audio_capture_get_channel(AudioCaptureContext *ctx, uint8_t channel)
{
    if (!ctx || channel >= AUDIO_CHANNELS) return NULL;
    switch (channel)
    {
        case 0: return ctx->ch0;
        case 1: return ctx->ch1;
        case 2: return ctx->ch2;
        case 3: return ctx->ch3;
        default: return NULL;
    }
}

void audio_capture_get_stats(AudioCaptureContext *ctx,
                             uint32_t *frames_processed,
                             uint32_t *errors)
{
    if (!ctx) return;
    if (frames_processed) *frames_processed = ctx->frame_count;
    if (errors)           *errors           = ctx->error_count;
}

/* ============================================================
 * DMA CALLBACKS
 * ============================================================ */
void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    sai_half_count++;
    if (hsai == &hsai_BlockA1)
        g_audio_ctx.dma_half_complete = 1;
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    sai_full_count++;
    if (hsai == &hsai_BlockA1)
        g_audio_ctx.dma_full_complete = 1;
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    g_audio_ctx.error_count++;
}
