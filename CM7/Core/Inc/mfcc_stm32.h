#ifndef MFCC_STM32_H
#define MFCC_STM32_H

#include <stdbool.h>
#include <stdint.h>

#define MFCC_SAMPLE_RATE    44100
#define MFCC_N_FFT          2048
#define MFCC_HOP            442
#define MFCC_N_MELS         64
#define MFCC_N_MFCC         20
#define MFCC_TARGET_FRAMES  100

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float data[MFCC_TARGET_FRAMES * MFCC_N_MFCC];
} mfcc_100x20_t;

bool mfcc_stm32_init(void);
bool mfcc_stm32_compute(const float *pcm_1s, mfcc_100x20_t *out);
bool mfcc_stm32_compute_n(const float *pcm, int n_samples, mfcc_100x20_t *out);
void mfcc_stm32_apply_dsp(float *samples, int n);


typedef enum {
    MFCC_PROFILE_STAGE_WINDOW = 0,
    MFCC_PROFILE_STAGE_FFT,
    MFCC_PROFILE_STAGE_POWER,
    MFCC_PROFILE_STAGE_MEL,
    MFCC_PROFILE_STAGE_LOG,
    MFCC_PROFILE_STAGE_DCT,
    MFCC_PROFILE_STAGE_COUNT
} MfccProfileStage;

typedef struct {
    uint64_t total_cycles;
    uint32_t last_cycles;
    uint32_t max_cycles;
    uint32_t runs;
} MfccProfileStageStats;

typedef struct {
    MfccProfileStageStats stage[MFCC_PROFILE_STAGE_COUNT];
    uint64_t compute_total_cycles;
    uint32_t compute_last_cycles;
    uint32_t compute_max_cycles;
    uint32_t compute_runs;
    uint32_t frames_profiled;
} MfccStm32Profile;

void mfcc_stm32_profile_reset(void);
bool mfcc_stm32_profile_get(MfccStm32Profile *out);

/**
 * Exporta por UART las etapas intermedias del frame frame_idx:
 *   POWER (1025), MEL_ENERGY (64), MEL_DB (64), MFCC_F (20)
 */
void mfcc_stm32_export_frame(const float *pcm_1s, int frame_idx);

#ifdef __cplusplus
}
#endif

#endif /* MFCC_STM32_H */
