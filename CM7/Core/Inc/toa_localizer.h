#ifndef TOA_LOCALIZER_H
#define TOA_LOCALIZER_H

#include <stdbool.h>
#include <stdint.h>

#include "audio_recorder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    TOA_PAIR_LEFT_TOP = 0,
    TOA_PAIR_LEFT_BACK,
    TOA_PAIR_LEFT_RIGHT,
    TOA_PAIR_TOP_BACK,
    TOA_PAIR_TOP_RIGHT,
    TOA_PAIR_BACK_RIGHT,
    TOA_PAIR_COUNT
} ToaPair;

typedef enum
{
    TOA_DIRECTION_UP = 0,
    TOA_DIRECTION_UP_RIGHT,
    TOA_DIRECTION_RIGHT,
    TOA_DIRECTION_DOWN_RIGHT,
    TOA_DIRECTION_DOWN,
    TOA_DIRECTION_DOWN_LEFT,
    TOA_DIRECTION_LEFT,
    TOA_DIRECTION_UP_LEFT,
    TOA_DIRECTION_UNKNOWN
} ToaDirection;

typedef enum
{
    TOA_CONFIDENCE_NONE = 0,
    TOA_CONFIDENCE_LOW,
    TOA_CONFIDENCE_MEDIUM,
    TOA_CONFIDENCE_HIGH
} ToaMeasurementConfidence;

#define TOA_PEAK_CANDIDATE_COUNT 3U

typedef struct
{
    float lag_samples;
    float correlation;
    int16_t integer_lag;
    uint8_t peak_at_search_boundary;
    uint8_t valid;
} ToaPeakCandidate;

typedef struct
{
    float lag_samples;
    float correlation;
    int16_t integer_lag;
    uint8_t peak_at_search_boundary;
    uint8_t reserved;
    ToaPeakCandidate candidate[TOA_PEAK_CANDIDATE_COUNT];
} ToaPairResult;

enum
{
    TOA_INVALID_NONE = 0U,
    TOA_INVALID_SEARCH_BOUNDARY = (1U << 0),
    TOA_INVALID_CLOSURE = (1U << 1)
};

typedef struct
{
    ToaPairResult pair[TOA_PAIR_COUNT];
    float h_samples;
    float v_samples;
    float azimuth_deg;
    float closure_error_samples;
    float closure_e1_samples;
    float closure_e2_samples;
    float closure_e3_samples;
    float closure_e4_samples;
    float raw_closure_error_samples;
    float solver_raw_score;
    float solver_selected_score;
    float solver_score_ratio;
    float confidence_peak_gap_min;
    float confidence_selected_corr_min;
    float confidence_selected_corr_mean;
    uint16_t solver_combinations_tested;
    uint16_t solver_valid_combinations;
    ToaDirection direction;
    ToaMeasurementConfidence confidence;
    uint8_t valid;
    uint8_t closure_valid;
    uint8_t invalid_flags;
    uint8_t raw_invalid_flags;
    uint8_t raw_valid;
    uint8_t solver_used;
    uint8_t solver_recovered;
    uint8_t confidence_ambiguous_pairs;
    uint8_t confidence_gap_pair_count;
    uint8_t track_eligible;
    uint8_t selected_candidate[TOA_PAIR_COUNT];
} ToaResult;

bool toa_localizer_process(
    const RecorderChunkView *window,
    ToaResult *out);

const char *toa_direction_text(ToaDirection direction);
const char *toa_confidence_text(ToaMeasurementConfidence confidence);

#ifdef __cplusplus
}
#endif

#endif /* TOA_LOCALIZER_H */
