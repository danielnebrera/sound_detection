#ifndef MODEL_RUNNER_STM32_H
#define MODEL_RUNNER_STM32_H

/* =================================================================
 * model_runner_stm32.h
 * Wrapper de inferencia TFLite Micro para STM32H747 (CM7)
 * Usa los archivos generados por X-CUBE-AI 10.2.0:
 *   network.h / network.c
 *   tflm_c.h  / tflm_c.c
 * ================================================================= */

#include <stdbool.h>
#include <stdint.h>

#define MODEL_RUNNER_TFLM_MAX_NODES 16U

typedef struct
{
    const char *name;
    uint32_t runs;
    uint64_t total_cycles;
    uint32_t last_cycles;
    uint32_t max_cycles;
} ModelRunnerTflmNodeProfile;

typedef struct
{
    uint32_t invokes;
    uint32_t node_count;
    ModelRunnerTflmNodeProfile nodes[MODEL_RUNNER_TFLM_MAX_NODES];
} ModelRunnerTflmProfile;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inicializa el modelo TFLite Micro.
 * Debe llamarse UNA SOLA VEZ antes de cualquier inferencia.
 * @return true si OK, false si error de arena o modelo corrupto
 */
bool model_runner_init(void);

/**
 * Ejecuta inferencia sobre una matriz MFCC [100×20×1].
 * @param mfcc_data  Puntero a 100*20 = 2000 floats (orden time-major)
 * @return           Probabilidad [0.0 - 1.0] de que sea un dron
 *                   Retorna -1.0f si el modelo no está inicializado
 */
float model_runner_infer(const float *mfcc_data);

void model_runner_tflm_profile_reset(void);
bool model_runner_tflm_profile_get(ModelRunnerTflmProfile *profile);

#ifdef __cplusplus
}
#endif

#endif /* MODEL_RUNNER_STM32_H */
