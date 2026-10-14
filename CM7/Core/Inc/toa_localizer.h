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

typedef struct
{
    ToaPairResult pair[TOA_PAIR_COUNT];
    float h_samples;
    float v_samples;
    float azimuth_deg;
    ToaDirection direction;
    uint8_t valid;
    uint8_t reserved[3];
} ToaResult;

bool toa_localizer_process(
    const RecorderChunkView *window,
    ToaResult *out);

const char *toa_direction_text(ToaDirection direction);

#ifdef __cplusplus
}
#endif

#endif /* TOA_LOCALIZER_H */
