/**
 * @file audio_capture.c
 * @brief Empareja callbacks SAI2 A/B y publica cada mitad al ring SDRAM.
 */

#include "audio_capture.h"
#include "audio_recorder.h"
#include "sai.h"

#include <string.h>
#include <stdio.h>

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

/*
 * RAWB-DIAG: bounded snapshot of unmodified SAI2_B DMA words.
 * Read only a completed, paired DMA half; never transmit from an ISR.
 * Slot 0 = CH3/MIC7, slot 1 = CH0/MIC2.
 * Disable this experiment by setting AUDIO_RAWB_DIAG_ENABLE to 0.
 */
#ifndef AUDIO_RAWB_DIAG_ENABLE
#define AUDIO_RAWB_DIAG_ENABLE 1U
#endif

#if AUDIO_RAWB_DIAG_ENABLE
#define AUDIO_RAWB_DIAG_EVENT_COUNT  4U
#define AUDIO_RAWB_DIAG_PRE_FRAMES   4U
#define AUDIO_RAWB_DIAG_POST_FRAMES  8U
#define AUDIO_RAWB_DIAG_FRAME_COUNT  (AUDIO_RAWB_DIAG_PRE_FRAMES + 1U + AUDIO_RAWB_DIAG_POST_FRAMES)
#define AUDIO_RAWB_DIAG_THRESHOLD  (-1500)

typedef struct
{
    uint32_t paired_index;
    uint32_t half_generation;
    uint16_t frame_in_half;
    uint8_t half;
    uint32_t words[AUDIO_RAWB_DIAG_FRAME_COUNT][AUDIO_SLOTS_PER_FRAME];
} AudioRawBDiagEvent;

/* Use internal RAM, not SDRAM, so snapshots survive DMA buffer reuse. */
static AudioRawBDiagEvent s_rawb_events[AUDIO_RAWB_DIAG_EVENT_COUNT];
static uint32_t s_rawb_event_count = 0U;
static uint32_t s_rawb_scanned_halves = 0U;
static uint32_t s_rawb_below_threshold_count = 0U;
static uint32_t s_rawb_baseline[AUDIO_RAWB_DIAG_FRAME_COUNT][AUDIO_SLOTS_PER_FRAME];
static uint8_t s_rawb_baseline_available = 0U;

static int16_t audio_rawb_pcm16(uint32_t value)
{
    int32_t sample = (int32_t)(value & 0x00FFFFFFU);
    if ((sample & 0x00800000L) != 0)
    {
        sample |= (int32_t)0xFF000000U;
    }
    return (int16_t)(sample >> 8);
}

static void audio_rawb_diag_scan_paired_half(uint8_t half, uint32_t generation,
                                             uint32_t paired_index)
{
    const uint32_t half_offset = (half == 0U) ? 0U : (AUDIO_DMA_BUFFER_SIZE / 2U);
    s_rawb_scanned_halves++;

    if (s_rawb_baseline_available == 0U)
    {
        for (uint32_t k = 0U; k < AUDIO_RAWB_DIAG_FRAME_COUNT; k++)
        {
            const uint32_t word_index = half_offset + k * AUDIO_SLOTS_PER_FRAME;
            s_rawb_baseline[k][0] = dma_buf_b[word_index];
            s_rawb_baseline[k][1] = dma_buf_b[word_index + 1U];
        }
        s_rawb_baseline_available = 1U;
    }

    if (s_rawb_event_count >= AUDIO_RAWB_DIAG_EVENT_COUNT)
    {
        return;
    }

    uint32_t last_event_frame = 0U;
    bool have_event_in_this_half = false;
    for (uint32_t frame = AUDIO_RAWB_DIAG_PRE_FRAMES;
         frame + AUDIO_RAWB_DIAG_POST_FRAMES < AUDIO_BUFFER_SIZE;
         frame++)
    {
        const uint32_t index = half_offset + frame * AUDIO_SLOTS_PER_FRAME;
        const int16_t sample = audio_rawb_pcm16(dma_buf_b[index]);
        if (sample >= AUDIO_RAWB_DIAG_THRESHOLD)
        {
            continue;
        }
        s_rawb_below_threshold_count++;
        /* At most one snapshot per half; other anomalous samples are counted. */
        if (have_event_in_this_half ||
            s_rawb_event_count >= AUDIO_RAWB_DIAG_EVENT_COUNT)
        {
            continue;
        }
        last_event_frame = frame;
        have_event_in_this_half = true;
        AudioRawBDiagEvent *event = &s_rawb_events[s_rawb_event_count];
        event->paired_index = paired_index;
        event->half_generation = generation;
        event->frame_in_half = (uint16_t)last_event_frame;
        event->half = half;
        for (uint32_t k = 0U; k < AUDIO_RAWB_DIAG_FRAME_COUNT; k++)
        {
            const uint32_t frame_index = frame - AUDIO_RAWB_DIAG_PRE_FRAMES + k;
            const uint32_t word_index = half_offset + frame_index * AUDIO_SLOTS_PER_FRAME;
            event->words[k][0] = dma_buf_b[word_index];
            event->words[k][1] = dma_buf_b[word_index + 1U];
        }
        s_rawb_event_count++;
    }
}

