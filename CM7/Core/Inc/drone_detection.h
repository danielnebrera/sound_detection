/* =================================================================
 * drone_detection.h
 * Lógica de detección de drones para main.c del Portenta H7
 *
 * Pipeline completo por canal:
 *   int32 DMA → float normalizado → DSP → MFCC → Inferencia
 *   → EMA asimétrica → ALERTA ROJA / NARANJA
 * ================================================================= */
#include <stdbool.h>
#ifndef DRONE_DETECTION_H
#define DRONE_DETECTION_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inicializar el sistema de detección.
 * Llamar una sola vez en main() después de audio_capture_init().
 * @return true si MFCC y modelo iniciaron correctamente
 */
bool drone_detection_init(void);

/**
 * Procesar un frame de audio de los 4 micrófonos.
 * Llamar cada vez que haya un segundo completo de audio disponible.
 * Imprime por UART el resultado de cada canal y la alerta final.
 */
void drone_detection_process(void);

#ifdef __cplusplus
}
#endif

#endif /* DRONE_DETECTION_H */
