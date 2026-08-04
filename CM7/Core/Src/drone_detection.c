/* =================================================================
 * drone_detection.c
 *
 * 4 MICROFONOS + MEMORIA SEGURA + DETECCION CERCANA/LEJANA
 *
 * Mantiene el pipeline que funcionaba en el ESP32:
 *
 *   PCM16 raw compartido
 *   -> HPF continuo
 *   -> tanh(hpf * 15)
 *   -> eliminar media del segundo
 *   -> pre-enfasis 0.97
 *   -> dBFS
 *   -> MFCC/DCT validado
 *   -> modelo
 *
 * Memoria:
 *   - Ventana PCM16 compartida con audio_recorder
 *   - Un unico float[44100] de trabajo
 *   - Un unico tensor MFCC
 *   - Una unica instancia/arena TFLite
 *
 * Mapeo de audio_capture:
 *   ch0 -> Mic2
 *   ch1 -> Mic4
 *   ch2 -> Mic3
 *   ch3 -> Mic1
 *
 * Comportamiento:
 *   - Un golpe aislado no activa la alarma.
 *   - Una señal fuerte y sostenida activa DRON CERCANO mediante EMA.
 *   - Una señal moderada sostenida 3 ventanas activa DRON LEJANO.
 *   - Al desaparecer la coherencia actual, el contador FAR vuelve a 0.
 * ================================================================= */

#include "drone_detection.h"
#include "mfcc_stm32.h"
#include "model_runner_stm32.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------
 * Audio y DSP
 * ----------------------------------------------------------------- */

#define DET_CHANNELS           4U
#define SAMPLES_PER_SECOND     44100U

#define HPF_ALPHA              0.90f
#define DSP_GAIN               15.0f
#define PREEMPHASIS            0.97f

/*
 * Gate relativo por microfono.
 *
 * Durante las primeras 3 ventanas, con el dron apagado, se mide el
 * nivel post-DSP de cada canal. Se usa la mediana de las 3 medidas y
 * luego la base queda CONGELADA hasta el siguiente reinicio.
 *
 * Un canal entra al modelo solo cuando:
 *
 *   dbfs_actual >= baseline_dbfs + margen_db
 *
 * De esta forma Mic4 puede usar su propio piso de ruido sin bajar el
 * gate global y sin dejar pasar el ruido constante de Mic1/Mic3.
 */
#define BASELINE_CAL_WINDOWS   3U

/* -----------------------------------------------------------------
 * EMA y deteccion
 * ----------------------------------------------------------------- */

#define ALPHA_RISE             0.60f
#define ALPHA_FALL             0.15f

#define THRESH_TRIGGER_FAST    0.85f
#define THRESH_INSTANT         0.60f

#define THRESH_SUSPICION       0.25f
#define THRESH_WEAK_PRIMARY    0.30f
#define THRESH_WEAK_SUPPORT    0.15f

#define TICKS_FOR_PERSISTENCE  3U

/*
 * Mantiene brevemente una alerta confirmada mientras la señal sigue
 * siendo valida. En silencio total se limpia inmediatamente.
 */
#define ALERT_HOLD_TICKS       3U

/* -----------------------------------------------------------------
 * Canales internos
 * ----------------------------------------------------------------- */

enum
{
    CHANNEL_MIC2 = 0,
    CHANNEL_MIC4 = 1,
    CHANNEL_MIC3 = 2,
    CHANNEL_MIC1 = 3
};

/*
 * Margen relativo inicial por canal, en dB.
 *
 * Mic2/Mic4: 5 dB para conservar robustez.
 * Mic1/Mic3: 4 dB para observar si realmente reaccionan al sonido
 * sin abrirles el paso por su nivel absoluto de ruido.
 *
 * Orden interno: Mic2, Mic4, Mic3, Mic1.
 */
static const float s_activity_margin_db[DET_CHANNELS] =
{
    5.0f, 5.0f, 4.0f, 4.0f
};

/* -----------------------------------------------------------------
 * Shared-window DSP buffers
 * ----------------------------------------------------------------- */

/* One float work buffer reused sequentially for all four microphones. */
__attribute__((section(".RAM_D1_work"), aligned(32)))
static float s_work_buffer[SAMPLES_PER_SECOND];

/* One MFCC tensor reused sequentially for all four microphones. */
__attribute__((section(".DTCM_dsp"), aligned(32)))
static mfcc_100x20_t s_mfcc;

/* HPF continuo por microfono */
static float s_hpf_previous_input[DET_CHANNELS] =
{
    0.0f, 0.0f, 0.0f, 0.0f
};

