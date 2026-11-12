/**
 * @file audio_capture.c
 * @brief Mantiene SAI2 A/B y diagnostica SAI1_B sin cambiar el recorder.
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

#if AUDIO_CAPTURE_C_ENABLE
/* 8 KiB extra RAM D2; revisar el .map antes de flashear. */
__attribute__((section(".RAM_D2_bss"), aligned(32)))
uint32_t dma_buf_c[AUDIO_DMA_BUFFER_SIZE];

/* Una instantanea corta e inmutable: nunca UART dentro de una ISR. */
static uint32_t s_rawc_first[AUDIO_C_RAW_FRAMES][AUDIO_SLOTS_PER_FRAME];
static volatile uint8_t s_rawc_saved = 0U;
#endif

#if AUDIO_CAPTURE_D_ENABLE
/* 8 KiB dedicados a D (SAI1_A, PB2) en RAM D2, sin pasar al recorder. */
__attribute__((section(".RAM_D2_bss"), aligned(32)))
uint32_t dma_buf_d[AUDIO_DMA_BUFFER_SIZE];
static uint32_t s_rawd_first[AUDIO_D_RAW_FRAMES][AUDIO_SLOTS_PER_FRAME];
static volatile uint8_t s_rawd_saved = 0U;
#endif

AudioCaptureContext g_audio_ctx = {0};

extern SAI_HandleTypeDef hsai_BlockA2;
extern SAI_HandleTypeDef hsai_BlockB2;
#if AUDIO_CAPTURE_C_ENABLE
extern SAI_HandleTypeDef hsai_BlockB1;
extern DMA_HandleTypeDef hdma_sai1_b;
#endif
#if AUDIO_CAPTURE_D_ENABLE
extern SAI_HandleTypeDef hsai_BlockA1;
extern DMA_HandleTypeDef hdma_sai1_a;
#endif

volatile uint32_t sai_half_count = 0U;
volatile uint32_t sai_full_count = 0U;
volatile uint32_t block_b_half_count = 0U;
volatile uint32_t block_b_full_count = 0U;
#if AUDIO_CAPTURE_C_ENABLE
volatile uint32_t block_c_half_count = 0U;
volatile uint32_t block_c_full_count = 0U;
#endif
#if AUDIO_CAPTURE_D_ENABLE
volatile uint32_t block_d_half_count = 0U;
volatile uint32_t block_d_full_count = 0U;
static volatile uint32_t s_ddiag_error_callbacks = 0U;
static uint32_t s_ddiag_start_result = 0xFFFFFFFFU;
static uint32_t s_ddiag_stop_result = 0xFFFFFFFFU;
#endif


#if AUDIO_CAPTURE_C_ENABLE
/* C1.1: lecturas de registros sin UART durante la captura. */
typedef struct
{
    uint32_t sai1_gcr, sai2_gcr, sai_cr1, sai_sr;
    uint32_t dma_cr, dma_ndtr, dma_par, dma_m0ar, dma_fcr;
    uint32_t dma_lisr, sai_err, dma_err;
    uint32_t sai_state, dma_state, c_half, c_full, a_half, b_half;
    uint32_t c_irq_enabled;
} AudioCDiagSnapshot;

static AudioCDiagSnapshot s_cdiag_arm;
static AudioCDiagSnapshot s_cdiag_start;
static AudioCDiagSnapshot s_cdiag_stop;
static AudioCDiagSnapshot s_cdiag_after_stop;
static uint32_t s_cdiag_start_result;
static uint32_t s_cdiag_stop_result;
static volatile uint32_t s_cdiag_error_callbacks;
static volatile uint32_t s_cdiag_stop_errors;

