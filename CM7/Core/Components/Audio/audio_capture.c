/**
 * @file audio_capture.c
 * @brief Empareja callbacks SAI2 A/B y publica cada mitad al ring SDRAM.
 */

#include "audio_capture.h"
#include "audio_recorder.h"
#include "sai.h"

#include <string.h>

__attribute__((section(".RAM_D2_bss"), aligned(32)))
uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];

__attribute__((section(".RAM_D2_bss"), aligned(32)))
uint32_t dma_buf_b[AUDIO_DMA_BUFFER_SIZE];

AudioCaptureContext g_audio_ctx = {0};

extern SAI_HandleTypeDef hsai_BlockA2;
extern SAI_HandleTypeDef hsai_BlockB2;

volatile uint32_t sai_half_count = 0U;
volatile uint32_t sai_full_count = 0U;
volatile uint32_t block_b_half_count = 0U;
volatile uint32_t block_b_full_count = 0U;

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(
    AUDIO_DMA_BUFFER_SIZE == (AUDIO_BUFFER_SIZE * 4U),
    "AUDIO_DMA_BUFFER_SIZE must be AUDIO_BUFFER_SIZE * 4"
);
#endif

static uint32_t absolute_difference_u32(uint32_t a, uint32_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

static void reset_context(AudioCaptureContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

static void mark_fatal_error(AudioCaptureContext *ctx)
{
    ctx->fatal_error = 1U;
    ctx->error_count++;
    ctx->overrun_count++;
}

static void handle_dma_event_from_isr(
    AudioCaptureContext *ctx,
    bool block_a,
    uint8_t half)
{
    if ((ctx == NULL) || (half >= AUDIO_DMA_HALVES) || ctx->fatal_error)
    {
        return;
    }

    if (block_a)
    {
        ctx->dma_a_generation[half]++;
    }
    else
    {
        ctx->dma_b_generation[half]++;
    }

    const uint32_t a = ctx->dma_a_generation[half];
    const uint32_t b = ctx->dma_b_generation[half];
    const uint32_t consumed = ctx->consumed_generation[half];
    const uint32_t skew = absolute_difference_u32(a, b);

    if (skew > ctx->maximum_pair_skew)
    {
        ctx->maximum_pair_skew = skew;
    }

    if ((skew > 1U) ||
        ((a - consumed) > 1U) ||
        ((b - consumed) > 1U))
    {
        ctx->pair_mismatch_count++;
        mark_fatal_error(ctx);
        return;
    }

    if ((a == b) && (a == (consumed + 1U)))
    {
        ctx->consumed_generation[half] = a;

        if (!audio_recorder_on_dma_pair_from_isr(
                half,
                dma_buf_a,
                dma_buf_b))
        {
            mark_fatal_error(ctx);
            return;
        }

        ctx->paired_blocks++;
    }
}

int audio_capture_init(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return -1;
    }

    memset(dma_buf_a, 0, sizeof(dma_buf_a));
    memset(dma_buf_b, 0, sizeof(dma_buf_b));
    reset_context(ctx);
    return 0;
}

int audio_capture_start(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return -1;
    }

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    reset_context(ctx);
    if (primask == 0U)
    {
        __enable_irq();
    }

    /* B es esclavo: se arma antes de que A genere SCK/FS. */
    if (HAL_SAI_Receive_DMA(
            &hsai_BlockB2,
            (uint8_t *)dma_buf_b,
            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
        ctx->error_count++;
        return -2;
    }

    HAL_Delay(5U);

    if (HAL_SAI_Receive_DMA(
            &hsai_BlockA2,
            (uint8_t *)dma_buf_a,
            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
        (void)HAL_SAI_DMAStop(&hsai_BlockB2);
        ctx->error_count++;
        return -3;
    }

    return 0;
}

int audio_capture_stop(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return -1;
    }

    const HAL_StatusTypeDef result_a = HAL_SAI_DMAStop(&hsai_BlockA2);
    const HAL_StatusTypeDef result_b = HAL_SAI_DMAStop(&hsai_BlockB2);

    if ((result_a != HAL_OK) || (result_b != HAL_OK))
    {
        ctx->error_count++;
        return -2;
    }

    return 0;
}

bool audio_capture_has_fatal_error(const AudioCaptureContext *ctx)
{
    return (ctx != NULL) && (ctx->fatal_error != 0U);
}

void audio_capture_get_stats(
    const AudioCaptureContext *ctx,
    AudioCaptureStats *stats)
{
    if ((ctx == NULL) || (stats == NULL))
    {
        return;
    }

    stats->paired_blocks = ctx->paired_blocks;
    stats->errors = ctx->error_count;
    stats->overruns = ctx->overrun_count;
    stats->pair_mismatches = ctx->pair_mismatch_count;
    stats->maximum_pair_skew = ctx->maximum_pair_skew;
    stats->fatal_error = (ctx->fatal_error != 0U);
}

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    sai_half_count++;

    if (hsai == &hsai_BlockA2)
    {
        handle_dma_event_from_isr(&g_audio_ctx, true, 0U);
    }
    else if (hsai == &hsai_BlockB2)
    {
        block_b_half_count++;
        handle_dma_event_from_isr(&g_audio_ctx, false, 0U);
    }
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    sai_full_count++;

    if (hsai == &hsai_BlockA2)
    {
        handle_dma_event_from_isr(&g_audio_ctx, true, 1U);
    }
    else if (hsai == &hsai_BlockB2)
    {
        block_b_full_count++;
        handle_dma_event_from_isr(&g_audio_ctx, false, 1U);
    }
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    g_audio_ctx.fatal_error = 1U;
    g_audio_ctx.error_count++;
}
