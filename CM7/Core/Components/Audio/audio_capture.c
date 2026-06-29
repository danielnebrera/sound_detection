/**
 * @file audio_capture.c — v5
 * Usa Mic1 (ch0, SAI2_SD_B slot impar) + Mic3 (ch2, SAI2_SD_A slot par)
 * Trigger por Block_A. Maestro primero, delay 5ms, esclavo después.
 */

#include "audio_capture.h"
#include <stdio.h>

__attribute__((section(".RAM_D2_bss")))
uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];

__attribute__((section(".RAM_D2_bss")))
uint32_t dma_buf_b[AUDIO_DMA_BUFFER_SIZE];

AudioCaptureContext g_audio_ctx = {0};

extern SAI_HandleTypeDef hsai_BlockA2;
extern SAI_HandleTypeDef hsai_BlockB2;

volatile uint32_t sai_half_count = 0;
volatile uint32_t sai_full_count = 0;
volatile uint32_t block_b_half_count = 0;
volatile uint32_t block_b_full_count = 0;

int audio_capture_init(AudioCaptureContext *ctx)
{
    if (!ctx) return -1;
    memset(dma_buf_a, 0, sizeof(dma_buf_a));
    memset(dma_buf_b, 0, sizeof(dma_buf_b));
    memset(ctx->ch0, 0, sizeof(ctx->ch0));
    memset(ctx->ch1, 0, sizeof(ctx->ch1));
    memset(ctx->ch2, 0, sizeof(ctx->ch2));
    memset(ctx->ch3, 0, sizeof(ctx->ch3));
    ctx->dma_a_half        = 0;
    ctx->dma_a_full        = 0;
    ctx->dma_b_half        = 0;
    ctx->dma_b_full        = 0;
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

    /* Maestro primero */
    if (HAL_SAI_Receive_DMA(&hsai_BlockA2,
                            (uint8_t *)dma_buf_a,
                            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
        ctx->error_count++;
        return -2;
    }

    HAL_Delay(5);

    /* Esclavo después */
    if (HAL_SAI_Receive_DMA(&hsai_BlockB2,
                            (uint8_t *)dma_buf_b,
                            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
        ctx->error_count++;
        return -1;
    }

    return 0;
}

int audio_capture_stop(AudioCaptureContext *ctx)
{
    if (!ctx) return -1;
    HAL_SAI_DMAStop(&hsai_BlockA2);
    HAL_SAI_DMAStop(&hsai_BlockB2);
    return 0;
}

/* ============================================================
 * DEINTERLEAVE
 *
 * dma_buf_b (SAI2_SD_B, PG10):
 *   idx+1 → slot impar → Mic1 (SEL=VCC) ← señal confirmada
 *   idx   → slot par   → Mic2 (SEL=GND) ← señal débil/problema
 *
 * dma_buf_a (SAI2_SD_A, PI6):
 *   idx   → slot par   → Mic3 (SEL=GND)
 *   idx+1 → slot impar → Mic4 (SEL=VCC)
 *
 * ch0 = Mic1 (dma_buf_b slot impar) — SAI2_SD_B
 * ch1 = Mic3 (dma_buf_a slot par)   — SAI2_SD_A ← nuevo
 * ch2 = Mic3 (dma_buf_a slot par)   — duplicado por compatibilidad
 * ch3 = Mic4 (dma_buf_a slot impar) — SAI2_SD_A
 * ============================================================ */
void audio_capture_deinterleave(AudioCaptureContext *ctx, uint8_t half)
{
    if (!ctx) return;

    uint32_t src_offset = (half == 0) ? 0 : AUDIO_BUFFER_SIZE;

    for (uint32_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
    {
        uint32_t idx = src_offset + i * 2;

        if (idx + 1 >= AUDIO_DMA_BUFFER_SIZE) break;

        /* Mic1 — SAI2_SD_B slot impar (confirmado funciona) */
        ctx->ch0[i] = (int32_t)dma_buf_b[idx + 1];

        /* Mic3 — SAI2_SD_A slot par (nuevo canal activo) */
        ctx->ch1[i] = (int32_t)dma_buf_a[idx + 1];  /* slot impar — señal real */


        /* Mic3 y Mic4 en ch2/ch3 para referencia */
        ctx->ch2[i] = (int32_t)dma_buf_a[idx];
        ctx->ch3[i] = (int32_t)dma_buf_a[idx + 1];
    }
}

AudioBufferState audio_capture_get_data(AudioCaptureContext *ctx)
{
    if (!ctx) return AUDIO_BUFFER_EMPTY;

    if (ctx->dma_a_half)
    {
        ctx->dma_a_half = 0;
        audio_capture_deinterleave(ctx, 0);
        ctx->buffer_state = AUDIO_BUFFER_HALF;
        ctx->frame_count++;
        return AUDIO_BUFFER_HALF;
    }

    if (ctx->dma_a_full)
    {
        ctx->dma_a_full = 0;
        audio_capture_deinterleave(ctx, 1);
        ctx->buffer_state = AUDIO_BUFFER_FULL;
        ctx->frame_count++;
        return AUDIO_BUFFER_FULL;
    }

    return AUDIO_BUFFER_EMPTY;
}

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

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    sai_half_count++;
    if (hsai == &hsai_BlockA2) g_audio_ctx.dma_a_half = 1;
    if (hsai == &hsai_BlockB2) {
        g_audio_ctx.dma_b_half = 1;
        block_b_half_count++;
    }
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    sai_full_count++;
    if (hsai == &hsai_BlockA2) g_audio_ctx.dma_a_full = 1;
    if (hsai == &hsai_BlockB2) {
        g_audio_ctx.dma_b_full = 1;
        block_b_full_count++;
    }
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    g_audio_ctx.error_count++;
}
