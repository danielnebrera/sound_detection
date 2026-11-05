/**
 * @file gpio_bus_capture.h
 * @brief Captura diagnostica directa de WS, SD_A y SD_B leyendo GPIOx->IDR.
 *
 * Los pines permanecen configurados en Alternate Function SAI2.
 * NO se reconfiguran como GPIO de entrada, porque eso detendria SCK/WS.
 */

#ifndef GPIO_BUS_CAPTURE_H
#define GPIO_BUS_CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Captura el bus durante unos milisegundos, detiene SAI/DMA, imprime
 * el resultado por UART y queda detenido para evitar que arranque el detector.
 *
 * Debe llamarse despues de audio_capture_start().
 * Esta funcion no retorna.
 */
__attribute__((noreturn))
void gpio_bus_capture_run_and_halt(void);

#ifdef __cplusplus
}
#endif

#endif /* GPIO_BUS_CAPTURE_H */