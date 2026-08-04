/* =================================================================
 * mfcc_stm32.c — con función de diagnóstico etapas intermedias
 * Sin cambios al pipeline principal mfcc_stm32_compute()
 * Nueva función: mfcc_stm32_export_frame()
 * ================================================================= */

#include "mfcc_stm32.h"
#include "mfcc_matrices_44k_sparse.h"
#include "arm_math.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "main.h"  /* HAL_Delay */

#define SR            MFCC_SAMPLE_RATE
#define N_FFT         MFCC_N_FFT
#define HOP           MFCC_HOP
#define N_MELS        MFCC_N_MELS
#define N_MFCC        MFCC_N_MFCC
#define N_FFT_BINS    (N_FFT / 2 + 1)
#define TARGET_FRAMES MFCC_TARGET_FRAMES

/* Buffers FFT separados - arm_rfft_fast_f32 requiere src != dst. */
__attribute__((section(".RAM_D2_dsp"), aligned(32)))
static float s_fft_in[N_FFT];

__attribute__((section(".RAM_D2_dsp"), aligned(32)))
static float s_fft_out[2 * N_FFT];

/* DSP scratch in DTCM keeps large temporary arrays off the stack. */
__attribute__((section(".DTCM_dsp"), aligned(32)))
static float s_hann[N_FFT];

__attribute__((section(".DTCM_dsp"), aligned(32)))
static float s_power[N_FFT_BINS];

__attribute__((section(".DTCM_dsp"), aligned(32)))
static float s_mel_a[N_MELS];

__attribute__((section(".DTCM_dsp"), aligned(32)))
static float s_mel_b[N_MELS];

__attribute__((section(".DTCM_dsp"), aligned(32)))
static float s_mfcc_frame[N_MFCC];

static arm_rfft_fast_instance_f32 s_rfft;
static bool s_inited = false;

static inline float reflect_at(const float *x, int n, int idx)
{
    while (idx < 0 || idx >= n) {
        if (idx < 0)  idx = -idx - 1;
        if (idx >= n) idx = 2 * n - idx - 1;
    }
    return x[idx];
}

bool mfcc_stm32_init(void)
{
    if (s_inited) return true;

    memset(s_fft_in, 0, sizeof(s_fft_in));
    memset(s_fft_out, 0, sizeof(s_fft_out));
    memset(s_power, 0, sizeof(s_power));
    memset(s_mel_a, 0, sizeof(s_mel_a));
    memset(s_mel_b, 0, sizeof(s_mel_b));
    memset(s_mfcc_frame, 0, sizeof(s_mfcc_frame));

    for (int i = 0; i < N_FFT; i++)
        s_hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (N_FFT - 1)));

    arm_status status = arm_rfft_fast_init_f32(&s_rfft, N_FFT);
    if (status != ARM_MATH_SUCCESS) {
        printf("[MFCC] ERROR: arm_rfft_fast_init_f32 fallo (status=%d)\r\n", (int)status);
        return false;
    }

    s_inited = true;
    printf("[MFCC] Init OK (N_FFT=%d, N_MELS=%d, N_MFCC=%d)\r\n", N_FFT, N_MELS, N_MFCC);
    return true;
}

void mfcc_stm32_apply_dsp(float *samples, int n)
{
    float prev_in  = 0.0f;
    float prev_out = 0.0f;
    for (int i = 0; i < n; i++) {
        float in    = samples[i];
        float hpf   = 0.9f * (prev_out + in - prev_in);
        prev_in     = in;
        prev_out    = hpf;
        samples[i]  = tanhf(hpf * 15.0f);
    }
}

static bool mfcc_compute_internal(const float *pcm, int n_in, mfcc_100x20_t *out)
{
    const int pad = N_FFT / 2;

    int n_frames = 1 + (n_in + 2 * pad - N_FFT) / HOP;
    if (n_frames > TARGET_FRAMES) n_frames = TARGET_FRAMES;

    memset(out->data, 0, sizeof(out->data));

    for (int f = 0; f < n_frames; f++)
    {
        const int start = f * HOP - pad;

        /* Ventana → buffer de entrada separado */
        for (int i = 0; i < N_FFT; i++)
            s_fft_in[i] = reflect_at(pcm, n_in, start + i) * s_hann[i];

        /* FFT con buffers separados */
        arm_rfft_fast_f32(&s_rfft, s_fft_in, s_fft_out, 0);

        s_power[0] = s_fft_out[0] * s_fft_out[0];
        for (int k = 1; k < N_FFT / 2; k++) {
            float re = s_fft_out[2 * k];
            float im = s_fft_out[2 * k + 1];
            s_power[k] = re * re + im * im;
        }
        s_power[N_FFT / 2] = s_fft_out[1] * s_fft_out[1];

        for (int m = 0; m < N_MELS; m++) {
            uint32_t bin_start = k_mel_start[m];
            uint32_t length    = k_mel_length[m];
            uint32_t offset    = k_mel_offset[m];
            float acc = 0.0f;
            for (uint32_t j = 0; j < length; j++)
                acc += s_power[bin_start + j] * k_mel_values[offset + j];
            s_mel_a[m] = acc;
        }

        for (int m = 0; m < N_MELS; m++) {
            float val = s_mel_a[m] < 1e-10f ? 1e-10f : s_mel_a[m];
            float db  = 10.0f * log10f(val);
            s_mel_a[m] = db < -80.0f ? -80.0f : db;
        }

        float *dst = &out->data[f * N_MFCC];
        for (int c = 0; c < N_MFCC; c++) {
            const float *dct_row = &k_dct_matrix[c * N_MELS];
            float acc = 0.0f;
            for (int m = 0; m < N_MELS; m++)
                acc += dct_row[m] * s_mel_a[m];
            dst[c] = acc;
        }
    }

    return true;
}

