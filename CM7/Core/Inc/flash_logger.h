/**
 * @file flash_logger.h
 * @brief Logging system usando SRAM de backup (sobrevive reset sin perder datos)
 *
 * Los logs se guardan en SRAM4 (0x38800000) que mantiene contenido
 * mientras haya energía. Se leen con STM32CubeProgrammer.
 *
 * Ubicación: 0x38800000 (SRAM4 - Backup SRAM, 16KB)
 * Tamaño máximo: 4KB de logs
 */

#ifndef FLASH_LOGGER_H
#define FLASH_LOGGER_H

#include <stdint.h>
#include <stdio.h>

/* Dirección de SRAM4 (backup RAM - sobrevive reset) */
#define FLASH_LOG_BASE  0x10000200UL  /* DTCMRAM del CM4 declarada en su linker script, dejando los primeros 512 bytes para el stack.  */
#define FLASH_LOG_SIZE  4096          /* 4KB para logs */
#define FLASH_LOG_MAGIC 0xDEADBEEF   /* Magic number para validar contenido */

void flash_logger_init(void);
void flash_log(const char *format, ...);
const char* flash_logger_get_buffer(void);
void flash_logger_clear(void);
uint32_t flash_logger_get_size(void);

#endif /* FLASH_LOGGER_H */