static void audio_cdiag_take(AudioCDiagSnapshot *out)
{
    out->sai1_gcr = SAI1->GCR;
    out->sai2_gcr = SAI2->GCR;
    out->sai_cr1 = hsai_BlockB1.Instance->CR1;
    out->sai_sr = hsai_BlockB1.Instance->SR;
    out->dma_cr = DMA1_Stream2->CR;
    out->dma_ndtr = DMA1_Stream2->NDTR;
    out->dma_par = DMA1_Stream2->PAR;
    out->dma_m0ar = DMA1_Stream2->M0AR;
    out->dma_fcr = DMA1_Stream2->FCR;
    out->dma_lisr = DMA1->LISR;
    out->sai_err = HAL_SAI_GetError(&hsai_BlockB1);
    out->dma_err = HAL_DMA_GetError(&hdma_sai1_b);
    out->sai_state = (uint32_t)HAL_SAI_GetState(&hsai_BlockB1);
    out->dma_state = (uint32_t)HAL_DMA_GetState(&hdma_sai1_b);
    out->c_half = block_c_half_count;
    out->c_full = block_c_full_count;
    out->a_half = g_audio_ctx.dma_a_generation[0];
    out->b_half = g_audio_ctx.dma_b_generation[0];
    out->c_irq_enabled = (uint32_t)NVIC_GetEnableIRQ(DMA1_Stream2_IRQn);
}

static void audio_cdiag_emit_one(const char *phase, const AudioCDiagSnapshot *v)
{
    char line[224];
    (void)snprintf(line, sizeof(line),
                   "[C1_REG] phase=%s GCR1=%08lX GCR2=%08lX SAI_CR1=%08lX SAI_SR=%08lX "
                   "SAI_STATE=%lu SAI_ERR=%08lX\r\n",
                   phase, (unsigned long)v->sai1_gcr, (unsigned long)v->sai2_gcr,
                   (unsigned long)v->sai_cr1, (unsigned long)v->sai_sr,
                   (unsigned long)v->sai_state, (unsigned long)v->sai_err);
    audio_recorder_send_text(line);
    (void)snprintf(line, sizeof(line),
                   "[C1_DMA] phase=%s CR=%08lX NDTR=%lu PAR=%08lX M0AR=%08lX FCR=%08lX "
                   "LISR=%08lX STATE=%lu ERR=%08lX IRQ=%lu\r\n",
                   phase, (unsigned long)v->dma_cr, (unsigned long)v->dma_ndtr,
                   (unsigned long)v->dma_par, (unsigned long)v->dma_m0ar,
                   (unsigned long)v->dma_fcr, (unsigned long)v->dma_lisr,
                   (unsigned long)v->dma_state, (unsigned long)v->dma_err,
                   (unsigned long)v->c_irq_enabled);
    audio_recorder_send_text(line);
    (void)snprintf(line, sizeof(line),
                   "[C1_COUNT] phase=%s c_half=%lu c_full=%lu a_half=%lu b_half=%lu\r\n",
                   phase, (unsigned long)v->c_half, (unsigned long)v->c_full,
                   (unsigned long)v->a_half, (unsigned long)v->b_half);
    audio_recorder_send_text(line);
}

static void audio_cdiag_emit(void)
{
    char line[160];
    (void)snprintf(line, sizeof(line),
                   "[C1_STATUS] start=%lu stop=%lu callback_err=%lu stop_err=%lu "
                   "c_errors=%lu c_error_code=%08lX\r\n",
                   (unsigned long)s_cdiag_start_result,
                   (unsigned long)s_cdiag_stop_result,
                   (unsigned long)s_cdiag_error_callbacks,
                   (unsigned long)s_cdiag_stop_errors,
                   (unsigned long)g_audio_ctx.c_error_count,
                   (unsigned long)g_audio_ctx.c_error_code);
    audio_recorder_send_text(line);
    audio_cdiag_emit_one("AFTER_C_ARM", &s_cdiag_arm);
    audio_cdiag_emit_one("AFTER_ABC_START", &s_cdiag_start);
    audio_cdiag_emit_one("BEFORE_STOP", &s_cdiag_stop);
    audio_cdiag_emit_one("AFTER_C_STOP", &s_cdiag_after_stop);
}
#endif

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