static float s_hpf_previous_output[DET_CHANNELS] =
{
    0.0f, 0.0f, 0.0f, 0.0f
};

/* -----------------------------------------------------------------
 * Resultados
 * ----------------------------------------------------------------- */

static float s_probability[DET_CHANNELS] =
{
    0.0f, 0.0f, 0.0f, 0.0f
};

static float s_dbfs[DET_CHANNELS] =
{
    -120.0f, -120.0f, -120.0f, -120.0f
};

static bool s_valid[DET_CHANNELS] =
{
    false, false, false, false
};

/* Gate relativo post-DSP, congelado tras la calibracion inicial. */
static float s_baseline_dbfs[DET_CHANNELS] =
{
    -120.0f, -120.0f, -120.0f, -120.0f
};

static float s_delta_dbfs[DET_CHANNELS] =
{
    0.0f, 0.0f, 0.0f, 0.0f
};

static bool s_acoustic_active[DET_CHANNELS] =
{
    false, false, false, false
};

static float s_calibration_dbfs
    [DET_CHANNELS][BASELINE_CAL_WINDOWS];

static uint32_t s_calibration_count = 0U;
static bool s_baseline_ready = false;

static float s_ema = 0.0f;
static uint8_t s_far_streak = 0U;
static uint8_t s_alert_hold = 0U;

/* -----------------------------------------------------------------
 * Load one recorded PCM16 channel into the shared float work buffer.
 * The HPF state remains independent and continuous for each microphone.
 * ----------------------------------------------------------------- */

static void load_pcm16_channel_into_work_buffer(
    uint32_t channel,
    const int16_t *source)
{
    if ((channel >= DET_CHANNELS) || (source == NULL))
    {
        return;
    }

    for (uint32_t i = 0U; i < SAMPLES_PER_SECOND; i++)
    {
        const float normalized =
            (float)source[i] / 32768.0f;

        const float hpf =
            HPF_ALPHA *
            (
                s_hpf_previous_output[channel] +
                normalized -
                s_hpf_previous_input[channel]
            );

        s_hpf_previous_input[channel] = normalized;
        s_hpf_previous_output[channel] = hpf;

        s_work_buffer[i] = tanhf(hpf * DSP_GAIN);
    }
}

/* -----------------------------------------------------------------
 * dBFS
 * ----------------------------------------------------------------- */

static float compute_dbfs(const float *buffer, uint32_t count)
{
    double sum_squares = 0.0;

    for (uint32_t i = 0U; i < count; i++)
    {
        const double sample = (double)buffer[i];
        sum_squares += sample * sample;
    }

    const double rms =
        sqrt(sum_squares / (double)count);

    if (rms <= 1.0e-12)
    {
        return -120.0f;
    }

    return (float)(20.0 * log10(rms));
}

/* -----------------------------------------------------------------
 * Preprocesamiento EXACTO de la version funcional
 * ----------------------------------------------------------------- */

static void prepare_work_buffer_for_model(void)
{
    /*
     * 1. Eliminar media del segundo.
     */
    double sum = 0.0;

    for (uint32_t i = 0U; i < SAMPLES_PER_SECOND; i++)
    {
        sum += (double)s_work_buffer[i];
    }

    const float mean =
        (float)(sum / (double)SAMPLES_PER_SECOND);

    for (uint32_t i = 0U; i < SAMPLES_PER_SECOND; i++)
    {
        s_work_buffer[i] -= mean;
    }

    /*
     * 2. Pre-enfasis, igual que en ESP32.
     *
     * Se recorre hacia atras para usar la muestra anterior original.
     */
    for (int32_t i = (int32_t)SAMPLES_PER_SECOND - 1;
         i >= 1;
         i--)
    {
        s_work_buffer[i] -=
            PREEMPHASIS * s_work_buffer[i - 1];
    }
}

/* -----------------------------------------------------------------
 * Inicializacion
 * ----------------------------------------------------------------- */

