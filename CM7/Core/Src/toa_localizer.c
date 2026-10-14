#include "toa_localizer.h"

#include <math.h>
#include <string.h>

#define TOA_SAMPLE_COUNT        44100U
#define TOA_WINDOW_SAMPLE_COUNT 4410U
#define TOA_WINDOW_START        ((TOA_SAMPLE_COUNT - TOA_WINDOW_SAMPLE_COUNT) / 2U)
#define TOA_PI_F                3.14159265358979323846f
#define TOA_RAD_TO_DEG_F        (180.0f / TOA_PI_F)

#define TOA_CH_LEFT             0U
#define TOA_CH_TOP              1U
#define TOA_CH_BACK             2U
#define TOA_CH_RIGHT            3U

#define TOA_MAX_LAG_HORIZONTAL  14
#define TOA_MAX_LAG_VERTICAL    18
#define TOA_MAX_LAG_DIAGONAL    22

static float correlation_at_lag(
    const int16_t *a,
    const int16_t *b,
    uint32_t count,
    int32_t lag)
{
    uint32_t a_start = 0U;
    uint32_t b_start = 0U;
    uint32_t overlap = count;

    if (lag > 0)
    {
        b_start = (uint32_t)lag;
        overlap -= (uint32_t)lag;
    }
    else if (lag < 0)
    {
        a_start = (uint32_t)(-lag);
        overlap -= (uint32_t)(-lag);
    }

    int64_t sum_a = 0;
    int64_t sum_b = 0;
    int64_t sum_aa = 0;
    int64_t sum_bb = 0;
    int64_t sum_ab = 0;

    for (uint32_t i = 0U; i < overlap; i++)
    {
        const int32_t x = (int32_t)a[a_start + i];
        const int32_t y = (int32_t)b[b_start + i];

        sum_a += (int64_t)x;
        sum_b += (int64_t)y;
        sum_aa += (int64_t)x * (int64_t)x;
        sum_bb += (int64_t)y * (int64_t)y;
        sum_ab += (int64_t)x * (int64_t)y;
    }

    const double n = (double)overlap;
    const double numerator =
        n * (double)sum_ab -
        (double)sum_a * (double)sum_b;

    const double energy_a =
        n * (double)sum_aa -
        (double)sum_a * (double)sum_a;

    const double energy_b =
        n * (double)sum_bb -
        (double)sum_b * (double)sum_b;

    if ((energy_a <= 0.0) || (energy_b <= 0.0))
    {
        return 0.0f;
    }

    return (float)(numerator / sqrt(energy_a * energy_b));
}

static bool estimate_pair(
    const int16_t *a,
    const int16_t *b,
    uint32_t count,
    int32_t max_lag,
    ToaPairResult *out)
{
    if ((a == NULL) || (b == NULL) || (out == NULL) ||
        (count <= (uint32_t)(2 * max_lag + 1)))
    {
        return false;
    }

    int32_t best_lag = 0;
    float best_corr = -2.0f;

    for (int32_t lag = -max_lag; lag <= max_lag; lag++)
    {
        const float corr = correlation_at_lag(a, b, count, lag);

        if (corr > best_corr)
        {
            best_corr = corr;
            best_lag = lag;
        }
    }

    float delta = 0.0f;

    if ((best_lag > -max_lag) && (best_lag < max_lag))
    {
        const float corr_m1 =
            correlation_at_lag(a, b, count, best_lag - 1);
        const float corr_0 = best_corr;
        const float corr_p1 =
            correlation_at_lag(a, b, count, best_lag + 1);

        const float denominator =
            corr_m1 - 2.0f * corr_0 + corr_p1;

        if (fabsf(denominator) > 1.0e-12f)
        {
            delta =
                0.5f * (corr_m1 - corr_p1) / denominator;

            if (delta > 1.0f)
            {
                delta = 1.0f;
            }
            else if (delta < -1.0f)
            {
                delta = -1.0f;
            }
        }
    }

    out->integer_lag = (int16_t)best_lag;
    out->lag_samples = (float)best_lag + delta;
    out->correlation = best_corr;
    out->peak_at_search_boundary =
        ((best_lag == -max_lag) || (best_lag == max_lag)) ? 1U : 0U;

    return true;
}

