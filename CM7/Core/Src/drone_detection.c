/* =================================================================
 * drone_detection.c — v12
 * Mic1 (ch0, SAI2_SD_B) + Mic3 (ch1, SAI2_SD_A)
 * Ambos con misma normalización >>8 / 8388608.0f
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

/* Buffer ch0 en RAM_D1 */
static float s_accum_ch0[SAMPLES_PER_SECOND];

/* Buffer ch1 en RAM_D2 */
__attribute__((section(".RAM_D2_bss")))
static float s_accum_ch1[SAMPLES_PER_SECOND];

static int  s_idx_ch0 = 0;
static int  s_idx_ch1 = 0;
static bool s_rdy_ch0 = false;
static bool s_rdy_ch1 = false;

static float s_hpf_in[2]  = {0, 0};
static float s_hpf_out[2] = {0, 0};

static mfcc_100x20_t s_mfcc;

static float s_p_ch[4]    = {0, 0, 0, 0};
static float s_db_ch[4]   = {-120, -120, -120, -120};
static bool  s_ch_valid[2] = {false, false};

static float s_ema        = 0.0f;
static int   s_persistence = 0;

bool drone_detection_init(void)
{
    printf("[DET] Inicializando deteccion de drones...\r\n");
    memset(s_accum_ch0, 0, sizeof(s_accum_ch0));
    memset(s_accum_ch1, 0, sizeof(s_accum_ch1));
    s_idx_ch0 = 0; s_idx_ch1 = 0;
    s_rdy_ch0 = false; s_rdy_ch1 = false;
    s_ch_valid[0] = false; s_ch_valid[1] = false;
    memset(s_hpf_in,  0, sizeof(s_hpf_in));
    memset(s_hpf_out, 0, sizeof(s_hpf_out));

    if (!mfcc_stm32_init()) {
        printf("[DET] ERROR: mfcc_stm32_init fallo\r\n");
        return false;
    }
    if (!model_runner_init()) {
        printf("[DET] ERROR: model_runner_init fallo\r\n");
        return false;
    }
    printf("[DET] Sistema listo — Mic1(SAI_B) + Mic3(SAI_A)\r\n");
    return true;
}

void drone_detection_accumulate(void)
{
    extern AudioCaptureContext g_audio_ctx;
    int32_t *ch0 = audio_capture_get_channel(&g_audio_ctx, 0);  /* Mic1 */
    int32_t *ch1 = audio_capture_get_channel(&g_audio_ctx, 1);  /* Mic3 */

    const float alpha = 0.9f;

    /* ── Mic1 (ch0) ─────────────────────────────────────────── */
    if (!s_rdy_ch0 && ch0 != NULL) {
        int space = SAMPLES_PER_SECOND - s_idx_ch0;
        int copy  = (AUDIO_BUFFER_SIZE < space) ? AUDIO_BUFFER_SIZE : space;
        for (int i = 0; i < copy; i++) {
            float in = (float)(ch0[i] >> 8) / 8388608.0f;
            float hpf = alpha * (s_hpf_out[0] + in - s_hpf_in[0]);
            s_hpf_in[0]  = in;
            s_hpf_out[0] = hpf;
            s_accum_ch0[s_idx_ch0 + i] = tanhf(hpf * 15.0f);
        }
        s_idx_ch0 += copy;
        if (s_idx_ch0 >= SAMPLES_PER_SECOND) {
            s_idx_ch0 = 0;
            s_rdy_ch0 = true;
        }
    }

    /* ── Mic3 (ch1) — misma normalización ───────────────────── */
    if (!s_rdy_ch1 && ch1 != NULL) {
        int space = SAMPLES_PER_SECOND - s_idx_ch1;
        int copy  = (AUDIO_BUFFER_SIZE < space) ? AUDIO_BUFFER_SIZE : space;
        for (int i = 0; i < copy; i++) {
            float in = (float)(ch1[i] >> 8) / 8388608.0f;
            float hpf = alpha * (s_hpf_out[1] + in - s_hpf_in[1]);
            s_hpf_in[1]  = in;
            s_hpf_out[1] = hpf;
            s_accum_ch1[s_idx_ch1 + i] = tanhf(hpf * 15.0f);
        }
        s_idx_ch1 += copy;
        if (s_idx_ch1 >= SAMPLES_PER_SECOND) {
            s_idx_ch1 = 0;
            s_rdy_ch1 = true;
        }
    }
}

