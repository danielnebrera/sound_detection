/**
 * @file audio_capture.c
 * @brief Captura SAI2 A/B emparejada sin diagnosticos costosos.
 */

#include "audio_capture.h"
#include "sai.h"

#include <stdbool.h>
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
    "AUDIO_DMA_BUFFER_SIZE debe ser AUDIO_BUFFER_SIZE * 4"
);
#endif

static uint32_t minimum_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

static uint32_t absolute_difference_u32(uint32_t a, uint32_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

static void reset_event_counters(AudioCaptureContext *ctx)
{
    ctx->dma_a_half_produced = 0U;
    ctx->dma_a_full_produced = 0U;
    ctx->dma_b_half_produced = 0U;
    ctx->dma_b_full_produced = 0U;
    ctx->dma_half_consumed = 0U;
    ctx->dma_full_consumed = 0U;
    ctx->expected_half = 1U;
    ctx->buffer_state = AUDIO_BUFFER_EMPTY;
}

int audio_capture_init(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return -1;
    }

    memset(dma_buf_a, 0, sizeof(dma_buf_a));
    memset(dma_buf_b, 0, sizeof(dma_buf_b));
    memset(ctx, 0, sizeof(*ctx));

    reset_event_counters(ctx);
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
    reset_event_counters(ctx);
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
        HAL_SAI_DMAStop(&hsai_BlockB2);
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

void audio_capture_deinterleave(AudioCaptureContext *ctx, uint8_t half)
{
    if (ctx == NULL)
    {
        return;
    }

    const uint32_t offset =
        (half == 0U) ? 0U : (AUDIO_DMA_BUFFER_SIZE / 2U);

    for (uint32_t i = 0U; i < AUDIO_BUFFER_SIZE; i++)
    {
        const uint32_t index =
            offset + (i * AUDIO_SLOTS_PER_FRAME);

        ctx->ch0[i] = (int32_t)dma_buf_b[index + 1U]; /* Mic2 / left  */
        ctx->ch1[i] = (int32_t)dma_buf_a[index + 1U]; /* Mic4 / top   */
        ctx->ch2[i] = (int32_t)dma_buf_a[index];      /* Mic3 / back  */
        ctx->ch3[i] = (int32_t)dma_buf_b[index];      /* Mic1 / right */
    }
}

AudioBufferState audio_capture_get_data(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return AUDIO_BUFFER_EMPTY;
    }

    bool process_half = false;
    bool process_full = false;
    bool overrun = false;

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();

    const uint32_t a_half = ctx->dma_a_half_produced;
    const uint32_t b_half = ctx->dma_b_half_produced;
    const uint32_t a_full = ctx->dma_a_full_produced;
    const uint32_t b_full = ctx->dma_b_full_produced;

    const uint32_t paired_half = minimum_u32(a_half, b_half);
    const uint32_t paired_full = minimum_u32(a_full, b_full);

    const uint32_t pending_half = paired_half - ctx->dma_half_consumed;
    const uint32_t pending_full = paired_full - ctx->dma_full_consumed;

    if ((absolute_difference_u32(a_half, b_half) > 1U) ||
        (absolute_difference_u32(a_full, b_full) > 1U) ||
        (pending_half > 1U) ||
        (pending_full > 1U))
    {
        overrun = true;
    }
    else if (ctx->expected_half != 0U)
    {
        if (pending_half > 0U)
        {
            ctx->dma_half_consumed++;
            ctx->expected_half = 0U;
            process_half = true;
        }
    }
    else
    {
        if (pending_full > 0U)
        {
            ctx->dma_full_consumed++;
            ctx->expected_half = 1U;
            process_full = true;
        }
    }

    if (primask == 0U)
    {
        __enable_irq();
    }

    if (overrun)
    {
        ctx->error_count++;
        ctx->overrun_count++;
        ctx->buffer_state = AUDIO_BUFFER_OVERRUN;
        return AUDIO_BUFFER_OVERRUN;
    }

    if (process_half)
    {
        audio_capture_deinterleave(ctx, 0U);
        ctx->blocks_processed++;
        ctx->buffer_state = AUDIO_BUFFER_HALF;
        return AUDIO_BUFFER_HALF;
    }

    if (process_full)
    {
        audio_capture_deinterleave(ctx, 1U);
        ctx->blocks_processed++;
        ctx->buffer_state = AUDIO_BUFFER_FULL;
        return AUDIO_BUFFER_FULL;
    }

    return AUDIO_BUFFER_EMPTY;
}

int32_t *audio_capture_get_channel(
    AudioCaptureContext *ctx,
    uint8_t channel)
{
    if ((ctx == NULL) || (channel >= AUDIO_CHANNELS))
    {
        return NULL;
    }

    switch (channel)
    {
        case 0U: return ctx->ch0;
        case 1U: return ctx->ch1;
        case 2U: return ctx->ch2;
        case 3U: return ctx->ch3;
        default: return NULL;
    }
}

void audio_capture_get_stats(
    AudioCaptureContext *ctx,
    uint32_t *blocks_processed,
    uint32_t *errors,
    uint32_t *overruns)
{
    if (ctx == NULL)
    {
        return;
    }

    if (blocks_processed != NULL)
    {
        *blocks_processed = ctx->blocks_processed;
    }

    if (errors != NULL)
    {
        *errors = ctx->error_count;
    }

    if (overruns != NULL)
    {
        *overruns = ctx->overrun_count;
    }
}

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    sai_half_count++;

    if (hsai == &hsai_BlockA2)
    {
        g_audio_ctx.dma_a_half_produced++;
    }
    else if (hsai == &hsai_BlockB2)
    {
        g_audio_ctx.dma_b_half_produced++;
        block_b_half_count++;
    }
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    sai_full_count++;

    if (hsai == &hsai_BlockA2)
    {
        g_audio_ctx.dma_a_full_produced++;
    }
    else if (hsai == &hsai_BlockB2)
    {
        g_audio_ctx.dma_b_full_produced++;
        block_b_full_count++;
    }
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
    (void)hsai;
    g_audio_ctx.error_count++;
}
