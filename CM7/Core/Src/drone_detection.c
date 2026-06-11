/* =================================================================
 * drone_detection.c
 * Pipeline de detección de drones — 4 micrófonos — Portenta H7
 *
 * Para cada canal en cada segundo de audio:
 *   1. Normalizar int32 DMA → float [-1.0, 1.0]
 *   2. Eliminar DC offset
 *   3. Pre-énfasis (coef 0.97, igual que main.c ESP32)
 *   4. Calcular dBFS
 *   5. Gate de silencio (< SILENCE_DB → saltar)
 *   6. Aplicar DSP: HPF + ganancia×15 + tanh
 *   7. Calcular MFCC [100×20×1]
 *   8. Inferencia TFLite → p_drone [0.0-1.0]
 *
 * Combinar 4 canales → EMA asimétrica → ALERTA ROJA / NARANJA
 * ================================================================= */

#include "drone_detection.h"
#include "mfcc_stm32.h"
#include "model_runner_stm32.h"
#include "audio_capture.h"   /* audio_capture_get_channel(), AUDIO_BUFFER_SIZE */

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Configuración (idéntica al ESP32) ──────────────────────── */
#define ALPHA_RISE          0.60f   /* EMA subida rápida          */
#define ALPHA_FALL          0.15f   /* EMA bajada lenta           */
#define THRESH_TRIGGER_FAST 0.85f   /* Alerta ROJA inmediata      */
#define THRESH_SUSPICION    0.25f   /* Inicio rastreo             */
#define THRESH_INSTANT      0.60f   /* Confirmación doble         */
#define TICKS_PERSISTENCE   3       /* Ticks para alerta naranja  */
#define SILENCE_DB         -35.0f   /* Gate de silencio (dBFS)    */

/* ── Buffers PCM por canal (44100 floats = 172 KB × 4 = 688 KB)
   Ubicados en RAM_D2 para no agotar RAM_D1 del CM7.
   NOTA: Si la RAM_D2 es insuficiente, procesar un canal a la vez
         reutilizando el mismo buffer.                            */
#define SAMPLES_PER_SECOND  44100

/* Procesamos un canal a la vez reutilizando el buffer — ahorro de RAM */
// Por esto (RAM_D1, sin restricción DMA):
static float s_pcm[SAMPLES_PER_SECOND];   /* 172 KB en RAM_D2 */

/* Buffer MFCC de salida */
static mfcc_100x20_t s_mfcc;

/* ── Estado EMA y persistencia ───────────────────────────────── */
static float s_ema              = 0.0f;
static int   s_persistence      = 0;

/* ── Inicialización ──────────────────────────────────────────── */
bool drone_detection_init(void)
{
    printf("[DET] Inicializando deteccion de drones...\r\n");

    if (!mfcc_stm32_init()) {
        printf("[DET] ERROR: mfcc_stm32_init falló\r\n");
        return false;
    }

    if (!model_runner_init()) {
        printf("[DET] ERROR: model_runner_init falló\r\n");
        return false;
    }

    printf("[DET] Sistema listo — 4 microfonos activos\r\n");
    return true;
}

/* ── Helpers ─────────────────────────────────────────────────── */

/* Normaliza int32 (ICS-43434: datos en bits 31-8) a float [-1,1] */
static inline float int32_to_float(int32_t s)
{
    return (float)s / (float)0x7FFFFF00;
}

/* Calcula dBFS de un buffer de floats */
static float compute_dbfs(const float *buf, int n)
{
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
    float rms = sqrtf(sum / (float)n);
    return (rms > 1e-12f) ? 20.0f * log10f(rms) : -120.0f;
}

/* Construye el buffer PCM de 1 segundo desde un canal DMA.
   El DMA captura en AUDIO_BUFFER_SIZE bloques; aquí acumulamos
   tantos bloques como hagan falta para llenar 44100 muestras.
   Por simplicidad usamos directamente el puntero del canal
   que ya tiene AUDIO_BUFFER_SIZE muestras del último frame.
   Para 1 segundo real se necesita integrar el loop principal —
   esta función convierte lo que hay disponible y rellena con 0. */
static void build_pcm_channel(const int32_t *src, int src_len, float *dst)
{
    int copy = (src_len < SAMPLES_PER_SECOND) ? src_len : SAMPLES_PER_SECOND;
    for (int i = 0; i < copy; i++) {
        dst[i] = int32_to_float(src[i]);
    }
    /* Rellenar el resto con ceros si src_len < 44100 */
    if (copy < SAMPLES_PER_SECOND) {
        memset(&dst[copy], 0, (SAMPLES_PER_SECOND - copy) * sizeof(float));
    }
}

