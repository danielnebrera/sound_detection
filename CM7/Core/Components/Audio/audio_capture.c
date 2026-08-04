/**
 * @file audio_capture.c
 *
 * Correcciones principales:
 *
 * 1. Cada mitad DMA contiene realmente 512 muestras por slot.
 *    El buffer DMA completo tiene 2048 palabras:
 *       512 muestras x 2 slots x 2 mitades.
 *
 * 2. Solo se desentrelaza cuando SAI2_A y SAI2_B terminaron
 *    la MISMA mitad, evitando mezclar audio nuevo con audio antiguo.
 *
 * 3. El diagnostico RAW usa la misma conversion PCM24 firmada
 *    que usa drone_detection.
 */

#include "audio_capture.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

__attribute__((section(".RAM_D2_bss")))
uint32_t dma_buf_a[AUDIO_DMA_BUFFER_SIZE];

__attribute__((section(".RAM_D2_bss")))
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
    AUDIO_DMA_BUFFER_SIZE ==
        (AUDIO_BUFFER_SIZE * 4U),
    "AUDIO_DMA_BUFFER_SIZE debe ser AUDIO_BUFFER_SIZE * 4"
);
#endif

static inline float sai24_right_to_float(uint32_t word)
{
    int32_t sample = (int32_t)(word & 0x00FFFFFFU);

    if ((sample & 0x00800000L) != 0)
    {
        sample |= (int32_t)0xFF000000U;
    }

    return (float)sample / 8388608.0f;
}

static inline float sai24_left_to_float(uint32_t word)
{
    const int32_t sample = ((int32_t)word) >> 8;
    return (float)sample / 8388608.0f;
}

int audio_capture_init(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return -1;
    }

    memset(dma_buf_a, 0, sizeof(dma_buf_a));
    memset(dma_buf_b, 0, sizeof(dma_buf_b));

    memset(ctx->ch0, 0, sizeof(ctx->ch0));
    memset(ctx->ch1, 0, sizeof(ctx->ch1));
    memset(ctx->ch2, 0, sizeof(ctx->ch2));
    memset(ctx->ch3, 0, sizeof(ctx->ch3));

    ctx->dma_a_half = 0U;
    ctx->dma_a_full = 0U;
    ctx->dma_b_half = 0U;
    ctx->dma_b_full = 0U;

    ctx->dma_half_complete = 0U;
    ctx->dma_full_complete = 0U;

    ctx->buffer_state = AUDIO_BUFFER_EMPTY;
    ctx->discard_pairs = 0U;
    ctx->frame_count = 0U;
    ctx->error_count = 0U;

    return 0;
}

int audio_capture_start(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return -1;
    }

    /*
     * B es esclavo sincronizado con A.
     * Se arma primero para que este listo antes de que A genere SCK/FS.
     */
    if (HAL_SAI_Receive_DMA(
            &hsai_BlockB2,
            (uint8_t *)dma_buf_b,
            AUDIO_DMA_BUFFER_SIZE
        ) != HAL_OK)
    {
        ctx->error_count++;
        return -2;
    }

    HAL_Delay(5U);

    if (HAL_SAI_Receive_DMA(
            &hsai_BlockA2,
            (uint8_t *)dma_buf_a,
            AUDIO_DMA_BUFFER_SIZE
        ) != HAL_OK)
    {
        HAL_SAI_DMAStop(&hsai_BlockB2);
        ctx->error_count++;
        return -1;
    }

    return 0;
}

int audio_capture_stop(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return -1;
    }

    HAL_SAI_DMAStop(&hsai_BlockA2);
    HAL_SAI_DMAStop(&hsai_BlockB2);

    return 0;
}

void audio_capture_discard_pending(AudioCaptureContext *ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    const uint32_t primask = __get_PRIMASK();

    __disable_irq();

    ctx->dma_a_half = 0U;
    ctx->dma_a_full = 0U;
    ctx->dma_b_half = 0U;
    ctx->dma_b_full = 0U;
    ctx->dma_half_complete = 0U;
    ctx->dma_full_complete = 0U;
    ctx->buffer_state = AUDIO_BUFFER_EMPTY;
    ctx->discard_pairs = 2U;

    if (primask == 0U)
    {
        __enable_irq();
    }
}

