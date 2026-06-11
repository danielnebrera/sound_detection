#ifndef MFCC_STM32_H
#define MFCC_STM32_H

#include <stdbool.h>
#include <stdint.h>

/* =========================================================
 * MFCC compatible con Librosa para STM32H747 (CM7)
 * Parámetros idénticos al proyecto ESP32 original:
 *   SR=44100, N_FFT=2048, HOP=442, N_MELS=64, N_MFCC=20
 *   Ventana Hann, power_to_db(ref=1.0, top_db=80)
 *   DCT-II ortho → 100 frames × 20 coeficientes
 * ========================================================= */

#define MFCC_SAMPLE_RATE    44100
#define MFCC_N_FFT          2048
#define MFCC_HOP            442
#define MFCC_N_MELS         64
#define MFCC_N_MFCC         20
#define MFCC_TARGET_FRAMES  100

#ifdef __cplusplus
extern "C" {
#endif

/* Contenedor de salida: 100 frames × 20 coeficientes */
typedef struct {
    float data[MFCC_TARGET_FRAMES * MFCC_N_MFCC];  /* 8000 floats = 32 KB */
} mfcc_100x20_t;

/**
 * Inicializa la ventana Hann y la instancia RFFT de CMSIS-DSP.
 * Llamar UNA SOLA VEZ al arrancar. Retorna false si falta RAM.
 */
bool mfcc_stm32_init(void);

/**
 * Calcula la matriz MFCC [100×20] a partir de 1 segundo de audio.
 *
 * @param pcm_1s   Buffer de 44100 floats normalizados [-1.0, 1.0]
 *                 con el procesamiento DSP ya aplicado
 *                 (HPF α=0.9 + ganancia×15 + tanh)
 * @param out      Estructura de salida con 100×20 floats
 * @return         true si OK, false si error de memoria
 */
bool mfcc_stm32_compute(const float *pcm_1s, mfcc_100x20_t *out);

/**
 * Aplica el mismo preprocesado DSP que audio_i2s.c del ESP32:
 *   1. Filtro paso alto  (α = 0.9)
 *   2. Ganancia ×15
 *   3. Soft clipping tanh()
 *
 * @param samples  Buffer in-place de N floats
 * @param n        Número de muestras
 */
void mfcc_stm32_apply_dsp(float *samples, int n);

#ifdef __cplusplus
}
#endif

#endif /* MFCC_STM32_H */