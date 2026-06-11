/* =================================================================
 * mfcc_stm32.c
 * MFCC compatible con Librosa para STM32H747 (Cortex-M7, 480 MHz)
 *
 * Algoritmo idéntico al ESP32 original. Cambios de plataforma:
 *   ESP32 dsps_fft2r_fc32  →  arm_rfft_fast_f32  (CMSIS-DSP)
 *   heap_caps_malloc       →  malloc estándar
 *   ESP_LOGE               →  printf (UART1 vía FTDI)
 * ================================================================= */

#include "mfcc_stm32.h"
#include "mfcc_matrices_44k.h"   /* k_mel_filter_bank[], k_dct_matrix[] */

/* CMSIS-DSP — ya incluido en el HAL del STM32H7 */
#include "arm_math.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Parámetros internos ──────────────────────────────────────── */
#define SR          MFCC_SAMPLE_RATE      /* 44100  */
#define N_FFT       MFCC_N_FFT            /* 2048   */
#define WIN_LEN     MFCC_N_FFT            /* 2048   */
#define HOP         MFCC_HOP              /* 442    */
#define N_MELS      MFCC_N_MELS           /* 64     */
#define N_MFCC      MFCC_N_MFCC           /* 20     */
#define N_FFT_BINS  (N_FFT / 2 + 1)       /* 1025   */
#define TARGET_FRAMES MFCC_TARGET_FRAMES  /* 100    */

/* ── Buffers estáticos en RAM_D2 (accesibles por DMA y CPU) ───── */
/* arm_rfft_fast_f32 necesita un buffer de N_FFT floats (entrada real)
   y devuelve N_FFT floats en formato packed: [R0, R1, I1, I2, ..., R_N/2]
   Usamos 2×N_FFT por compatibilidad con el layout del ESP32.         */
__attribute__((section(".RAM_D2_bss")))
static float s_fft_buf[2 * N_FFT];   /* 16 KB en RAM_D2 */

/* Ventana Hann (2048 floats = 8 KB) — RAM normal */
static float s_hann[N_FFT];

/* Instancia RFFT de CMSIS-DSP */
static arm_rfft_fast_instance_f32 s_rfft;

/* Flag de inicialización */
static bool s_inited = false;

/* ── Función auxiliar: reflexión de borde (igual que ESP32) ───── */
static inline float reflect_at(const float *x, int n, int idx)
{
    while (idx < 0 || idx >= n) {
        if (idx < 0)   idx = -idx - 1;
        if (idx >= n)  idx = 2 * n - idx - 1;
    }
    return x[idx];
}

/* ── Inicialización ───────────────────────────────────────────── */
bool mfcc_stm32_init(void)
{
    if (s_inited) return true;

    /* 1. Ventana Hann (idéntica al ESP32) */
    for (int i = 0; i < N_FFT; i++) {
        s_hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (WIN_LEN - 1)));
    }

    /* 2. Inicializar RFFT de CMSIS-DSP para N_FFT=2048 puntos */
    arm_status status = arm_rfft_fast_init_f32(&s_rfft, N_FFT);
    if (status != ARM_MATH_SUCCESS) {
        printf("[MFCC] ERROR: arm_rfft_fast_init_f32 falló (status=%d)\r\n", (int)status);
        return false;
    }

    s_inited = true;
    printf("[MFCC] Init OK (N_FFT=%d, N_MELS=%d, N_MFCC=%d)\r\n", N_FFT, N_MELS, N_MFCC);
    return true;
}

/* ── Preprocesado DSP (replica audio_i2s.c del ESP32) ────────── */
void mfcc_stm32_apply_dsp(float *samples, int n)
{
    float prev_sample = 0.0f;
    float prev_output = 0.0f;

    for (int i = 0; i < n; i++) {
        float input = samples[i];

        /* Filtro paso alto: α=0.9, corta ~100-200 Hz */
        float hpf = 0.9f * (prev_output + input - prev_sample);
        prev_sample = input;
        prev_output = hpf;

        /* Ganancia ×15 + soft clipping tanh */
        samples[i] = tanhf(hpf * 15.0f);
    }
}

