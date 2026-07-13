/**
 * @file audio_capture.c — v9 estable
 *
 * Sistema activo con 2 micrófonos ICS-43434 (SEL=VCC, canal RIGHT):
 *   ch0 = Mic2 — SAI2_SD_B (PG10), slot impar, SEL=VCC ✅
 *   ch1 = Mic4 — SAI2_SD_A (PI6),  slot impar, SEL=VCC ✅
 *   ch2 = Mic3 — SAI2_SD_A (PI6),  slot par,   SEL=GND (no activo)
 *   ch3 = Mic1 — SAI2_SD_B (PG10), slot par,   SEL=GND (no activo)
 *
 * Trigger por Block_A (maestro). Esclavo primero, maestro después.
 */

#include "audio_capture.h"
#include <stdio.h>
#include <math.h>

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

    /* Esclavo primero */
    if (HAL_SAI_Receive_DMA(&hsai_BlockB2,
                            (uint8_t *)dma_buf_b,
                            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
        ctx->error_count++;
        return -2;
    }

    HAL_Delay(5);

    /* Maestro después — genera SCK y WS */
    if (HAL_SAI_Receive_DMA(&hsai_BlockA2,
                            (uint8_t *)dma_buf_a,
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

void audio_capture_deinterleave(AudioCaptureContext *ctx, uint8_t half)
{
    if (!ctx) return;

    /* ── DEBUG RAW ─────────────────────────────────────────── */
    static uint32_t dbg_count = 0;
    if (++dbg_count >= 86) {
        dbg_count = 0;

        float db_b_par = 0, db_b_imp = 0, db_a_par = 0, db_a_imp = 0;
        for (int i = 0; i < 64; i++) {
            float s;
            s = (float)((int32_t)dma_buf_b[i*2]   >> 8) / 8388608.0f; db_b_par += s*s;
            s = (float)((int32_t)dma_buf_b[i*2+1] >> 8) / 8388608.0f; db_b_imp += s*s;
            s = (float)((int32_t)dma_buf_a[i*2]   >> 8) / 8388608.0f; db_a_par += s*s;
            s = (float)((int32_t)dma_buf_a[i*2+1] >> 8) / 8388608.0f; db_a_imp += s*s;
        }
        db_b_par = (db_b_par > 1e-12f) ? 20.0f*log10f(sqrtf(db_b_par/64)) : -120.0f;
        db_b_imp = (db_b_imp > 1e-12f) ? 20.0f*log10f(sqrtf(db_b_imp/64)) : -120.0f;
        db_a_par = (db_a_par > 1e-12f) ? 20.0f*log10f(sqrtf(db_a_par/64)) : -120.0f;
        db_a_imp = (db_a_imp > 1e-12f) ? 20.0f*log10f(sqrtf(db_a_imp/64)) : -120.0f;

        printf("[RAW] B_par:%5.1f B_imp:%5.1f A_par:%5.1f A_imp:%5.1f dBFS\r\n",
               db_b_par, db_b_imp, db_a_par, db_a_imp);
    }
    /* ─────────────────────────────────────────────────────── */

    uint32_t offset = (half == 0U) ? 0U : (AUDIO_DMA_BUFFER_SIZE / 2U);

    for (uint32_t i = 0; i < AUDIO_BUFFER_SIZE; i++)
    {
        uint32_t idx = offset + i * 2;
        if (idx + 1 >= AUDIO_DMA_BUFFER_SIZE) break;

        ctx->ch0[i] = (int32_t)dma_buf_b[idx + 1]; /* Mic2 slot impar SEL=VCC */
        ctx->ch1[i] = (int32_t)dma_buf_a[idx + 1]; /* Mic4 slot impar SEL=VCC */
        ctx->ch2[i] = (int32_t)dma_buf_a[idx];      /* Mic3 slot par   SEL=GND */
        ctx->ch3[i] = (int32_t)dma_buf_b[idx];      /* Mic1 slot par   SEL=GND */
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
        case 0: return ctx->ch0; /* Mic2 */
        case 1: return ctx->ch1; /* Mic4 */
        case 2: return ctx->ch2; /* Mic3 */
        case 3: return ctx->ch3; /* Mic1 */
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
