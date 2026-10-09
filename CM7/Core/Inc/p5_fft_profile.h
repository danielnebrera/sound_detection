#ifndef P5_FFT_PROFILE_H
#define P5_FFT_PROFILE_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32h7xx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P5_FFT_STAGE_RFFT_TOTAL = 0,
    P5_FFT_STAGE_CFFT_TOTAL,
    P5_FFT_STAGE_RFFT_STAGE,
    P5_FFT_STAGE_CFFT_KERNEL,
    P5_FFT_STAGE_BITREV,
    P5_FFT_STAGE_RADIX2_PREP,
    P5_FFT_STAGE_RADIX8_COL1,
    P5_FFT_STAGE_RADIX8_COL2,
    P5_FFT_STAGE_COUNT
} P5FftProfileStage;

typedef struct {
    uint64_t total_cycles;
    uint32_t last_cycles;
    uint32_t max_cycles;
    uint32_t runs;
} P5FftProfileStageStats;

typedef struct {
    P5FftProfileStageStats stage[P5_FFT_STAGE_COUNT];
} P5FftProfile;

static inline uint32_t p5_fft_profile_now(void)
{
    return DWT->CYCCNT;
}

void p5_fft_profile_reset(void);
bool p5_fft_profile_get(P5FftProfile *out);
void p5_fft_profile_add(P5FftProfileStage stage, uint32_t cycles);

#ifdef __cplusplus
}
#endif

#endif
