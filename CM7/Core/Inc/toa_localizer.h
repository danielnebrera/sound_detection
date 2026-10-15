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

typedef struct
{
    float lag_samples;
    float correlation;
    int16_t integer_lag;
    uint8_t peak_at_search_boundary;
    uint8_t reserved;
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
    ToaDirection direction;
    uint8_t valid;
    uint8_t closure_valid;
    uint8_t invalid_flags;
    uint8_t reserved;
} ToaResult;

bool toa_localizer_process(
    const RecorderChunkView *window,
    ToaResult *out);

const char *toa_direction_text(ToaDirection direction);

#ifdef __cplusplus
}
#endif

#endif /* TOA_LOCALIZER_H */
