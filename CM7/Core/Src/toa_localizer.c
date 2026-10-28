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

#define TOA_MAX_LAG_TETRA       22
#define TOA_CLOSURE_MAX_SAMPLES  1.50f
#define TOA_PEAK_MIN_SEPARATION   2
#define TOA_CORR_VALUE_COUNT_MAX  (2 * TOA_MAX_LAG_TETRA + 1)
#define TOA_SOLVER_MIN_SCORE_RATIO 0.95f
#define TOA_SOLVER_SCORE_EPSILON   1.0e-6f
#define TOA_CONF_AMBIGUOUS_GAP_MAX  0.020f
#define TOA_CONF_HIGH_CLOSURE_MAX   0.80f

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

static float refine_peak_delta(
    const float *corr_values,
    int32_t max_lag,
    int32_t lag)
{
    if ((corr_values == NULL) ||
        (lag <= -max_lag) ||
        (lag >= max_lag))
    {
        return 0.0f;
    }

    const uint32_t index = (uint32_t)(lag + max_lag);
    const float corr_m1 = corr_values[index - 1U];
    const float corr_0 = corr_values[index];
    const float corr_p1 = corr_values[index + 1U];
    const float denominator = corr_m1 - 2.0f * corr_0 + corr_p1;

    if (fabsf(denominator) <= 1.0e-12f)
    {
        return 0.0f;
    }

    float delta = 0.5f * (corr_m1 - corr_p1) / denominator;

    if (delta > 1.0f)
    {
        delta = 1.0f;
    }
    else if (delta < -1.0f)
    {
        delta = -1.0f;
    }

    return delta;
}

static bool lag_is_local_peak(
    const float *corr_values,
    int32_t max_lag,
    int32_t lag)
{
    const uint32_t index = (uint32_t)(lag + max_lag);
    const uint32_t last = (uint32_t)(2 * max_lag);
    const float value = corr_values[index];

    if ((index > 0U) && (value < corr_values[index - 1U]))
    {
        return false;
    }

    if ((index < last) && (value < corr_values[index + 1U]))
    {
        return false;
    }

    return true;
}

static bool lag_is_separated(
    int32_t lag,
    const ToaPeakCandidate candidate[TOA_PEAK_CANDIDATE_COUNT],
    uint32_t selected_count)
{
    for (uint32_t i = 0U; i < selected_count; i++)
    {
        if (candidate[i].valid == 0U)
        {
            continue;
        }

        int32_t distance = lag - (int32_t)candidate[i].integer_lag;
        if (distance < 0)
        {
            distance = -distance;
        }

        if (distance < TOA_PEAK_MIN_SEPARATION)
        {
            return false;
        }
    }

    return true;
}

static void fill_peak_candidate(
    const float *corr_values,
    int32_t max_lag,
    int32_t lag,
    ToaPeakCandidate *candidate)
{
    const uint32_t index = (uint32_t)(lag + max_lag);
    const float delta = refine_peak_delta(corr_values, max_lag, lag);

    candidate->integer_lag = (int16_t)lag;
    candidate->lag_samples = (float)lag + delta;
    candidate->correlation = corr_values[index];
    candidate->peak_at_search_boundary =
        ((lag == -max_lag) || (lag == max_lag)) ? 1U : 0U;
    candidate->valid = 1U;
}