#if AUDIO_CAPTURE_C_ENABLE
/* CRC32 estandar de la instantanea RAW C; se calcula SOLO al parar. */
static uint32_t rawc_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < n; ++i)
    {
        crc ^= p[i];
        for (uint32_t b = 0U; b < 8U; ++b)
        {
            crc = (crc >> 1U) ^
                  ((crc & 1U) ? 0xEDB88320U : 0U);
        }
    }
    return ~crc;
}

/* La mitad completada por DMA C se inspecciona sin tocar A/B. */
static void rawc_snapshot_from_isr(uint8_t half)
{
    if ((half != 0U) || (s_rawc_saved != 0U))
    {
        return;
    }
    for (uint32_t frame = 0U; frame < AUDIO_C_RAW_FRAMES; ++frame)
    {
        const uint32_t i = frame * AUDIO_SLOTS_PER_FRAME;
        s_rawc_first[frame][0] = dma_buf_c[i];
        s_rawc_first[frame][1] = dma_buf_c[i + 1U];
    }
    __DMB();
    s_rawc_saved = 1U;
}

static void rawc_emit_after_stop(const AudioCaptureContext *ctx)
{
    char msg[160];
    const uint32_t crc = (s_rawc_saved != 0U)
        ? rawc_crc32((const uint8_t *)s_rawc_first,
                     (uint32_t)sizeof(s_rawc_first))
        : 0U;
    (void)snprintf(msg, sizeof(msg),
                   "[RAWC_BEGIN] saved=%u frames=%u half=%lu full=%lu errors=%lu "
                   "error_code=%08lX crc32=%08lX a_half=%lu b_half=%lu\r\n",
                   (unsigned)s_rawc_saved, (unsigned)AUDIO_C_RAW_FRAMES,
                   (unsigned long)block_c_half_count,
                   (unsigned long)block_c_full_count,
                   (unsigned long)ctx->c_error_count,
                   (unsigned long)ctx->c_error_code,
                   (unsigned long)crc,
                   (unsigned long)ctx->dma_a_generation[0],
                   (unsigned long)ctx->dma_b_generation[0]);
    audio_recorder_send_text(msg);
    if (s_rawc_saved != 0U)
    {
        for (uint32_t f = 0U; f < AUDIO_C_RAW_FRAMES; ++f)
        {
            (void)snprintf(msg, sizeof(msg),
                           "[RAWC_FRAME] f=%lu slot0=%08lX slot1=%08lX\r\n",
                           (unsigned long)f,
                           (unsigned long)s_rawc_first[f][0],
                           (unsigned long)s_rawc_first[f][1]);
            audio_recorder_send_text(msg);
        }
    }
    audio_recorder_send_text("[RAWC_END]\r\n");
}
#endif /* AUDIO_CAPTURE_C_ENABLE */

#if AUDIO_CAPTURE_D_ENABLE
/* Callback: copiar una sola instantanea de la primera mitad completada. */
static void rawd_snapshot_from_isr(uint8_t half)
{
    if ((half != 0U) || (s_rawd_saved != 0U))
    {
        return;
    }
    for (uint32_t f = 0U; f < AUDIO_D_RAW_FRAMES; ++f)
    {
        const uint32_t i = f * AUDIO_SLOTS_PER_FRAME;
        s_rawd_first[f][0] = dma_buf_d[i];
        s_rawd_first[f][1] = dma_buf_d[i + 1U];
    }
    __DMB();
    s_rawd_saved = 1U;
}

