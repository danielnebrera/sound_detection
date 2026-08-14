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
