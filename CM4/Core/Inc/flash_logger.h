/**
 * @file flash_logger.h
 * @brief Logging system that saves to flash memory
 *
 * Los logs se guardan en memoria flash y se pueden leer
 * usando STM32CubeProgrammer.
 *
 * Ubicación en flash: 0x081F0000 (última página de 128KB)
 * Tamaño máximo: 4KB de logs
 */

#ifndef FLASH_LOGGER_H
#define FLASH_LOGGER_H

#include <stdint.h>
#include <stdio.h>

/* Direcciones de flash para logging */
#define FLASH_LOG_BASE  0x081F0000UL  /* Última página de 128KB */
#define FLASH_LOG_SIZE  4096          /* 4KB para logs */

/**
 * @brief Inicializar el sistema de logging
 */
void flash_logger_init(void);

/**
 * @brief Escribir un mensaje al log
 *
 * @param format Formato similar a printf
 */
void flash_log(const char *format, ...);

/**
 * @brief Leer todos los logs guardados (para debugging)
 *
 * @return Puntero a buffer con los logs
 */
const char* flash_logger_get_buffer(void);

/**
 * @brief Limpiar el buffer de logs
 */
void flash_logger_clear(void);

/**
 * @brief Obtener cantidad de logs guardados (en bytes)
 */
uint32_t flash_logger_get_size(void);

#endif /* FLASH_LOGGER_H */
