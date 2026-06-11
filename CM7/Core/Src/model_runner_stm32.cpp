/* =================================================================
 * model_runner_stm32.cpp
 * Inferencia TFLite Micro para STM32H747 (CM7)
 * API: X-CUBE-AI 10.2.0 — tflm_c.h v2.2
 *
 * Entrada:  tensor float32 [1, 100, 20, 1]
 * Salida:   sigmoid escalar → probabilidad dron [0.0-1.0]
 * Arena:    160 KB en RAM_D1
 * ================================================================= */

/* tflm_c.h necesita estos tipos antes de incluirse */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "tflm_c.h"
#include "network_tflite_data.h"   /* g_tflm_network_model_data[] */
#include "model_runner_stm32.h"

/* ── Arena de memoria ─────────────────────────────────────────── */
/* X-CUBE-AI reportó RAM: 143872 bytes. Ponemos 160 KB con margen. */
#define TENSOR_ARENA_SIZE  (160 * 1024)

__attribute__((aligned(16)))
static uint8_t s_tensor_arena[TENSOR_ARENA_SIZE];

/* ── Handle del modelo (uint32_t según tflm_c.h) ─────────────── */
static uint32_t s_hdl   = 0;
static bool     s_inited = false;

/* ── Inicialización ──────────────────────────────────────────── */
extern "C" bool model_runner_init(void)
{
    if (s_inited) return true;

    TfLiteStatus st = tflm_c_create(
        g_tflm_network_model_data,
        s_tensor_arena,
        (uint32_t)TENSOR_ARENA_SIZE,
        &s_hdl
    );

    if (st != kTfLiteOk) {
        printf("[MODEL] ERROR tflm_c_create: %d\r\n", (int)st);
        return false;
    }

    /* Verificar tensores */
    struct tflm_c_tensor_info t_in  = {kTfLiteNoType};
    struct tflm_c_tensor_info t_out = {kTfLiteNoType};
    tflm_c_input (s_hdl, 0, &t_in);
    tflm_c_output(s_hdl, 0, &t_out);

    s_inited = true;
    printf("[MODEL] Init OK — Arena %d KB usados: %ld B\r\n",
           TENSOR_ARENA_SIZE / 1024,
           (long)tflm_c_arena_used_bytes(s_hdl));
    printf("[MODEL] Input : %u bytes, tipo %d\r\n",
           (unsigned)t_in.bytes,  (int)t_in.type);
    printf("[MODEL] Output: %u bytes, tipo %d\r\n",
           (unsigned)t_out.bytes, (int)t_out.type);
    return true;
}

/* ── Inferencia ──────────────────────────────────────────────── */
extern "C" float model_runner_infer(const float *mfcc_data)
{
    if (!s_inited) {
        printf("[MODEL] ERROR: no inicializado\r\n");
        return -1.0f;
    }

    /* Obtener info del tensor de entrada para saber tamaño */
    struct tflm_c_tensor_info t_in = {kTfLiteNoType};
    tflm_c_input(s_hdl, 0, &t_in);

    /* Copiar MFCC al buffer de entrada del modelo */
    memcpy(t_in.data, mfcc_data, t_in.bytes);

    /* Ejecutar inferencia */
    TfLiteStatus st = tflm_c_invoke(s_hdl);
    if (st != kTfLiteOk) {
        printf("[MODEL] ERROR invoke: %d\r\n", (int)st);
        return -1.0f;
    }

    /* Leer salida sigmoid [0.0-1.0] */
    struct tflm_c_tensor_info t_out = {kTfLiteNoType};
    tflm_c_output(s_hdl, 0, &t_out);

    float p_drone   = ((float *)t_out.data)[0];
    float p_nodrone = 1.0f - p_drone;

    printf("[MODEL] NoDrone=%.4f Drone=%.4f\r\n", p_nodrone, p_drone);
    return p_drone;
}