static bool estimate_pair(
    const int16_t *a,
    const int16_t *b,
    uint32_t count,
    int32_t max_lag,
    ToaPairResult *out)
{
    if ((a == NULL) || (b == NULL) || (out == NULL) ||
        (count <= (uint32_t)(2 * max_lag + 1)) ||
        ((2 * max_lag + 1) > TOA_CORR_VALUE_COUNT_MAX))
    {
        return false;
    }

    float corr_values[TOA_CORR_VALUE_COUNT_MAX];
    int32_t best_lag = 0;
    float best_corr = -2.0f;

    memset(corr_values, 0, sizeof(corr_values));
    memset(out, 0, sizeof(*out));

    for (int32_t lag = -max_lag; lag <= max_lag; lag++)
    {
        const float corr = correlation_at_lag(a, b, count, lag);
        const uint32_t index = (uint32_t)(lag + max_lag);
        corr_values[index] = corr;

        if (corr > best_corr)
        {
            best_corr = corr;
            best_lag = lag;
        }
    }

    fill_peak_candidate(
        corr_values,
        max_lag,
        best_lag,
        &out->candidate[0]
    );

    uint32_t selected_count = 1U;

    for (uint32_t candidate_index = 1U;
         candidate_index < TOA_PEAK_CANDIDATE_COUNT;
         candidate_index++)
    {
        bool found = false;
        int32_t selected_lag = 0;
        float selected_corr = -2.0f;

        for (int32_t lag = -max_lag; lag <= max_lag; lag++)
        {
            if (!lag_is_local_peak(corr_values, max_lag, lag) ||
                !lag_is_separated(lag, out->candidate, selected_count))
            {
                continue;
            }

            const float corr = corr_values[(uint32_t)(lag + max_lag)];

            if ((!found) || (corr > selected_corr))
            {
                found = true;
                selected_corr = corr;
                selected_lag = lag;
            }
        }

        if (!found)
        {
            break;
        }

        fill_peak_candidate(
            corr_values,
            max_lag,
            selected_lag,
            &out->candidate[candidate_index]
        );
        selected_count++;
    }

    out->integer_lag = out->candidate[0].integer_lag;
    out->lag_samples = out->candidate[0].lag_samples;
    out->correlation = out->candidate[0].correlation;
    out->peak_at_search_boundary =
        out->candidate[0].peak_at_search_boundary;

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

static void compute_closure_values(
    float lt,
    float lb,
    float lr,
    float tb,
    float tr,
    float br,
    float *e1,
    float *e2,
    float *e3,
    float *e4,
    float *closure)
{
    const float local_e1 = fabsf(lb - lt - tb);
    const float local_e2 = fabsf(lr - lt - tr);
    const float local_e3 = fabsf(lr - lb - br);
    const float local_e4 = fabsf(tr - tb - br);

    float local_closure = local_e1;

    if (local_e2 > local_closure)
    {
        local_closure = local_e2;
    }
    if (local_e3 > local_closure)
    {
        local_closure = local_e3;
    }
    if (local_e4 > local_closure)
    {
        local_closure = local_e4;
    }

    if (e1 != NULL)
    {
        *e1 = local_e1;
    }
    if (e2 != NULL)
    {
        *e2 = local_e2;
    }
    if (e3 != NULL)
    {
        *e3 = local_e3;
    }
    if (e4 != NULL)
    {
        *e4 = local_e4;
    }
    if (closure != NULL)
    {
        *closure = local_closure;
    }
}

static uint8_t selected_boundary_flag(const ToaResult *out)
{
    if (out == NULL)
    {
        return 1U;
    }

    for (uint32_t pair = 0U; pair < TOA_PAIR_COUNT; pair++)
    {
        if (out->pair[pair].peak_at_search_boundary != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}

/* Coordinates in metres: X west, Y north, Z up.
 * CH0 MIC2 south, CH1 MIC6 east, CH2 MIC5 west, CH3 MIC7 top.
 * Correlation lag (a,b) > 0 means signal reaches a before b.
 */
static const float tetra_xyz[4][3] = {
    { 0.00000f, -0.08660254f, 0.00000f},
    {-0.07500f,  0.04330127f, 0.00000f},
    { 0.07500f,  0.04330127f, 0.00000f},
    { 0.00000f,  0.00000000f, 0.12000f}
};
static const uint8_t tetra_pair_ch[TOA_PAIR_COUNT][2] = {
    {0U,1U}, {0U,2U}, {0U,3U}, {1U,2U}, {1U,3U}, {2U,3U}
};

static bool tetra_fit(ToaResult *out)
{
    float a[3][4] = {{0}};
    const float samples_per_metre = 44100.0f / 343.0f;
    for (uint32_t p = 0U; p < TOA_PAIR_COUNT; p++) {
        uint8_t i = tetra_pair_ch[p][0], j = tetra_pair_ch[p][1];
        float g[3];
        for (uint32_t k = 0U; k < 3U; k++)
            g[k] = (tetra_xyz[i][k] - tetra_xyz[j][k]) * samples_per_metre;
        const float lag = out->pair[p].lag_samples;
        for (uint32_t r = 0U; r < 3U; r++) {
            for (uint32_t col = 0U; col < 3U; col++)
                a[r][col] += g[r] * g[col];
            a[r][3] += g[r] * lag;
        }
    }
    for (uint32_t k = 0U; k < 3U; k++) {
        uint32_t pivot = k;
        for (uint32_t r = k + 1U; r < 3U; r++)
            if (fabsf(a[r][k]) > fabsf(a[pivot][k])) pivot = r;
        if (fabsf(a[pivot][k]) < 1.0e-6f) return false;
        if (pivot != k) for (uint32_t col = k; col < 4U; col++) {
            float t = a[k][col]; a[k][col] = a[pivot][col]; a[pivot][col] = t;
        }
        float div = a[k][k];
        for (uint32_t col = k; col < 4U; col++) a[k][col] /= div;
        for (uint32_t r = 0U; r < 3U; r++) if (r != k) {
            float mult = a[r][k];
            for (uint32_t col = k; col < 4U; col++) a[r][col] -= mult * a[k][col];
        }
    }
    float x = a[0][3], y = a[1][3], z = a[2][3];
    float norm = sqrtf(x*x + y*y + z*z);
    out->tetra_norm = norm;
    if (norm < 1.0e-6f) return false;
    float sum_sq = 0.0f;
    for (uint32_t p = 0U; p < TOA_PAIR_COUNT; p++) {
        uint8_t i = tetra_pair_ch[p][0], j = tetra_pair_ch[p][1];
        float prediction = samples_per_metre * (
            (tetra_xyz[i][0]-tetra_xyz[j][0])*x +
            (tetra_xyz[i][1]-tetra_xyz[j][1])*y +
            (tetra_xyz[i][2]-tetra_xyz[j][2])*z);
        float diff = prediction - out->pair[p].lag_samples;
        sum_sq += diff * diff;
    }
    out->tetra_rms_samples = sqrtf(sum_sq / (float)TOA_PAIR_COUNT);
    out->tetra_elevation_deg = atan2f(z, sqrtf(x*x+y*y))*TOA_RAD_TO_DEG_F;
    /* Legacy native azimuth clockwise from NORTH; monitor converts to physical. */
    float native_az = atan2f(-x, y)*TOA_RAD_TO_DEG_F;
    if (native_az < 0.0f) native_az += 360.0f;
    out->azimuth_deg = native_az;
    out->direction = direction_from_azimuth(native_az);
    /* H/V remain the legacy diagnostics; tetra information is explicit. */
    out->h_samples = 0.5f * (
        out->pair[TOA_PAIR_LEFT_RIGHT].lag_samples +
        out->pair[TOA_PAIR_TOP_BACK].lag_samples);
    out->v_samples = 0.5f * (
        out->pair[TOA_PAIR_LEFT_TOP].lag_samples -
        out->pair[TOA_PAIR_BACK_RIGHT].lag_samples);
    return true;
}

static void compute_selected_geometry(ToaResult *out)
{
    out->tetra_fit_ok = tetra_fit(out) ? 1U : 0U;

    const float lt = out->pair[TOA_PAIR_LEFT_TOP].lag_samples;
    const float lb = out->pair[TOA_PAIR_LEFT_BACK].lag_samples;
    const float lr = out->pair[TOA_PAIR_LEFT_RIGHT].lag_samples;
    const float tb = out->pair[TOA_PAIR_TOP_BACK].lag_samples;
    const float tr = out->pair[TOA_PAIR_TOP_RIGHT].lag_samples;
    const float br = out->pair[TOA_PAIR_BACK_RIGHT].lag_samples;

    compute_closure_values(
        lt,
        lb,
        lr,
        tb,
        tr,
        br,
        &out->closure_e1_samples,
        &out->closure_e2_samples,
        &out->closure_e3_samples,
        &out->closure_e4_samples,
        &out->closure_error_samples
    );

    out->closure_valid =
        (out->closure_error_samples <= TOA_CLOSURE_MAX_SAMPLES) ? 1U : 0U;
    out->invalid_flags = TOA_INVALID_NONE;

    if (selected_boundary_flag(out) != 0U)
    {
        out->invalid_flags |= TOA_INVALID_SEARCH_BOUNDARY;
    }

    if (out->closure_valid == 0U)
    {
        out->invalid_flags |= TOA_INVALID_CLOSURE;
    }

    out->valid = ((out->invalid_flags == TOA_INVALID_NONE) &&
                  (out->tetra_fit_ok != 0U)) ? 1U : 0U;

    if (out->valid == 0U)
    {
        out->direction = TOA_DIRECTION_UNKNOWN;
    }
}

static float p1_score(const ToaResult *out)
{
    float score = 0.0f;

    for (uint32_t pair = 0U; pair < TOA_PAIR_COUNT; pair++)
    {
        score += out->pair[pair].candidate[0].correlation;
    }

    return score;
}

static void apply_candidate_selection(
    ToaResult *out,
    const uint8_t selected[TOA_PAIR_COUNT])
{
    for (uint32_t pair = 0U; pair < TOA_PAIR_COUNT; pair++)
    {
        const uint8_t candidate_index = selected[pair];
        const ToaPeakCandidate *candidate =
            &out->pair[pair].candidate[candidate_index];

        out->pair[pair].integer_lag = candidate->integer_lag;
        out->pair[pair].lag_samples = candidate->lag_samples;
        out->pair[pair].correlation = candidate->correlation;
        out->pair[pair].peak_at_search_boundary =
            candidate->peak_at_search_boundary;
        out->selected_candidate[pair] = (uint8_t)(candidate_index + 1U);
    }
}

static bool solve_global_candidates(ToaResult *out)
{
    if ((out == NULL) || (out->solver_raw_score <= 0.0f))
    {
        return false;
    }

    bool found = false;
    float best_score = -1000.0f;
    float best_closure = 1000.0f;
    uint8_t best_selection[TOA_PAIR_COUNT] = {0};

    const uint32_t combination_count =
        TOA_PEAK_CANDIDATE_COUNT *
        TOA_PEAK_CANDIDATE_COUNT *
        TOA_PEAK_CANDIDATE_COUNT *
        TOA_PEAK_CANDIDATE_COUNT *
        TOA_PEAK_CANDIDATE_COUNT *
        TOA_PEAK_CANDIDATE_COUNT;

    for (uint32_t code = 0U; code < combination_count; code++)
    {
        uint32_t value = code;
        uint8_t selected[TOA_PAIR_COUNT] = {0};
        float lag[TOA_PAIR_COUNT] = {0.0f};
        float score = 0.0f;
        bool usable = true;

        for (uint32_t pair = 0U; pair < TOA_PAIR_COUNT; pair++)
        {
            const uint8_t candidate_index =
                (uint8_t)(value % TOA_PEAK_CANDIDATE_COUNT);
            value /= TOA_PEAK_CANDIDATE_COUNT;

            const ToaPeakCandidate *candidate =
                &out->pair[pair].candidate[candidate_index];

            if ((candidate->valid == 0U) ||
                (candidate->peak_at_search_boundary != 0U))
            {
                usable = false;
                break;
            }

            selected[pair] = candidate_index;
            lag[pair] = candidate->lag_samples;
            score += candidate->correlation;
        }

        if (!usable)
        {
            continue;
        }

        out->solver_combinations_tested++;

        float closure = 0.0f;
        compute_closure_values(
            lag[TOA_PAIR_LEFT_TOP],
            lag[TOA_PAIR_LEFT_BACK],
            lag[TOA_PAIR_LEFT_RIGHT],
            lag[TOA_PAIR_TOP_BACK],
            lag[TOA_PAIR_TOP_RIGHT],
            lag[TOA_PAIR_BACK_RIGHT],
            NULL,
            NULL,
            NULL,
            NULL,
            &closure
        );

        if (closure > TOA_CLOSURE_MAX_SAMPLES)
        {
            continue;
        }

        const float score_ratio = score / out->solver_raw_score;

        if (score_ratio < TOA_SOLVER_MIN_SCORE_RATIO)
        {
            continue;
        }

        out->solver_valid_combinations++;

        if ((!found) ||
            (score > (best_score + TOA_SOLVER_SCORE_EPSILON)) ||
            ((fabsf(score - best_score) <= TOA_SOLVER_SCORE_EPSILON) &&
             (closure < best_closure)))
        {
            found = true;
            best_score = score;
            best_closure = closure;
            memcpy(best_selection, selected, sizeof(best_selection));
        }
    }

    if (!found)
    {
        return false;
    }

    apply_candidate_selection(out, best_selection);
    out->solver_selected_score = best_score;
    out->solver_score_ratio = best_score / out->solver_raw_score;

    for (uint32_t pair = 0U; pair < TOA_PAIR_COUNT; pair++)
    {
        if (best_selection[pair] != 0U)
        {
            out->solver_used = 1U;
            break;
        }
    }

    return true;
}


static void compute_measurement_confidence(ToaResult *out)
{
    if (out == NULL)
    {
        return;
    }

    out->confidence = TOA_CONFIDENCE_NONE;
    out->track_eligible = 0U;
    out->confidence_ambiguous_pairs = 0U;
    out->confidence_gap_pair_count = 0U;
    out->confidence_peak_gap_min = 0.0f;
    out->confidence_selected_corr_min = 0.0f;
    out->confidence_selected_corr_mean = 0.0f;

    if (out->valid == 0U)
    {
        return;
    }

    float selected_corr_sum = 0.0f;
    float selected_corr_min = 2.0f;
    float gap_min = 2.0f;

    for (uint32_t pair = 0U; pair < TOA_PAIR_COUNT; pair++)
    {
        const float selected_corr = out->pair[pair].correlation;
        selected_corr_sum += selected_corr;

        if (selected_corr < selected_corr_min)
        {
            selected_corr_min = selected_corr;
        }

        float best = -2.0f;
        float second = -2.0f;

        for (uint32_t candidate_index = 0U;
             candidate_index < TOA_PEAK_CANDIDATE_COUNT;
             candidate_index++)
        {
            const ToaPeakCandidate *candidate =
                &out->pair[pair].candidate[candidate_index];

            if ((candidate->valid == 0U) ||
                (candidate->peak_at_search_boundary != 0U))
            {
                continue;
            }

            const float corr = candidate->correlation;

            if (corr > best)
            {
                second = best;
                best = corr;
            }
            else if (corr > second)
            {
                second = corr;
            }
        }

        if (second > -1.5f)
        {
            const float gap = best - second;
            out->confidence_gap_pair_count++;

            if (gap < gap_min)
            {
                gap_min = gap;
            }

            if (gap <= TOA_CONF_AMBIGUOUS_GAP_MAX)
            {
                out->confidence_ambiguous_pairs++;
            }
        }
    }

    out->confidence_selected_corr_min = selected_corr_min;
    out->confidence_selected_corr_mean =
        selected_corr_sum / (float)TOA_PAIR_COUNT;

    if (out->confidence_gap_pair_count != 0U)
    {
        out->confidence_peak_gap_min = gap_min;
    }

    if (out->confidence_ambiguous_pairs >= 2U)
    {
        out->confidence = TOA_CONFIDENCE_LOW;
        out->track_eligible = 0U;
        return;
    }

    if ((out->raw_valid != 0U) &&
        (out->solver_used == 0U) &&
        (out->confidence_ambiguous_pairs == 0U) &&
        (out->closure_error_samples <= TOA_CONF_HIGH_CLOSURE_MAX))
    {
        out->confidence = TOA_CONFIDENCE_HIGH;
        out->track_eligible = 1U;
        return;
    }

    out->confidence = TOA_CONFIDENCE_MEDIUM;
    out->track_eligible = 1U;
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
            TOA_MAX_LAG_TETRA,
            &out->pair[TOA_PAIR_LEFT_TOP]) ||
        !estimate_pair(
            left,
            back,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_TETRA,
            &out->pair[TOA_PAIR_LEFT_BACK]) ||
        !estimate_pair(
            left,
            right,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_TETRA,
            &out->pair[TOA_PAIR_LEFT_RIGHT]) ||
        !estimate_pair(
            top,
            back,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_TETRA,
            &out->pair[TOA_PAIR_TOP_BACK]) ||
        !estimate_pair(
            top,
            right,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_TETRA,
            &out->pair[TOA_PAIR_TOP_RIGHT]) ||
        !estimate_pair(
            back,
            right,
            TOA_WINDOW_SAMPLE_COUNT,
            TOA_MAX_LAG_TETRA,
            &out->pair[TOA_PAIR_BACK_RIGHT]))
    {
        return false;
    }

    for (uint32_t pair = 0U; pair < TOA_PAIR_COUNT; pair++)
    {
        out->selected_candidate[pair] = 1U;
    }

    out->solver_raw_score = p1_score(out);
    out->solver_selected_score = out->solver_raw_score;
    out->solver_score_ratio = 1.0f;

    compute_selected_geometry(out);

    out->raw_closure_error_samples = out->closure_error_samples;
    out->raw_invalid_flags = out->invalid_flags;
    out->raw_valid = out->valid;

    if (out->raw_valid == 0U)
    {
        if (solve_global_candidates(out))
        {
            compute_selected_geometry(out);
        }
    }

    out->solver_recovered =
        ((out->raw_valid == 0U) && (out->valid != 0U)) ? 1U : 0U;

    compute_measurement_confidence(out);

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


const char *toa_confidence_text(ToaMeasurementConfidence confidence)
{
    switch (confidence)
    {
        case TOA_CONFIDENCE_LOW:
            return "LOW";
        case TOA_CONFIDENCE_MEDIUM:
            return "MEDIUM";
        case TOA_CONFIDENCE_HIGH:
            return "HIGH";
        default:
            return "NONE";
    }
}