bool mfcc_stm32_compute(const float *pcm_1s, mfcc_100x20_t *out)
{
    if (!s_inited) {
        printf("[MFCC] ERROR: no inicializado\r\n");
        return false;
    }
    return mfcc_compute_internal(pcm_1s, SR, out);
}

bool mfcc_stm32_compute_n(const float *pcm, int n_samples, mfcc_100x20_t *out)
{
    if (!s_inited) {
        printf("[MFCC] ERROR: no inicializado\r\n");
        return false;
    }
    return mfcc_compute_internal(pcm, n_samples, out);
}

/* =================================================================
 * mfcc_stm32_export_frame — FUNCIÓN DE DIAGNÓSTICO
 * Exporta por UART las etapas intermedias de un frame específico:
 *   [POWER_START]      → 1025 floats potencia FFT
 *   [MEL_ENERGY_START] → 64 floats energías Mel
 *   [MEL_DB_START]     → 64 floats Mel en dB
 *   [MFCC_F_START]     → 20 floats coeficientes MFCC
 * ================================================================= */
static void export_floats_uart(const char *tag, const float *arr, int n)
{
    printf("[%s_START] %d\r\n", tag, n);
    for (int i = 0; i < n; i++) {
        uint32_t u; memcpy(&u, &arr[i], 4);
        printf("%08lX\r\n", (unsigned long)u);
        if ((i % 200) == 199) HAL_Delay(2);
    }
    printf("[%s_END]\r\n", tag);
}

void mfcc_stm32_export_frame(const float *pcm_1s, int frame_idx)
{
    if (!s_inited) {
        printf("[MFCC_DIAG] ERROR: no inicializado\r\n");
        return;
    }

    const int n_in = SR;
    const int pad  = N_FFT / 2;
    const int start = frame_idx * HOP - pad;

    printf("[MFCC_DIAG] Exportando frame %d\r\n", frame_idx);

    /* ── Ventana → buffer de entrada separado ────────────────── */
    for (int i = 0; i < N_FFT; i++)
        s_fft_in[i] = reflect_at(pcm_1s, n_in, start + i) * s_hann[i];

    /* ── FFT CMSIS con buffers separados ─────────────────────── */
    arm_rfft_fast_f32(&s_rfft, s_fft_in, s_fft_out, 0);

    /* ── Potencia ─────────────────────────────────────────────── */
    s_power[0] = s_fft_out[0] * s_fft_out[0];
    for (int k = 1; k < N_FFT / 2; k++) {
        float re = s_fft_out[2 * k];
        float im = s_fft_out[2 * k + 1];
        s_power[k] = re * re + im * im;
    }
    s_power[N_FFT / 2] = s_fft_out[1] * s_fft_out[1];

    /* ── Banco Mel ────────────────────────────────────────────── */
    for (int m = 0; m < N_MELS; m++) {
        uint32_t bin_start = k_mel_start[m];
        uint32_t length    = k_mel_length[m];
        uint32_t offset    = k_mel_offset[m];
        float acc = 0.0f;
        for (uint32_t j = 0; j < length; j++)
            acc += s_power[bin_start + j] * k_mel_values[offset + j];
        s_mel_a[m] = acc;
    }

    /* ── Mel dB ───────────────────────────────────────────────── */
    for (int m = 0; m < N_MELS; m++) {
        float val = s_mel_a[m] < 1e-10f ? 1e-10f : s_mel_a[m];
        float db  = 10.0f * log10f(val);
        s_mel_b[m] = db < -80.0f ? -80.0f : db;
    }

    /* ── MFCC ─────────────────────────────────────────────────── */
    for (int c = 0; c < N_MFCC; c++) {
        const float *dct_row = &k_dct_matrix[c * N_MELS];
        float acc = 0.0f;
        for (int m = 0; m < N_MELS; m++)
            acc += dct_row[m] * s_mel_b[m];
        s_mfcc_frame[c] = acc;
    }

    /* ── Exportar por UART ────────────────────────────────────── */
    export_floats_uart("POWER",      s_power,      N_FFT_BINS);
    export_floats_uart("MEL_ENERGY", s_mel_a,      N_MELS);
    export_floats_uart("MEL_DB",     s_mel_b,      N_MELS);
    export_floats_uart("MFCC_F",     s_mfcc_frame, N_MFCC);

    printf("[MFCC_DIAG] Frame %d exportado\r\n", frame_idx);
}