/* Solo se emite despues del STOP: no se utiliza UART desde la ISR. */
static void audio_ddiag_emit_after_stop(const AudioCaptureContext *ctx)
{
    char msg[192];
    (void)snprintf(msg, sizeof(msg),
        "[D_STATUS] start=%lu stop=%lu callback_err=%lu d_errors=%lu "
        "d_error_code=%08lX d_half=%lu d_full=%lu a_half=%lu b_half=%lu "
        "c_half=%lu irq=%lu\r\n",
        (unsigned long)s_ddiag_start_result,
        (unsigned long)s_ddiag_stop_result,
        (unsigned long)s_ddiag_error_callbacks,
        (unsigned long)ctx->d_error_count,
        (unsigned long)ctx->d_error_code,
        (unsigned long)block_d_half_count,
        (unsigned long)block_d_full_count,
        (unsigned long)ctx->dma_a_generation[0],
        (unsigned long)ctx->dma_b_generation[0],
        (unsigned long)ctx->dma_c_generation[0],
        (unsigned long)NVIC_GetEnableIRQ(DMA1_Stream3_IRQn));
    audio_recorder_send_text(msg);

    (void)snprintf(msg, sizeof(msg),
        "[D_RAW_BEGIN] saved=%u frames=%u NDTR=%lu "
        "SAI_STATE=%lu SAI_ERR=%08lX DMA_STATE=%lu DMA_ERR=%08lX\r\n",
        (unsigned)s_rawd_saved, (unsigned)AUDIO_D_RAW_FRAMES,
        (unsigned long)DMA1_Stream3->NDTR,
        (unsigned long)HAL_SAI_GetState(&hsai_BlockA1),
        (unsigned long)HAL_SAI_GetError(&hsai_BlockA1),
        (unsigned long)HAL_DMA_GetState(&hdma_sai1_a),
        (unsigned long)HAL_DMA_GetError(&hdma_sai1_a));
    audio_recorder_send_text(msg);

    if (s_rawd_saved != 0U)
    {
        for (uint32_t f = 0U; f < AUDIO_D_RAW_FRAMES; ++f)
        {
            (void)snprintf(msg, sizeof(msg),
                "[D_RAW_FRAME] f=%lu slot0=%08lX slot1=%08lX\r\n",
                (unsigned long)f,
                (unsigned long)s_rawd_first[f][0],
                (unsigned long)s_rawd_first[f][1]);
            audio_recorder_send_text(msg);
        }
    }
    audio_recorder_send_text("[D_RAW_END]\r\n");
}
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
#if AUDIO_CAPTURE_C_ENABLE
    memset(dma_buf_c, 0, sizeof(dma_buf_c));
    memset(s_rawc_first, 0, sizeof(s_rawc_first));
    s_rawc_saved = 0U;
    block_c_half_count = 0U;
    block_c_full_count = 0U;
    memset(&s_cdiag_arm, 0, sizeof(s_cdiag_arm));
    memset(&s_cdiag_start, 0, sizeof(s_cdiag_start));
    memset(&s_cdiag_stop, 0, sizeof(s_cdiag_stop));
    memset(&s_cdiag_after_stop, 0, sizeof(s_cdiag_after_stop));
    s_cdiag_start_result = 0xFFFFFFFFU;
    s_cdiag_stop_result = 0xFFFFFFFFU;
    s_cdiag_error_callbacks = 0U;
    s_cdiag_stop_errors = 0U;
#endif
    reset_context(ctx);
#if AUDIO_CAPTURE_D_ENABLE
    memset(dma_buf_d, 0, sizeof(dma_buf_d));
    memset(s_rawd_first, 0, sizeof(s_rawd_first));
    s_rawd_saved = 0U;
    block_d_half_count = 0U;
    block_d_full_count = 0U;
    s_ddiag_error_callbacks = 0U;
    s_ddiag_start_result = 0xFFFFFFFFU;
    s_ddiag_stop_result = 0xFFFFFFFFU;
#endif
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

#if AUDIO_CAPTURE_C_ENABLE
    /* C depende de SAI2; armarlo antes del maestro de BCLK/WS. */
    const HAL_StatusTypeDef start_c = HAL_SAI_Receive_DMA(
            &hsai_BlockB1,
            (uint8_t *)dma_buf_c,
            AUDIO_DMA_BUFFER_SIZE);
    s_cdiag_start_result = (uint32_t)start_c;
    audio_cdiag_take(&s_cdiag_arm);
    if (start_c != HAL_OK)
    {
        ctx->c_error_count++;
        return -4;
    }
    ctx->c_dma_active = 1U;