void audio_capture_deinterleave(
    AudioCaptureContext *ctx,
    uint8_t half
)
{
    if (ctx == NULL)
    {
        return;
    }

    /*
     * Primera mitad:  palabras [0 .. 1023]
     * Segunda mitad: palabras [1024 .. 2047]
     *
     * Cada mitad contiene:
     *   512 tramas estereo x 2 palabras.
     */
    const uint32_t offset =
        (half == 0U)
            ? 0U
            : (AUDIO_DMA_BUFFER_SIZE / 2U);

    /* ---------------- DIAGNOSTICO DE ALINEACION ---------------- */

    static uint32_t debug_counter = 0U;

    debug_counter++;

    if (debug_counter >= 86U)
    {
        debug_counter = 0U;

        float energy_left[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float energy_right[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        uint32_t lsb_zero[4] = {0U, 0U, 0U, 0U};
        uint32_t msb_zero_or_ff[4] = {0U, 0U, 0U, 0U};

        for (uint32_t i = 0U; i < 64U; i++)
        {
            const uint32_t index = offset + i * AUDIO_SLOTS_PER_FRAME;
            const uint32_t words[4] =
            {
                dma_buf_b[index],
                dma_buf_b[index + 1U],
                dma_buf_a[index],
                dma_buf_a[index + 1U]
            };

            for (uint32_t slot = 0U; slot < 4U; slot++)
            {
                const float left = sai24_left_to_float(words[slot]);
                const float right = sai24_right_to_float(words[slot]);
                const uint8_t lsb = (uint8_t)(words[slot] & 0xFFU);
                const uint8_t msb = (uint8_t)((words[slot] >> 24U) & 0xFFU);

                energy_left[slot] += left * left;
                energy_right[slot] += right * right;

                if (lsb == 0U)
                {
                    lsb_zero[slot]++;
                }

                if ((msb == 0x00U) || (msb == 0xFFU))
                {
                    msb_zero_or_ff[slot]++;
                }
            }
        }

        float db_left[4];
        float db_right[4];

        for (uint32_t slot = 0U; slot < 4U; slot++)
        {
            db_left[slot] =
                (energy_left[slot] > 1.0e-12f)
                    ? 20.0f * log10f(sqrtf(energy_left[slot] / 64.0f))
                    : -120.0f;

            db_right[slot] =
                (energy_right[slot] > 1.0e-12f)
                    ? 20.0f * log10f(sqrtf(energy_right[slot] / 64.0f))
                    : -120.0f;
        }

        printf(
            "[ALIGN-L] B_par:%5.1f B_imp:%5.1f A_par:%5.1f A_imp:%5.1f dBFS\r\n",
            (double)db_left[0],
            (double)db_left[1],
            (double)db_left[2],
            (double)db_left[3]
        );

        printf(
            "[ALIGN-R] B_par:%5.1f B_imp:%5.1f A_par:%5.1f A_imp:%5.1f dBFS\r\n",
            (double)db_right[0],
            (double)db_right[1],
            (double)db_right[2],
            (double)db_right[3]
        );

        printf(
            "[BYTEPOS] LSB00=[%lu %lu %lu %lu]/64 MSB00FF=[%lu %lu %lu %lu]/64\r\n",
            (unsigned long)lsb_zero[0],
            (unsigned long)lsb_zero[1],
            (unsigned long)lsb_zero[2],
            (unsigned long)lsb_zero[3],
            (unsigned long)msb_zero_or_ff[0],
            (unsigned long)msb_zero_or_ff[1],
            (unsigned long)msb_zero_or_ff[2],
            (unsigned long)msb_zero_or_ff[3]
        );
    }

    /* ---------------- DESENTRELAZADO ---------------- */

    for (uint32_t i = 0U;
         i < AUDIO_BUFFER_SIZE;
         i++)
    {
        const uint32_t index =
            offset + i * AUDIO_SLOTS_PER_FRAME;

        ctx->ch0[i] =
            (int32_t)dma_buf_b[index + 1U];
        /* Mic2: SAI B, slot impar, SEL=VCC */

        ctx->ch1[i] =
            (int32_t)dma_buf_a[index + 1U];
        /* Mic4: SAI A, slot impar, SEL=VCC */

        ctx->ch2[i] =
            (int32_t)dma_buf_a[index];
        /* Mic3: SAI A, slot par, SEL=GND */

        ctx->ch3[i] =
            (int32_t)dma_buf_b[index];
        /* Mic1: SAI B, slot par, SEL=GND */
    }
}

AudioBufferState audio_capture_get_data(
    AudioCaptureContext *ctx
)
{
    if (ctx == NULL)
    {
        return AUDIO_BUFFER_EMPTY;
    }

    bool half_ready = false;
    bool full_ready = false;

    /*
     * La comprobacion y limpieza de pares de flags se hace
     * dentro de una seccion critica muy corta.
     */
    const uint32_t primask = __get_PRIMASK();

    __disable_irq();

    if ((ctx->dma_a_half != 0U) &&
        (ctx->dma_b_half != 0U))
    {
        ctx->dma_a_half = 0U;
        ctx->dma_b_half = 0U;
        half_ready = true;
    }
    else if ((ctx->dma_a_full != 0U) &&
             (ctx->dma_b_full != 0U))
    {
        ctx->dma_a_full = 0U;
        ctx->dma_b_full = 0U;
        full_ready = true;
    }

    if (primask == 0U)
    {
        __enable_irq();
    }

    if ((half_ready || full_ready) &&
        (ctx->discard_pairs > 0U))
    {
        ctx->discard_pairs--;
        return AUDIO_BUFFER_EMPTY;
    }

    if (half_ready)
    {
        audio_capture_deinterleave(ctx, 0U);

        ctx->buffer_state = AUDIO_BUFFER_HALF;
        ctx->frame_count++;

        return AUDIO_BUFFER_HALF;
    }

    if (full_ready)
    {
        audio_capture_deinterleave(ctx, 1U);

        ctx->buffer_state = AUDIO_BUFFER_FULL;
        ctx->frame_count++;

        return AUDIO_BUFFER_FULL;
    }

    return AUDIO_BUFFER_EMPTY;
}

int32_t *audio_capture_get_channel(
    AudioCaptureContext *ctx,
    uint8_t channel
)
{
    if ((ctx == NULL) ||
        (channel >= AUDIO_CHANNELS))
    {
        return NULL;
    }

    switch (channel)
    {
        case 0U:
            return ctx->ch0; /* Mic2 */

        case 1U:
            return ctx->ch1; /* Mic4 */

        case 2U:
            return ctx->ch2; /* Mic3 */

        case 3U:
            return ctx->ch3; /* Mic1 */

        default:
            return NULL;
    }
}

void audio_capture_get_stats(
    AudioCaptureContext *ctx,
    uint32_t *frames_processed,
    uint32_t *errors
)
{
    if (ctx == NULL)
    {
        return;
    }

    if (frames_processed != NULL)
    {
        *frames_processed = ctx->frame_count;
    }

    if (errors != NULL)
    {
        *errors = ctx->error_count;
    }
}

void HAL_SAI_RxHalfCpltCallback(
    SAI_HandleTypeDef *hsai
)
{
    sai_half_count++;

    if (hsai == &hsai_BlockA2)
    {
        g_audio_ctx.dma_a_half = 1U;
    }
    else if (hsai == &hsai_BlockB2)
    {
        g_audio_ctx.dma_b_half = 1U;
        block_b_half_count++;
    }
}

void HAL_SAI_RxCpltCallback(
    SAI_HandleTypeDef *hsai
)
{
    sai_full_count++;

    if (hsai == &hsai_BlockA2)
    {
        g_audio_ctx.dma_a_full = 1U;
    }
    else if (hsai == &hsai_BlockB2)
    {
        g_audio_ctx.dma_b_full = 1U;
        block_b_full_count++;
    }
}

void HAL_SAI_ErrorCallback(
    SAI_HandleTypeDef *hsai
)
{
    (void)hsai;
    g_audio_ctx.error_count++;
}