bool drone_detection_init(void)
{
    printf(
        "[DET] Inicializando 4 mics con baseline post-DSP congelado\r\n"
    );
    printf(
        "[DET] Mantener silencio durante las primeras %u ventanas\r\n",
        (unsigned)BASELINE_CAL_WINDOWS
    );

    memset(s_work_buffer, 0, sizeof(s_work_buffer));
    memset(&s_mfcc, 0, sizeof(s_mfcc));

    memset(
        s_hpf_previous_input,
        0,
        sizeof(s_hpf_previous_input)
    );

    memset(
        s_hpf_previous_output,
        0,
        sizeof(s_hpf_previous_output)
    );

    memset(s_probability, 0, sizeof(s_probability));
    memset(s_valid, 0, sizeof(s_valid));
    memset(s_delta_dbfs, 0, sizeof(s_delta_dbfs));
    memset(s_acoustic_active, 0, sizeof(s_acoustic_active));
    memset(s_calibration_dbfs, 0, sizeof(s_calibration_dbfs));

    for (uint32_t channel = 0U;
         channel < DET_CHANNELS;
         channel++)
    {
        s_dbfs[channel] = -120.0f;
        s_baseline_dbfs[channel] = -120.0f;
    }

    s_calibration_count = 0U;
    s_baseline_ready = false;

    s_ema = 0.0f;
    s_far_streak = 0U;
    s_alert_hold = 0U;

    if (!mfcc_stm32_init())
    {
        printf("[DET] ERROR: mfcc_stm32_init fallo\r\n");
        return false;
    }

    if (!model_runner_init())
    {
        printf("[DET] ERROR: model_runner_init fallo\r\n");
        return false;
    }

    printf(
        "[DET] Sistema listo: ventana PCM16 compartida + 1 float, FAR=%u, "
        "margenes=[M1 %.1f M2 %.1f M3 %.1f M4 %.1f] dB\r\n",
        (unsigned)TICKS_FOR_PERSISTENCE,
        (double)s_activity_margin_db[CHANNEL_MIC1],
        (double)s_activity_margin_db[CHANNEL_MIC2],
        (double)s_activity_margin_db[CHANNEL_MIC3],
        (double)s_activity_margin_db[CHANNEL_MIC4]
    );

    return true;
}

/* -----------------------------------------------------------------
 * Individual channel processing
 * ----------------------------------------------------------------- */

static float median_of_three(float a, float b, float c)
{
    if (a > b)
    {
        const float temp = a;
        a = b;
        b = temp;
    }

    if (b > c)
    {
        const float temp = b;
        b = c;
        c = temp;
    }

    if (a > b)
    {
        const float temp = a;
        a = b;
        b = temp;
    }

    return b;
}

static void process_channel(
    uint32_t channel,
    const int16_t *source)
{
    load_pcm16_channel_into_work_buffer(channel, source);

    /*
     * Pipeline identico a la version estable.
     */
    prepare_work_buffer_for_model();

    s_dbfs[channel] =
        compute_dbfs(
            s_work_buffer,
            SAMPLES_PER_SECOND
        );

    s_probability[channel] = 0.0f;
    s_valid[channel] = false;
    s_acoustic_active[channel] = false;
    s_delta_dbfs[channel] = 0.0f;

    /*
     * Durante la calibracion solo medimos el nivel post-DSP.
     * No se ejecuta MFCC ni modelo.
     */
    if (!s_baseline_ready)
    {
        return;
    }

    s_delta_dbfs[channel] =
        s_dbfs[channel] - s_baseline_dbfs[channel];

    if (s_delta_dbfs[channel] <
        s_activity_margin_db[channel])
    {
        return;
    }

    s_acoustic_active[channel] = true;

    if (!mfcc_stm32_compute(
            s_work_buffer,
            &s_mfcc
        ))
    {
        return;
    }

    s_valid[channel] = true;

    s_probability[channel] =
        model_runner_infer(
            s_mfcc.data
        );
}