static void audio_rawb_diag_emit(void)
{
    char message[192];
    (void)snprintf(message, sizeof(message),
                   "[RAWB_BEGIN] events=%lu baseline=%u scanned_halves=%lu "
                   "below=%lu threshold=%ld frames=%lu slot0=MIC7 slot1=MIC2\r\n",
                   (unsigned long)s_rawb_event_count,
                   (unsigned)s_rawb_baseline_available,
                   (unsigned long)s_rawb_scanned_halves,
                   (unsigned long)s_rawb_below_threshold_count,
                   (long)AUDIO_RAWB_DIAG_THRESHOLD,
                   (unsigned long)AUDIO_RAWB_DIAG_FRAME_COUNT);
    audio_recorder_send_text(message);

    if (s_rawb_baseline_available != 0U)
    {
        for (uint32_t k = 0U; k < AUDIO_RAWB_DIAG_FRAME_COUNT; k++)
        {
            const uint32_t word0 = s_rawb_baseline[k][0];
            const uint32_t word1 = s_rawb_baseline[k][1];
            (void)snprintf(message, sizeof(message),
                           "[RAWB_BASE] offset=%lu b0=%08lX b1=%08lX pcm0=%ld pcm1=%ld\r\n",
                           (unsigned long)k,
                           (unsigned long)word0, (unsigned long)word1,
                           (long)audio_rawb_pcm16(word0),
                           (long)audio_rawb_pcm16(word1));
            audio_recorder_send_text(message);
        }
    }

    for (uint32_t e = 0U; e < s_rawb_event_count; e++)
    {
        const AudioRawBDiagEvent *event = &s_rawb_events[e];
        (void)snprintf(message, sizeof(message),
                       "[RAWB_EVENT] id=%lu pair=%lu half=%u gen=%lu frame=%u\r\n",
                       (unsigned long)e, (unsigned long)event->paired_index,
                       (unsigned)event->half,
                       (unsigned long)event->half_generation,
                       (unsigned)event->frame_in_half);
        audio_recorder_send_text(message);
        for (uint32_t k = 0U; k < AUDIO_RAWB_DIAG_FRAME_COUNT; k++)
        {
            const uint32_t word0 = event->words[k][0];
            const uint32_t word1 = event->words[k][1];
            (void)snprintf(message, sizeof(message),
                           "[RAWB_FRAME] id=%lu rel=%ld b0=%08lX b1=%08lX pcm0=%ld pcm1=%ld\r\n",
                           (unsigned long)e,
                           (long)k - (long)AUDIO_RAWB_DIAG_PRE_FRAMES,
                           (unsigned long)word0, (unsigned long)word1,
                           (long)audio_rawb_pcm16(word0),
                           (long)audio_rawb_pcm16(word1));
            audio_recorder_send_text(message);
        }
    }
    audio_recorder_send_text("[RAWB_END]\r\n");
}
#endif /* AUDIO_RAWB_DIAG_ENABLE */

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

#if AUDIO_RAWB_DIAG_ENABLE
        /* Snapshot while the paired half is still valid and unmodified. */
        audio_rawb_diag_scan_paired_half(half, b, ctx->paired_blocks);
#endif

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
#if AUDIO_RAWB_DIAG_ENABLE
    memset(s_rawb_events, 0, sizeof(s_rawb_events));
    memset(s_rawb_baseline, 0, sizeof(s_rawb_baseline));
    s_rawb_event_count = 0U;
    s_rawb_scanned_halves = 0U;
    s_rawb_below_threshold_count = 0U;
    s_rawb_baseline_available = 0U;
#endif
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

#if AUDIO_RAWB_DIAG_ENABLE
    /* DMA is stopped. Emit only now, never from an ISR or during capture. */
    audio_rawb_diag_emit();
#endif
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
