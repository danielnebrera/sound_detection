/* =================================================================
 * model_runner_stm32.cpp
 * Inferencia TFLite Micro para STM32H747 (CM7).
 *
 * Esta version mantiene la arena y la API existentes, pero elimina el
 * printf por inferencia para no interferir con la captura concurrente.
 * ================================================================= */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "tflm_c.h"
#include "network_tflite_data.h"
#include "model_runner_stm32.h"
#include "stm32h7xx.h"

#ifndef MODEL_RUNNER_VERBOSE_LOGS
#define MODEL_RUNNER_VERBOSE_LOGS 0
#endif

#if MODEL_RUNNER_VERBOSE_LOGS
#define MODEL_LOG(...) printf(__VA_ARGS__)
#else
#define MODEL_LOG(...) ((void)0)
#endif

#define TENSOR_ARENA_SIZE  (160 * 1024)

__attribute__((section(".RAM_D1_model"), aligned(32)))
static uint8_t s_tensor_arena[TENSOR_ARENA_SIZE];

static uint32_t s_hdl = 0U;
static bool s_inited = false;
static ModelRunnerTflmProfile s_tflm_profile = {};
static uint32_t s_time_last_low = 0U;
static uint64_t s_time_high = 0ULL;

static uint64_t model_runner_profile_time(int mode)
{
    (void)mode;

    const uint32_t now = DWT->CYCCNT;

    if (now < s_time_last_low)
    {
        s_time_high += (1ULL << 32);
    }

    s_time_last_low = now;
    return s_time_high | (uint64_t)now;
}

static int model_runner_profile_notify(
    const void *cookie,
    const uint32_t flags,
    const struct tflm_c_node *node)
{
    (void)cookie;
    (void)flags;

    if (node == nullptr)
    {
        return 0;
    }

    const uint32_t idx = node->node_info.idx;

    if (idx >= MODEL_RUNNER_TFLM_MAX_NODES)
    {
        return 0;
    }

    ModelRunnerTflmNodeProfile *entry = &s_tflm_profile.nodes[idx];
    const uint32_t duration = (uint32_t)node->node_info.dur;

    entry->name = node->node_info.name;
    entry->runs++;
    entry->total_cycles += node->node_info.dur;
    entry->last_cycles = duration;

    if (duration > entry->max_cycles)
    {
        entry->max_cycles = duration;
    }

    if ((idx + 1U) > s_tflm_profile.node_count)
    {
        s_tflm_profile.node_count = idx + 1U;
    }

    return 0;
}

static struct tflm_c_observer_options s_observer_options =
{
    model_runner_profile_notify,
    model_runner_profile_time,
    nullptr,
    OBSERVER_FLAGS_TIME_ONLY
};

extern "C" bool model_runner_init(void)
{
    if (s_inited)
    {
        return true;
    }

    memset(s_tensor_arena, 0, sizeof(s_tensor_arena));

    const TfLiteStatus status = tflm_c_create(
        g_tflm_network_model_data,
        s_tensor_arena,
        (uint32_t)TENSOR_ARENA_SIZE,
        &s_hdl
    );

    if (status != kTfLiteOk)
    {
        printf("[MODEL] ERROR tflm_c_create: %d\r\n", (int)status);
        return false;
    }

    struct tflm_c_tensor_info input_info = {};
    struct tflm_c_tensor_info output_info = {};
    input_info.type = kTfLiteNoType;
    output_info.type = kTfLiteNoType;
    tflm_c_input(s_hdl, 0, &input_info);
    tflm_c_output(s_hdl, 0, &output_info);

    memset(&s_tflm_profile, 0, sizeof(s_tflm_profile));
    s_time_last_low = DWT->CYCCNT;
    s_time_high = 0ULL;

    if (tflm_c_observer_register(s_hdl, &s_observer_options) != kTfLiteOk)
    {
        printf("[MODEL] ERROR observer register\r\n");
        return false;
    }

    if (tflm_c_observer_start(s_hdl) != kTfLiteOk)
    {
        printf("[MODEL] ERROR observer start\r\n");
        return false;
    }

    s_inited = true;

    MODEL_LOG(
        "[MODEL] Init OK - Arena %d KB usados: %ld B\r\n",
        TENSOR_ARENA_SIZE / 1024,
        (long)tflm_c_arena_used_bytes(s_hdl)
    );
    MODEL_LOG(
        "[MODEL] Input: %u bytes, tipo %d\r\n",
        (unsigned)input_info.bytes,
        (int)input_info.type
    );
    MODEL_LOG(
        "[MODEL] Output: %u bytes, tipo %d\r\n",
        (unsigned)output_info.bytes,
        (int)output_info.type
    );

    return true;
}

extern "C" float model_runner_infer(const float *mfcc_data)
{
    if (!s_inited || (mfcc_data == nullptr))
    {
        printf("[MODEL] ERROR: no inicializado o entrada nula\r\n");
        return -1.0f;
    }

    struct tflm_c_tensor_info input_info = {};
    input_info.type = kTfLiteNoType;
    tflm_c_input(s_hdl, 0, &input_info);
    memcpy(input_info.data, mfcc_data, input_info.bytes);

    const TfLiteStatus status = tflm_c_invoke(s_hdl);
    if (status != kTfLiteOk)
    {
        printf("[MODEL] ERROR invoke: %d\r\n", (int)status);
        return -1.0f;
    }

    s_tflm_profile.invokes++;

    struct tflm_c_tensor_info output_info = {};
    output_info.type = kTfLiteNoType;
    tflm_c_output(s_hdl, 0, &output_info);

    const float p_drone = ((float *)output_info.data)[0];
    MODEL_LOG(
        "[MODEL] NoDrone=%.4f Drone=%.4f\r\n",
        (double)(1.0f - p_drone),
        (double)p_drone
    );

    return p_drone;
}

extern "C" void model_runner_tflm_profile_reset(void)
{
    memset(&s_tflm_profile, 0, sizeof(s_tflm_profile));
    s_time_last_low = DWT->CYCCNT;
    s_time_high = 0ULL;

    if (s_inited)
    {
        (void)tflm_c_observer_start(s_hdl);
    }
}

extern "C" bool model_runner_tflm_profile_get(ModelRunnerTflmProfile *profile)
{
    if (!s_inited || (profile == nullptr))
    {
        return false;
    }

    *profile = s_tflm_profile;
    return true;
}