#endif

#if AUDIO_CAPTURE_D_ENABLE
    /* D (SAI1_A) tambien debe quedar armado antes del maestro SAI2_A. */
    const HAL_StatusTypeDef start_d = HAL_SAI_Receive_DMA(
        &hsai_BlockA1, (uint8_t *)dma_buf_d, AUDIO_DMA_BUFFER_SIZE);
    s_ddiag_start_result = (uint32_t)start_d;
    if (start_d != HAL_OK)
    {
        ctx->d_error_count++;
        ctx->d_error_code |= HAL_SAI_GetError(&hsai_BlockA1);
#if AUDIO_CAPTURE_C_ENABLE
        (void)HAL_SAI_DMAStop(&hsai_BlockB1);
        ctx->c_dma_active = 0U;
#endif
        return -5;
    }
    ctx->d_dma_active = 1U;
#endif

    /* B es esclavo: se arma antes de que A genere SCK/FS. */
    if (HAL_SAI_Receive_DMA(
            &hsai_BlockB2,
            (uint8_t *)dma_buf_b,
            AUDIO_DMA_BUFFER_SIZE) != HAL_OK)
    {
#if AUDIO_CAPTURE_C_ENABLE
        (void)HAL_SAI_DMAStop(&hsai_BlockB1);
        ctx->c_dma_active = 0U;
#endif
#if AUDIO_CAPTURE_D_ENABLE
        (void)HAL_SAI_DMAStop(&hsai_BlockA1);
        ctx->d_dma_active = 0U;
#endif
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
#if AUDIO_CAPTURE_C_ENABLE
        (void)HAL_SAI_DMAStop(&hsai_BlockB1);
        ctx->c_dma_active = 0U;
#endif
#if AUDIO_CAPTURE_D_ENABLE
        (void)HAL_SAI_DMAStop(&hsai_BlockA1);
        ctx->d_dma_active = 0U;
#endif
        ctx->error_count++;
        return -3;
    }

#if AUDIO_CAPTURE_C_ENABLE
    audio_cdiag_take(&s_cdiag_start);
#endif
    return 0;
}

int audio_capture_stop(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return -1;
    }

#if AUDIO_CAPTURE_C_ENABLE
    /* Snapshot antes de detener A/B: conserva el estado operativo de C. */
    audio_cdiag_take(&s_cdiag_stop);
#endif
    const HAL_StatusTypeDef result_a = HAL_SAI_DMAStop(&hsai_BlockA2);
    const HAL_StatusTypeDef result_b = HAL_SAI_DMAStop(&hsai_BlockB2);
#if AUDIO_CAPTURE_C_ENABLE
    HAL_StatusTypeDef result_c = HAL_OK;
    if (ctx->c_dma_active != 0U)
    {
        result_c = HAL_SAI_DMAStop(&hsai_BlockB1);
        ctx->c_dma_active = 0U;
        if (result_c != HAL_OK)
        {
            ctx->c_error_count++;
            s_cdiag_stop_errors++;
        }
    }
    s_cdiag_stop_result = (uint32_t)result_c;
    audio_cdiag_take(&s_cdiag_after_stop);
    audio_cdiag_emit();
    rawc_emit_after_stop(ctx);
#endif
#if AUDIO_CAPTURE_D_ENABLE
    HAL_StatusTypeDef result_d = HAL_OK;
    if (ctx->d_dma_active != 0U)
    {
        result_d = HAL_SAI_DMAStop(&hsai_BlockA1);
        ctx->d_dma_active = 0U;
        if (result_d != HAL_OK)
        {
            ctx->d_error_count++;
            ctx->d_error_code |= HAL_SAI_GetError(&hsai_BlockA1);
        }
    }
    s_ddiag_stop_result = (uint32_t)result_d;
    audio_ddiag_emit_after_stop(ctx);