bool drone_detection_is_ready(void)
{
    return s_rdy_ch0 && s_rdy_ch1;
}

static float compute_dbfs(const float *buf, int n)
{
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += buf[i] * buf[i];
    float rms = sqrtf(sum / (float)n);
    return (rms > 1e-12f) ? 20.0f * log10f(rms) : -120.0f;
}

static void process_channel(int ch, float *buf, float silence_db)
{
    float mean = 0.0f;
    for (int i = 0; i < SAMPLES_PER_SECOND; i++) mean += buf[i];
    mean /= SAMPLES_PER_SECOND;
    for (int i = 0; i < SAMPLES_PER_SECOND; i++) buf[i] -= mean;

    for (int i = SAMPLES_PER_SECOND - 1; i >= 1; i--)
        buf[i] -= 0.97f * buf[i - 1];

    s_db_ch[ch] = compute_dbfs(buf, SAMPLES_PER_SECOND);

    printf("[GATE ch%d] dBFS=%.1f\r\n", ch, s_db_ch[ch]);

    if (s_db_ch[ch] < silence_db) {
        s_p_ch[ch]     = 0.0f;
        s_ch_valid[ch] = false;
        return;
    }

    s_ch_valid[ch] = true;
    if (mfcc_stm32_compute(buf, &s_mfcc)) {
        s_p_ch[ch] = model_runner_infer(s_mfcc.data);
    } else {
        s_p_ch[ch] = 0.0f;
    }
}

void drone_detection_process(void)
{
    if (!s_rdy_ch0 || !s_rdy_ch1) return;

    s_rdy_ch0 = false;
    s_rdy_ch1 = false;

    process_channel(0, s_accum_ch0, SILENCE_DB);
    process_channel(1, s_accum_ch1, SILENCE_DB);

    float p_drone = 0.0f;
    int   valid   = 0;
    for (int i = 0; i < 2; i++) {
        if (s_ch_valid[i]) {
            p_drone += s_p_ch[i];
            valid++;
        }
    }

    if (valid == 0) {
        s_ema = s_ema * (1.0f - ALPHA_FALL);
        s_persistence = 0;
        printf("Mic1:%5.0fdB p=0.00 | Mic3:%5.0fdB p=0.00 | EMA:%.2f\r\n",
               s_db_ch[0], s_db_ch[1], s_ema);
        can_sender_transmit(s_p_ch, s_db_ch, s_ema, 0);
        return;
    }

    p_drone /= (float)valid;

    float alpha = (p_drone > s_ema) ? ALPHA_RISE : ALPHA_FALL;
    s_ema = s_ema * (1.0f - alpha) + p_drone * alpha;

    printf("Mic1:%5.0fdB p=%.2f | Mic3:%5.0fdB p=%.2f | EMA:%.2f",
           s_db_ch[0], s_p_ch[0],
           s_db_ch[1], s_p_ch[1],
           s_ema);

    if (s_ema >= THRESH_TRIGGER_FAST && p_drone > THRESH_INSTANT) {
        printf(" >>> ALERTA ROJA: DRON DETECTADO (%.0f%%)\r\n", s_ema * 100.0f);
        s_persistence = TICKS_PERSISTENCE;
    } else if (s_ema >= THRESH_SUSPICION) {
        s_persistence++;
        if (s_persistence > TICKS_PERSISTENCE)
            s_persistence = TICKS_PERSISTENCE;
        printf(" [RASTREANDO %d/%d]\r\n", s_persistence, TICKS_PERSISTENCE);
        if (s_persistence >= TICKS_PERSISTENCE)
            printf(" >>> ALERTA NARANJA: DRON LEJANO CONFIRMADO\r\n");
    } else {
        if (s_persistence > 0) printf(" [senal perdida]\r\n");
        else printf("\r\n");
        s_persistence = 0;
    }

    uint8_t alerta_can = (s_ema >= THRESH_TRIGGER_FAST) ? 3 :
                         (s_ema >= THRESH_SUSPICION)     ? 1 : 0;
    can_sender_transmit(s_p_ch, s_db_ch, s_ema, alerta_can);
}