/* ── Pipeline principal ──────────────────────────────────────── */
void drone_detection_process(void)
{
    float p_channels[4] = {0};
    float db_channels[4] = {0};

    /* Obtener punteros a los 4 canales del audio_capture */
    extern AudioCaptureContext g_audio_ctx;  /* definido en main.c */
    int32_t *ch[4] = {
        audio_capture_get_channel(&g_audio_ctx, 0),  /* Mic1 */
        audio_capture_get_channel(&g_audio_ctx, 1),  /* Mic2 */
        audio_capture_get_channel(&g_audio_ctx, 2),  /* Mic3 */
        audio_capture_get_channel(&g_audio_ctx, 3)   /* Mic4 */
    };

    /* Procesar cada canal independientemente */
    for (int ch_idx = 0; ch_idx < 4; ch_idx++)
    {
        if (ch[ch_idx] == NULL) {
            p_channels[ch_idx] = 0.0f;
            db_channels[ch_idx] = -120.0f;
            continue;
        }

        /* 1. Convertir int32 → float */
        build_pcm_channel(ch[ch_idx], AUDIO_BUFFER_SIZE, s_pcm);

        /* 2. Eliminar DC offset */
        float mean = 0.0f;
        for (int i = 0; i < SAMPLES_PER_SECOND; i++) mean += s_pcm[i];
        mean /= SAMPLES_PER_SECOND;
        for (int i = 0; i < SAMPLES_PER_SECOND; i++) s_pcm[i] -= mean;

        /* 3. Pre-énfasis (coef 0.97, igual que ESP32) */
        for (int i = SAMPLES_PER_SECOND - 1; i >= 1; i--) {
            s_pcm[i] -= 0.97f * s_pcm[i - 1];
        }

        /* 4. Calcular dBFS */
        db_channels[ch_idx] = compute_dbfs(s_pcm, SAMPLES_PER_SECOND);

        /* 5. Gate de silencio */
        if (db_channels[ch_idx] < SILENCE_DB) {
            p_channels[ch_idx] = 0.0f;
            continue;
        }

        /* 6. Aplicar DSP del ESP32: HPF + ganancia + tanh */
        mfcc_stm32_apply_dsp(s_pcm, SAMPLES_PER_SECOND);


        printf("[DEBUG] PCM[0]=%.6f PCM[100]=%.6f PCM[500]=%.6f dBFS=%.1f\r\n",
               s_pcm[0], s_pcm[100], s_pcm[500], db_channels[ch_idx]);

        printf("[DEBUG] src_len=%d copy=%d\r\n", src_len,
               (src_len < SAMPLES_PER_SECOND) ? src_len : SAMPLES_PER_SECOND);

        /* 7. Calcular MFCC */
        if (!mfcc_stm32_compute(s_pcm, &s_mfcc)) {
            p_channels[ch_idx] = 0.0f;
            continue;
        }

        /* 8. Inferencia */
        p_channels[ch_idx] = model_runner_infer(s_mfcc.data);
    }

    /* ── Combinar 4 canales: promedio ponderado ──────────────── */
    float p_drone = 0.0f;
    int   valid   = 0;
    for (int i = 0; i < 4; i++) {
        if (p_channels[i] >= 0.0f) {
            p_drone += p_channels[i];
            valid++;
        }
    }
    if (valid > 0) p_drone /= (float)valid;

    /* ── EMA asimétrica (idéntica al ESP32) ──────────────────── */
    float alpha = (p_drone > s_ema) ? ALPHA_RISE : ALPHA_FALL;
    s_ema = s_ema * (1.0f - alpha) + p_drone * alpha;

    /* ── Sistema de alertas ──────────────────────────────────── */
    /* Imprimir estado de los 4 micrófonos */
    printf("Mic1:%5.0fdB p=%.2f | Mic2:%5.0fdB p=%.2f | "
           "Mic3:%5.0fdB p=%.2f | Mic4:%5.0fdB p=%.2f | "
           "EMA:%.2f",
           db_channels[0], p_channels[0],
           db_channels[1], p_channels[1],
           db_channels[2], p_channels[2],
           db_channels[3], p_channels[3],
           s_ema);

    /* CAMINO A: Alerta ROJA inmediata */
    if (s_ema >= THRESH_TRIGGER_FAST && p_drone > THRESH_INSTANT) {
        printf(" >>> ALERTA ROJA: DRON DETECTADO (%.0f%%)\r\n",
               s_ema * 100.0f);
        s_persistence = TICKS_PERSISTENCE;
    }
    /* CAMINO B: Rastreo dron lejano */
    else if (s_ema >= THRESH_SUSPICION) {
        s_persistence++;
        printf(" [RASTREANDO %d/%d]\r\n", s_persistence, TICKS_PERSISTENCE);

        if (s_persistence >= TICKS_PERSISTENCE) {
            printf(" >>> ALERTA NARANJA: DRON LEJANO CONFIRMADO\r\n");
            s_persistence = TICKS_PERSISTENCE;
        }
    }
    /* CAMINO C: Sin señal */
    else {
        if (s_persistence > 0) printf(" [señal perdida]\r\n");
        else printf("\r\n");
        s_persistence = 0;
    }
}