static ToaDirection direction_from_azimuth(float azimuth_deg)
{
    if ((azimuth_deg >= 337.5f) || (azimuth_deg < 22.5f))
    {
        return TOA_DIRECTION_UP;
    }
    if (azimuth_deg < 67.5f)
    {
        return TOA_DIRECTION_UP_RIGHT;
    }
    if (azimuth_deg < 112.5f)
    {
        return TOA_DIRECTION_RIGHT;
    }
    if (azimuth_deg < 157.5f)
    {
        return TOA_DIRECTION_DOWN_RIGHT;
    }
    if (azimuth_deg < 202.5f)
    {
        return TOA_DIRECTION_DOWN;
    }
    if (azimuth_deg < 247.5f)
    {
        return TOA_DIRECTION_DOWN_LEFT;
    }
    if (azimuth_deg < 292.5f)
    {
        return TOA_DIRECTION_LEFT;
    }

    return TOA_DIRECTION_UP_LEFT;
}

bool toa_localizer_process(
    const RecorderChunkView *window,
    ToaResult *out)
{
    if ((window == NULL) || (out == NULL) ||
        (window->frame_count != TOA_SAMPLE_COUNT))
    {
        return false;
    }

    for (uint32_t channel = 0U; channel < RECORD_CHANNELS; channel++)
    {
        if (window->channel[channel] == NULL)
        {
            return false;
        }
    }

    memset(out, 0, sizeof(*out));

    const int16_t *left = window->channel[TOA_CH_LEFT] + TOA_WINDOW_START;
    const int16_t *top = window->channel[TOA_CH_TOP] + TOA_WINDOW_START;
    const int16_t *back = window->channel[TOA_CH_BACK] + TOA_WINDOW_START;
    const int16_t *right = window->channel[TOA_CH_RIGHT] + TOA_WINDOW_START;

    if (!estimate_pair(
            left,
            top,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_VERTICAL,
            &out->pair[TOA_PAIR_LEFT_TOP]) ||
        !estimate_pair(
            left,
            back,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_DIAGONAL,
            &out->pair[TOA_PAIR_LEFT_BACK]) ||
        !estimate_pair(
            left,
            right,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_HORIZONTAL,
            &out->pair[TOA_PAIR_LEFT_RIGHT]) ||
        !estimate_pair(
            top,
            back,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_HORIZONTAL,
            &out->pair[TOA_PAIR_TOP_BACK]) ||
        !estimate_pair(
            top,
            right,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_DIAGONAL,
            &out->pair[TOA_PAIR_TOP_RIGHT]) ||
        !estimate_pair(
            back,
            right,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_VERTICAL,
            &out->pair[TOA_PAIR_BACK_RIGHT]))
    {
        return false;
    }

    out->h_samples =
        0.5f *
        (
            out->pair[TOA_PAIR_LEFT_RIGHT].lag_samples +
            out->pair[TOA_PAIR_TOP_BACK].lag_samples
        );

    out->v_samples =
        0.5f *
        (
            out->pair[TOA_PAIR_LEFT_TOP].lag_samples -
            out->pair[TOA_PAIR_BACK_RIGHT].lag_samples
        );

    const float x = -out->h_samples / 0.09f;
    const float y = out->v_samples / 0.12f;

    float azimuth = atan2f(x, y) * TOA_RAD_TO_DEG_F;

    if (azimuth < 0.0f)
    {
        azimuth += 360.0f;
    }
    else if (azimuth >= 360.0f)
    {
        azimuth -= 360.0f;
    }

    out->azimuth_deg = azimuth;
    out->direction = direction_from_azimuth(azimuth);
    out->valid = 1U;

    for (uint32_t pair = 0U; pair < TOA_PAIR_COUNT; pair++)
    {
        if (out->pair[pair].peak_at_search_boundary != 0U)
        {
            out->valid = 0U;
            break;
        }
    }

    return true;
}

const char *toa_direction_text(ToaDirection direction)
{
    switch (direction)
    {
        case TOA_DIRECTION_UP:
            return "UP";
        case TOA_DIRECTION_UP_RIGHT:
            return "UP_RIGHT";
        case TOA_DIRECTION_RIGHT:
            return "RIGHT";
        case TOA_DIRECTION_DOWN_RIGHT:
            return "DOWN_RIGHT";
        case TOA_DIRECTION_DOWN:
            return "DOWN";
        case TOA_DIRECTION_DOWN_LEFT:
            return "DOWN_LEFT";
        case TOA_DIRECTION_LEFT:
            return "LEFT";
        case TOA_DIRECTION_UP_LEFT:
            return "UP_LEFT";
        default:
            return "UNKNOWN";
    }
}
