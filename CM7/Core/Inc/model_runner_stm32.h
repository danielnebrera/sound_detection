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

#ifdef __cplusplus
}
#endif

#endif /* MODEL_RUNNER_STM32_H */