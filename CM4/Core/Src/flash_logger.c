/**
 * @file flash_logger.c
 * @brief Flash memory logging implementation
 */

#include "flash_logger.h"
#include "main.h"
#include <stdarg.h>
#include <string.h>

/* Flash logger state */
typedef struct {
    uint32_t write_pos;      /* Posición actual en el buffer */
    uint8_t buffer[FLASH_LOG_SIZE];  /* Buffer en RAM */
    uint8_t initialized;
} FlashLogger;

static FlashLogger logger = {0};

/**
 * @brief Inicializar el sistema de logging
 */
void flash_logger_init(void)
{
    if (logger.initialized) return;

    memset(logger.buffer, 0, FLASH_LOG_SIZE);
    logger.write_pos = 0;
    logger.initialized = 1;
}

/**
 * @brief Escribir un mensaje al log (en RAM)
 */
void flash_log(const char *format, ...)
{
    if (!logger.initialized) flash_logger_init();

    if (logger.write_pos >= FLASH_LOG_SIZE - 100)
    {
        return;  /* Buffer lleno */
    }

    va_list args;
    va_start(args, format);

    /* Escribir en el buffer */
    int written = vsnprintf(
        (char *)&logger.buffer[logger.write_pos],
        FLASH_LOG_SIZE - logger.write_pos,
        format,
        args
    );

    va_end(args);

    if (written > 0)
    {
        logger.write_pos += written;
    }
}

/**
 * @brief Obtener el buffer con los logs
 */
const char* flash_logger_get_buffer(void)
{
    return (const char*)logger.buffer;
}

/**
 * @brief Limpiar los logs
 */
void flash_logger_clear(void)
{
    memset(logger.buffer, 0, FLASH_LOG_SIZE);
    logger.write_pos = 0;
}

/**
 * @brief Obtener cantidad de bytes guardados
 */
uint32_t flash_logger_get_size(void)
{
    return logger.write_pos;
}

/**
 * @brief Redirect printf a los logs
 */
int _write(int file, char *ptr, int len)
{
    (void)file;

    /* Escribir en el log */
    for (int i = 0; i < len; i++)
    {
        if (logger.write_pos < FLASH_LOG_SIZE - 1)
        {
            logger.buffer[logger.write_pos++] = ptr[i];
        }
    }

    return len;
}