#endif

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
#if AUDIO_CAPTURE_C_ENABLE
    stats->c_half_count = block_c_half_count;
    stats->c_full_count = block_c_full_count;
    stats->c_errors = ctx->c_error_count;
    stats->c_error_code = ctx->c_error_code;
    stats->c_dma_active = (ctx->c_dma_active != 0U);
#else
    stats->c_half_count = 0U;
    stats->c_full_count = 0U;
    stats->c_errors = 0U;
    stats->c_error_code = 0U;
    stats->c_dma_active = false;
#endif
#if AUDIO_CAPTURE_D_ENABLE
    stats->d_half_count = block_d_half_count;
    stats->d_full_count = block_d_full_count;
    stats->d_errors = ctx->d_error_count;
    stats->d_error_code = ctx->d_error_code;
    stats->d_dma_active = (ctx->d_dma_active != 0U);
#else
    stats->d_half_count = 0U;
    stats->d_full_count = 0U;
    stats->d_errors = 0U;
    stats->d_error_code = 0U;
    stats->d_dma_active = false;
#endif
    stats->fatal_error = (ctx->fatal_error != 0U);
}

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai == &hsai_BlockA2)
    {
        sai_half_count++;
        handle_dma_event_from_isr(&g_audio_ctx, true, 0U);
    }
    else if (hsai == &hsai_BlockB2)
    {
        sai_half_count++;
        block_b_half_count++;
        handle_dma_event_from_isr(&g_audio_ctx, false, 0U);
    }
#if AUDIO_CAPTURE_C_ENABLE
    else if (hsai == &hsai_BlockB1)
    {
        block_c_half_count++;
        g_audio_ctx.dma_c_generation[0]++;
        rawc_snapshot_from_isr(0U);
    }
#endif
#if AUDIO_CAPTURE_D_ENABLE
    else if (hsai == &hsai_BlockA1)
    {
        block_d_half_count++;
        g_audio_ctx.dma_d_generation[0]++;
        rawd_snapshot_from_isr(0U);
    }
#endif
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai == &hsai_BlockA2)
    {
        sai_full_count++;
        handle_dma_event_from_isr(&g_audio_ctx, true, 1U);
    }
    else if (hsai == &hsai_BlockB2)
    {
        sai_full_count++;
        block_b_full_count++;
        handle_dma_event_from_isr(&g_audio_ctx, false, 1U);
    }
#if AUDIO_CAPTURE_C_ENABLE
    else if (hsai == &hsai_BlockB1)
    {
        block_c_full_count++;
        g_audio_ctx.dma_c_generation[1]++;
    }
#endif
#if AUDIO_CAPTURE_D_ENABLE
    else if (hsai == &hsai_BlockA1)
    {
        block_d_full_count++;
        g_audio_ctx.dma_d_generation[1]++;
    }
#endif
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
#if AUDIO_CAPTURE_D_ENABLE
    if (hsai == &hsai_BlockA1)
    {
        /* Fallo D no detiene ni altera la ruta A/B ni la diagnostica C. */
        g_audio_ctx.d_error_count++;
        s_ddiag_error_callbacks++;
        g_audio_ctx.d_error_code |= HAL_SAI_GetError(hsai);
        return;
    }
#endif
#if AUDIO_CAPTURE_C_ENABLE
    if (hsai == &hsai_BlockB1)
    {
        /* Diagnostico C separado: no derribar A/B por un fallo de C. */
        g_audio_ctx.c_error_count++;
        s_cdiag_error_callbacks++;
        g_audio_ctx.c_error_code |= HAL_SAI_GetError(hsai);
        return;
    }
#endif
    /* Comportamiento estable de SAI2 A/B sin cambios. */
    g_audio_ctx.fatal_error = 1U;
    g_audio_ctx.error_count++;
}
