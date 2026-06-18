/* =================================================================
 * drone_detection.c
 * Pipeline de deteccion de drones — 4 microfonos — Portenta H7
 *
 * Estrategia de memoria: 1 buffer de acumulacion (172 KB).
 * Se procesa 1 canal por ciclo de 1 segundo. Los 4 canales se
 * procesan en 4 segundos consecutivos. El EMA integra los 4.
 * ================================================================= */

#include "drone_detection.h"
#include "mfcc_stm32.h"
#include "model_runner_stm32.h"
#include "audio_capture.h"
#include "can_sender.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#define ALPHA_RISE          0.60f
#define ALPHA_FALL          0.15f
#define THRESH_TRIGGER_FAST 0.85f
#define THRESH_SUSPICION    0.25f
#define THRESH_INSTANT      0.60f
#define TICKS_PERSISTENCE   3
#define SILENCE_DB         -35.0f
#define SAMPLES_PER_SECOND  44100

/* Un solo buffer de acumulacion — 172 KB en RAM_D1 */
static float s_accum[SAMPLES_PER_SECOND];
static int   s_accum_idx   = 0;
static bool  s_accum_ready = false;
static int   s_current_ch  = 0;   /* canal que se acumula ahora (0-3) */

/* Buffer MFCC */
static mfcc_100x20_t s_mfcc;

/* Resultados de los 4 canales — se llenan de a 1 por segundo */
static float s_p_ch[4]  = {0, 0, 0, 0};
static float s_db_ch[4] = {-120, -120, -120, -120};

/* EMA y persistencia */
static float s_ema         = 0.0f;
static int   s_persistence = 0;

/* ── Inicializacion ──────────────────────────────────────────── */
bool drone_detection_init(void)
{
    printf("[DET] Inicializando deteccion de drones...\r\n");
    memset(s_accum, 0, sizeof(s_accum));
    s_accum_idx   = 0;
    s_accum_ready = false;
    s_current_ch  = 0;

    if (!mfcc_stm32_init()) {
        printf("[DET] ERROR: mfcc_stm32_init fallo\r\n");
        return false;
    }
    if (!model_runner_init()) {
        printf("[DET] ERROR: model_runner_init fallo\r\n");
        return false;
    }
    printf("[DET] Sistema listo — procesando canal por canal\r\n");
    return true;
}

/* ── Acumular muestras del canal activo ──────────────────────── */
void drone_detection_accumulate(void)
{
    extern AudioCaptureContext g_audio_ctx;

    int32_t *ch = audio_capture_get_channel(&g_audio_ctx, s_current_ch);
    if (ch == NULL) return;

    int space = SAMPLES_PER_SECOND - s_accum_idx;
    int copy  = (AUDIO_BUFFER_SIZE < space) ? AUDIO_BUFFER_SIZE : space;

    for (int i = 0; i < copy; i++) {
    	// Por esto (24 bits en bits 31-8):
    	s_accum[s_accum_idx + i] = (float)(ch[i] >> 8) / 8388608.0f;
    }

    s_accum_idx += copy;
    if (s_accum_idx >= SAMPLES_PER_SECOND) {
        s_accum_idx   = 0;
        s_accum_ready = true;
    }
}

bool drone_detection_is_ready(void) { return s_accum_ready; }

/* ── Helpers ─────────────────────────────────────────────────── */
static float compute_dbfs(const float *buf, int n)
{
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
    float rms = sqrtf(sum / (float)n);
    return (rms > 1e-12f) ? 20.0f * log10f(rms) : -120.0f;
}

/* ── Pipeline — procesa el canal activo ──────────────────────── */
void drone_detection_process(void)
{
    if (!s_accum_ready) return;
    s_accum_ready = false;

    int ch = s_current_ch;

    /* 1. DC offset */
    float mean = 0.0f;
    for (int i = 0; i < SAMPLES_PER_SECOND; i++) mean += s_accum[i];
    mean /= SAMPLES_PER_SECOND;
    for (int i = 0; i < SAMPLES_PER_SECOND; i++) s_accum[i] -= mean;

    /* 2. Pre-enfasis */
    for (int i = SAMPLES_PER_SECOND - 1; i >= 1; i--)
        s_accum[i] -= 0.97f * s_accum[i - 1];

    /* 3. dBFS */
    s_db_ch[ch] = compute_dbfs(s_accum, SAMPLES_PER_SECOND);


    printf("[DBG ch%d] accum[0]=%.6f accum[512]=%.6f accum[1000]=%.6f\r\n",
           ch, s_accum[0], s_accum[512], s_accum[1000]);

    /* 4. Gate de silencio */
    if (s_db_ch[ch] < SILENCE_DB) {
        s_p_ch[ch] = 0.0f;
    } else {
        /* 5. DSP: HPF + ganancia×15 + tanh */
        mfcc_stm32_apply_dsp(s_accum, SAMPLES_PER_SECOND);

        /* 6. MFCC */
        if (mfcc_stm32_compute(s_accum, &s_mfcc)) {
            /* 7. Inferencia */
            s_p_ch[ch] = model_runner_infer(s_mfcc.data);
        } else {
            s_p_ch[ch] = 0.0f;
        }
    }

    /* Avanzar al siguiente canal */
    s_current_ch = (s_current_ch + 1) % 4;

    /* Cuando completamos los 4 canales → calcular EMA y alertas */
    if (s_current_ch == 0) {
        float p_drone = (s_p_ch[0] + s_p_ch[1] + s_p_ch[2] + s_p_ch[3]) / 4.0f;
        float alpha   = (p_drone > s_ema) ? ALPHA_RISE : ALPHA_FALL;
        s_ema = s_ema * (1.0f - alpha) + p_drone * alpha;

        printf("Mic1:%5.0fdB p=%.2f | Mic2:%5.0fdB p=%.2f | "
               "Mic3:%5.0fdB p=%.2f | Mic4:%5.0fdB p=%.2f | EMA:%.2f",
               s_db_ch[0], s_p_ch[0],
               s_db_ch[1], s_p_ch[1],
               s_db_ch[2], s_p_ch[2],
               s_db_ch[3], s_p_ch[3], s_ema);

        if (s_ema >= THRESH_TRIGGER_FAST && p_drone > THRESH_INSTANT) {
            printf(" >>> ALERTA ROJA: DRON DETECTADO (%.0f%%)\r\n", s_ema * 100.0f);
            s_persistence = TICKS_PERSISTENCE;
        } else if (s_ema >= THRESH_SUSPICION) {
            s_persistence++;
            printf(" [RASTREANDO %d/%d]\r\n", s_persistence, TICKS_PERSISTENCE);
            if (s_persistence >= TICKS_PERSISTENCE)
                printf(" >>> ALERTA NARANJA: DRON LEJANO CONFIRMADO\r\n");
        } else {
            if (s_persistence > 0) printf(" [senal perdida]\r\n");
            else printf("\r\n");
            s_persistence = 0;
        }
        /* ── Transmitir por CAN ──────────────────────────────── */
                uint8_t alerta_can = (s_ema >= THRESH_TRIGGER_FAST) ? 3 :
                                     (s_ema >= THRESH_SUSPICION)     ? 1 : 0;
                can_sender_transmit(s_p_ch, s_db_ch, s_ema, alerta_can);
    }
}