/* ── Cálculo MFCC principal ───────────────────────────────────── */
bool mfcc_stm32_compute(const float *pcm_1s, mfcc_100x20_t *out)
{
    if (!s_inited) {
        printf("[MFCC] ERROR: no inicializado\r\n");
        return false;
    }

    const int pad     = N_FFT / 2;
    const int n_in    = SR;          /* 44100 muestras */

    /* Número de frames (igual que librosa con center=True) */
    int n_frames = 1 + (n_in + 2 * pad - N_FFT) / HOP;
    if (n_frames > TARGET_FRAMES) n_frames = TARGET_FRAMES;

    /* Limpiar salida */
    memset(out->data, 0, sizeof(out->data));

    /* Buffer temporal de energías Mel (stack, 64 floats = 256 bytes) */
    float mel_vec[N_MELS];

    for (int f = 0; f < n_frames; f++)
    {
        const int start = f * HOP - pad;

        /* ── 1. Enventanado con Hann ────────────────────────── */
        /* arm_rfft_fast_f32 trabaja con señal REAL de N_FFT floats */
        float *rfft_in = s_fft_buf;          /* primera mitad del buffer */
        for (int i = 0; i < N_FFT; i++) {
            rfft_in[i] = reflect_at(pcm_1s, n_in, start + i) * s_hann[i];
        }

        /* ── 2. FFT real con CMSIS-DSP ──────────────────────── */
        /* Salida en s_fft_buf formato packed:
           [R0, R_N/2, R1, I1, R2, I2, ..., R_{N/2-1}, I_{N/2-1}] */
        float *rfft_out = s_fft_buf;
        arm_rfft_fast_f32(&s_rfft, rfft_in, rfft_out, 0 /* forward */);

        /* ── 3. Calcular potencia espectral (|X[k]|²) ──────── */
        /* Convertir el formato packed a magnitud² bin a bin      */
        /* Bin 0 (DC): s_fft_buf[0]², imaginaria = 0             */
        /* Bin N/2:    s_fft_buf[1]², imaginaria = 0             */
        /* Bins 1..N/2-1: R=s_fft_buf[2k], I=s_fft_buf[2k+1]   */

        /* Usamos un array temporal en stack para las potencias   */
        /* N_FFT_BINS = 1025, cada float = 4 bytes → 4.1 KB stack */
        float power[N_FFT_BINS];

        /* Bin DC */
        power[0] = rfft_out[0] * rfft_out[0];

        /* Bins intermedios */
        for (int k = 1; k < N_FFT / 2; k++) {
            float re = rfft_out[2 * k];
            float im = rfft_out[2 * k + 1];
            power[k] = re * re + im * im;
        }

        /* Bin de Nyquist */
        power[N_FFT / 2] = rfft_out[1] * rfft_out[1];

        /* Eliminar frecuencias <300 Hz (bins 0-6, igual que ESP32) */
        for (int k = 0; k < 7; k++) power[k] = 0.0f;

        /* ── 4. Energías Mel ────────────────────────────────── */
        for (int m = 0; m < N_MELS; m++) {
            const float *mel_row = &k_mel_filter_bank[m * N_FFT_BINS];
            float acc = 0.0f;
            for (int k = 0; k < N_FFT_BINS; k++) {
                acc += power[k] * mel_row[k];
            }
            mel_vec[m] = acc;
        }

        /* ── 5. Power to dB (igual que ESP32: ref=1.0, top_db=80) */
        const float AMIN   = 1e-10f;
        const float TOP_DB = 80.0f;
        for (int m = 0; m < N_MELS; m++) {
            float val = mel_vec[m];
            if (val < AMIN) val = AMIN;
            float db = 10.0f * log10f(val);
            if (db < -TOP_DB) db = -TOP_DB;
            mel_vec[m] = db;
        }

        /* ── 6. DCT-II ortho → N_MFCC coeficientes ─────────── */
        float *dst = &out->data[f * N_MFCC];
        for (int c = 0; c < N_MFCC; c++) {
            const float *dct_row = &k_dct_matrix[c * N_MELS];
            float acc = 0.0f;
            for (int m = 0; m < N_MELS; m++) {
                acc += dct_row[m] * mel_vec[m];
            }
            dst[c] = acc;
        }
    }

    return true;
}