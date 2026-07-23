/* =================================================================
 * drone_detection.c — repo v13 + sai24_to_float + DCT corregida
 * Mic2 (SAI2_SD_B, SEL=VCC) → ch0
 * Mic4 (SAI2_SD_A, SEL=VCC) → ch1
 * ================================================================= */
#include "drone_detection.h"
#include "mfcc_stm32.h"
#include "model_runner_stm32.h"
#include "audio_capture.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#define ALPHA_RISE          0.60f
#define ALPHA_FALL          0.15f
#define THRESH_TRIGGER_FAST 0.85f
#define THRESH_INSTANT      0.60f
#define TICKS_PERSISTENCE   3
#define SILENCE_DB         -35.0f
#define SAMPLES_PER_SECOND  44100

/* Conversión correcta para DATASIZE_24 */
static inline float sai24_to_float(int32_t word)
{
    int32_t s = (int32_t)((uint32_t)word & 0x00FFFFFFu);
    if (s & 0x00800000L) s |= (int32_t)0xFF000000u;
    return (float)s / 8388608.0f;
}

static float s_accum_ch0[SAMPLES_PER_SECOND];

__attribute__((section(".RAM_D2_bss")))
static float s_accum_ch1[SAMPLES_PER_SECOND];

static int  s_idx_ch0 = 0;
static int  s_idx_ch1 = 0;
static bool s_rdy_ch0 = false;
static bool s_rdy_ch1 = false;

static float s_hpf_in[2]   = {0, 0};
static float s_hpf_out[2]  = {0, 0};

static mfcc_100x20_t s_mfcc;
static float s_p_ch[2]     = {0, 0};
static float s_db_ch[2]    = {-120, -120};
static bool  s_ch_valid[2] = {false, false};
static float s_ema         = 0.0f;
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
    printf("[DET] Sistema listo — Mic2(SAI_B) + Mic4(SAI_A)\r\n");
    return true;
}

void drone_detection_accumulate(void)
{
    extern AudioCaptureContext g_audio_ctx;
    int32_t *ch0 = audio_capture_get_channel(&g_audio_ctx, 0); /* Mic2 */
    int32_t *ch1 = audio_capture_get_channel(&g_audio_ctx, 1); /* Mic4 */
    const float alpha = 0.9f;

    if (!s_rdy_ch0 && ch0 != NULL) {
        int space = SAMPLES_PER_SECOND - s_idx_ch0;
        int copy  = (AUDIO_BUFFER_SIZE < space) ? AUDIO_BUFFER_SIZE : space;
        for (int i = 0; i < copy; i++) {
            float in  = sai24_to_float(ch0[i]);
            float hpf = alpha * (s_hpf_out[0] + in - s_hpf_in[0]);
            s_hpf_in[0]  = in;
            s_hpf_out[0] = hpf;
            s_accum_ch0[s_idx_ch0 + i] = tanhf(hpf * 15.0f);
        }
        s_idx_ch0 += copy;
        if (s_idx_ch0 >= SAMPLES_PER_SECOND) { s_idx_ch0 = 0; s_rdy_ch0 = true; }
    }

    if (!s_rdy_ch1 && ch1 != NULL) {
        int space = SAMPLES_PER_SECOND - s_idx_ch1;
        int copy  = (AUDIO_BUFFER_SIZE < space) ? AUDIO_BUFFER_SIZE : space;
        for (int i = 0; i < copy; i++) {
            float in  = sai24_to_float(ch1[i]);
            float hpf = alpha * (s_hpf_out[1] + in - s_hpf_in[1]);
            s_hpf_in[1]  = in;
            s_hpf_out[1] = hpf;
            s_accum_ch1[s_idx_ch1 + i] = tanhf(hpf * 15.0f);
        }
        s_idx_ch1 += copy;
        if (s_idx_ch1 >= SAMPLES_PER_SECOND) { s_idx_ch1 = 0; s_rdy_ch1 = true; }
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

static void process_channel(int ch, float *buf)
{
    float mean = 0.0f;
    for (int i = 0; i < SAMPLES_PER_SECOND; i++) mean += buf[i];
    mean /= SAMPLES_PER_SECOND;
    for (int i = 0; i < SAMPLES_PER_SECOND; i++) buf[i] -= mean;
    for (int i = SAMPLES_PER_SECOND - 1; i >= 1; i--)
        buf[i] -= 0.97f * buf[i - 1];

    s_db_ch[ch] = compute_dbfs(buf, SAMPLES_PER_SECOND);
    if (s_db_ch[ch] < SILENCE_DB) {
        s_p_ch[ch]     = 0.0f;
        s_ch_valid[ch] = false;
        return;
    }
    s_ch_valid[ch] = true;
    if (mfcc_stm32_compute(buf, &s_mfcc))
        s_p_ch[ch] = model_runner_infer(s_mfcc.data);
    else
        s_p_ch[ch] = 0.0f;
}

void drone_detection_process(void)
{
    if (!s_rdy_ch0 || !s_rdy_ch1) return;
    s_rdy_ch0 = false;
    s_rdy_ch1 = false;

    process_channel(0, s_accum_ch0);
    process_channel(1, s_accum_ch1);

    float p_final   = 0.0f;
    int valid_count = 0;
    if (s_ch_valid[0]) { p_final += s_p_ch[0]; valid_count++; }
    if (s_ch_valid[1]) { p_final += s_p_ch[1]; valid_count++; }
    if (valid_count > 0) p_final /= valid_count;

    float alpha = (p_final > s_ema) ? ALPHA_RISE : ALPHA_FALL;
    s_ema = alpha * p_final + (1.0f - alpha) * s_ema;

    if (s_ema >= THRESH_TRIGGER_FAST || p_final >= THRESH_INSTANT)
        s_persistence = TICKS_PERSISTENCE;
    else if (s_persistence > 0)
        s_persistence--;

    printf("[DET] Mic2:%5.1fdB Mic4:%5.1fdB p=[%.2f %.2f] EMA=%.2f %s\r\n",
           s_db_ch[0], s_db_ch[1],
           s_p_ch[0], s_p_ch[1],
           s_ema,
           s_persistence > 0 ? "*** DRON ***" : "");
}