static bool update_frozen_baseline(void)
{
    if (s_baseline_ready)
    {
        return true;
    }

    for (uint32_t channel = 0U;
         channel < DET_CHANNELS;
         channel++)
    {
        s_calibration_dbfs[channel][s_calibration_count] =
            s_dbfs[channel];
    }

    s_calibration_count++;

    printf(
        "[CAL] %lu/%u M1=%5.1f M2=%5.1f M3=%5.1f M4=%5.1f dBFS\r\n",
        (unsigned long)s_calibration_count,
        (unsigned)BASELINE_CAL_WINDOWS,
        (double)s_dbfs[CHANNEL_MIC1],
        (double)s_dbfs[CHANNEL_MIC2],
        (double)s_dbfs[CHANNEL_MIC3],
        (double)s_dbfs[CHANNEL_MIC4]
    );

    if (s_calibration_count < BASELINE_CAL_WINDOWS)
    {
        return false;
    }

    for (uint32_t channel = 0U;
         channel < DET_CHANNELS;
         channel++)
    {
        s_baseline_dbfs[channel] =
            median_of_three(
                s_calibration_dbfs[channel][0],
                s_calibration_dbfs[channel][1],
                s_calibration_dbfs[channel][2]
            );
    }

    s_baseline_ready = true;

    /*
     * La base queda congelada: no se vuelve a modificar.
     */
    printf(
        "[BASE] M1=%5.1f(+%.1f) M2=%5.1f(+%.1f) "
        "M3=%5.1f(+%.1f) M4=%5.1f(+%.1f) dBFS\r\n",
        (double)s_baseline_dbfs[CHANNEL_MIC1],
        (double)s_activity_margin_db[CHANNEL_MIC1],
        (double)s_baseline_dbfs[CHANNEL_MIC2],
        (double)s_activity_margin_db[CHANNEL_MIC2],
        (double)s_baseline_dbfs[CHANNEL_MIC3],
        (double)s_activity_margin_db[CHANNEL_MIC3],
        (double)s_baseline_dbfs[CHANNEL_MIC4],
        (double)s_activity_margin_db[CHANNEL_MIC4]
    );

    s_ema = 0.0f;
    s_far_streak = 0U;
    s_alert_hold = 0U;

    return false;
}

/* -----------------------------------------------------------------
 * Dos mejores probabilidades validas
 * ----------------------------------------------------------------- */

static void find_top_two_probabilities(
    float *highest,
    float *second,
    uint32_t *valid_count)
{
    *highest = 0.0f;
    *second = 0.0f;
    *valid_count = 0U;

    for (uint32_t channel = 0U;
         channel < DET_CHANNELS;
         channel++)
    {
        if (!s_valid[channel])
        {
            continue;
        }

        (*valid_count)++;

        const float probability =
            s_probability[channel];

        if (probability > *highest)
        {
            *second = *highest;
            *highest = probability;
        }
        else if (probability > *second)
        {
            *second = probability;
        }
    }
}

/* -----------------------------------------------------------------
 * Procesamiento global
 * ----------------------------------------------------------------- */

bool drone_detection_process_window(const RecorderChunkView *window)
{
    if ((window == NULL) ||
        (window->frame_count != SAMPLES_PER_SECOND))
    {
        printf("[DET] ERROR: ventana PCM16 invalida\r\n");
        return false;
    }

    for (uint32_t channel = 0U; channel < DET_CHANNELS; channel++)
    {
        if (window->channel[channel] == NULL)
        {
            printf("[DET] ERROR: canal %lu nulo\r\n",
                   (unsigned long)channel);
            return false;
        }
    }

    process_channel(CHANNEL_MIC2, window->channel[CHANNEL_MIC2]);
    process_channel(CHANNEL_MIC4, window->channel[CHANNEL_MIC4]);
    process_channel(CHANNEL_MIC3, window->channel[CHANNEL_MIC3]);
    process_channel(CHANNEL_MIC1, window->channel[CHANNEL_MIC1]);

    if (!update_frozen_baseline())
    {
        return true;
    }

    uint32_t activity_count = 0U;

    for (uint32_t channel = 0U;
         channel < DET_CHANNELS;
         channel++)
    {
        if (s_acoustic_active[channel])
        {
            activity_count++;
        }
    }

    float p_highest = 0.0f;
    float p_second = 0.0f;
    uint32_t valid_count = 0U;

    find_top_two_probabilities(
        &p_highest,
        &p_second,
        &valid_count
    );

    /*
     * Fusion:
     *
     * - Dos o mas canales: se usan los dos mejores.
     * - Un solo canal valido: se permite funcionar como el antiguo
     *   sistema de un microfono, pero necesita continuidad temporal.
     */
    float p_fused = 0.0f;

    if (valid_count >= 2U)
    {
        p_fused =
            0.65f * p_highest +
            0.35f * p_second;
    }
    else if (valid_count == 1U)
    {
        p_fused = p_highest;
    }

    /*
     * EMA asimetrica.
     */
    const float ema_alpha =
        (p_fused > s_ema)
            ? ALPHA_RISE
            : ALPHA_FALL;

    s_ema =
        (1.0f - ema_alpha) * s_ema +
        ema_alpha * p_fused;

    /*
     * Candidato fuerte:
     * puede proceder de un solo microfono, como en el ESP32,
     * pero la EMA evita que una unica ventana fuerte dispare.
     */
    const bool strong_candidate =
        (valid_count >= 1U) &&
        (p_highest >= 0.90f) &&
        (p_fused >= THRESH_INSTANT);

    const bool red_alert =
        strong_candidate &&
        (s_ema >= THRESH_TRIGGER_FAST);

    /*
     * Candidato debil/lejano:
     *
     * Con dos o mas mics validos se pide apoyo del segundo.
     * Con un solo mic valido se exige un p un poco mayor.
     */
    bool weak_candidate = false;

    if (valid_count >= 2U)
    {
        weak_candidate =
            (p_highest >= THRESH_WEAK_PRIMARY) &&
            (p_second >= THRESH_WEAK_SUPPORT) &&
            (p_fused >= THRESH_SUSPICION);
    }
    else if (valid_count == 1U)
    {
        weak_candidate =
            (p_highest >= 0.40f) &&
            (p_fused >= THRESH_SUSPICION);
    }

    /*
     * Persistencia CONSECUTIVA:
     * una ventana sin candidato rompe la secuencia.
     */
    if (weak_candidate)
    {
        if (s_far_streak < TICKS_FOR_PERSISTENCE)
        {
            s_far_streak++;
        }
    }
    else
    {
        s_far_streak = 0U;
    }

    const bool orange_alert =
        (s_far_streak >= TICKS_FOR_PERSISTENCE);

    /*
     * Sin ningun canal por encima del gate:
     * limpiar de inmediato, como en el ESP32.
     */
    if (valid_count == 0U)
    {
        s_far_streak = 0U;
        s_alert_hold = 0U;

        s_ema =
            s_ema * (1.0f - ALPHA_FALL);

        if (s_ema < 0.01f)
        {
            s_ema = 0.0f;
        }
    }

    /*
     * Mantener brevemente una alerta confirmada.
     * No se mantiene durante silencio total.
     */
    if (red_alert || orange_alert)
    {
        s_alert_hold = ALERT_HOLD_TICKS;
    }
    else if ((valid_count > 0U) &&
             (s_alert_hold > 0U))
    {
        s_alert_hold--;
    }

    const bool alert =
        red_alert ||
        orange_alert ||
        (s_alert_hold > 0U);

    const char *alert_text = "";

    if (red_alert)
    {
        alert_text = "*** DRON CERCANO ***";
    }
    else if (orange_alert)
    {
        alert_text = "*** DRON LEJANO ***";
    }
    else if (alert)
    {
        alert_text = "*** DRON MANTENIDO ***";
    }

    printf(
        "[DET4] "
        "M1:%5.1f b=%5.1f d=%4.1f A=%u p=%.2f | "
        "M2:%5.1f b=%5.1f d=%4.1f A=%u p=%.2f | "
        "M3:%5.1f b=%5.1f d=%4.1f A=%u p=%.2f | "
        "M4:%5.1f b=%5.1f d=%4.1f A=%u p=%.2f | "
        "ACT=%lu VALID=%lu TOP=[%.2f %.2f] "
        "FUSED=%.2f EMA=%.2f FAR=%u/%u %s\r\n",

        (double)s_dbfs[CHANNEL_MIC1],
        (double)s_baseline_dbfs[CHANNEL_MIC1],
        (double)s_delta_dbfs[CHANNEL_MIC1],
        (unsigned)s_acoustic_active[CHANNEL_MIC1],
        (double)s_probability[CHANNEL_MIC1],

        (double)s_dbfs[CHANNEL_MIC2],
        (double)s_baseline_dbfs[CHANNEL_MIC2],
        (double)s_delta_dbfs[CHANNEL_MIC2],
        (unsigned)s_acoustic_active[CHANNEL_MIC2],
        (double)s_probability[CHANNEL_MIC2],

        (double)s_dbfs[CHANNEL_MIC3],
        (double)s_baseline_dbfs[CHANNEL_MIC3],
        (double)s_delta_dbfs[CHANNEL_MIC3],
        (unsigned)s_acoustic_active[CHANNEL_MIC3],
        (double)s_probability[CHANNEL_MIC3],

        (double)s_dbfs[CHANNEL_MIC4],
        (double)s_baseline_dbfs[CHANNEL_MIC4],
        (double)s_delta_dbfs[CHANNEL_MIC4],
        (unsigned)s_acoustic_active[CHANNEL_MIC4],
        (double)s_probability[CHANNEL_MIC4],

        (unsigned long)activity_count,
        (unsigned long)valid_count,
        (double)p_highest,
        (double)p_second,
        (double)p_fused,
        (double)s_ema,

        (unsigned)s_far_streak,
        (unsigned)TICKS_FOR_PERSISTENCE,

        alert_text
    );

    return true;
}
