/**
 * @file flash_logger.c
 * @brief Logging en SRAM4 (backup RAM) - sobrevive reset mientras haya energía
 *
 * Estructura en memoria (0x38800000):
 * [0x00-0x03] Magic number (0xDEADBEEF) - indica que hay datos válidos
 * [0x04-0x07] write_pos - cantidad de bytes escritos
 * [0x08-...] datos de log (texto ASCII)
 */

#include "flash_logger.h"
#include "stm32h7xx.h"
#include <stdarg.h>
#include <string.h>

/* Estructura en SRAM4 */
typedef struct {
    uint32_t magic;                      /* 0xDEADBEEF si hay datos válidos */
    uint32_t write_pos;                  /* Bytes escritos */
    char     data[FLASH_LOG_SIZE - 8];   /* Datos del log */
} __attribute__((packed)) FlashLogRegion;

/* Puntero directo a SRAM4 */
#define LOG_REGION  ((volatile FlashLogRegion *)FLASH_LOG_BASE)

/**
 * @brief Inicializar el sistema de logging
 * Si ya hay datos válidos (magic correcto), los preserva.
 * Si no, limpia y empieza desde cero.
 */
void flash_logger_init(void)
{
	/* Habilitar clock de D3 domain para SRAM4 */
	    RCC->AHB4ENR |= RCC_AHB4ENR_BDMAEN;

    if (LOG_REGION->magic != FLASH_LOG_MAGIC)
    {
        /* Primera vez o datos corruptos — inicializar */
        LOG_REGION->magic = FLASH_LOG_MAGIC;
        LOG_REGION->write_pos = 0;
        memset((void*)LOG_REGION->data, 0, sizeof(LOG_REGION->data));
    }
    /* Si magic es correcto, preserva los datos anteriores */
}

/**
 * @brief Escribir un mensaje al log
 */
void flash_log(const char *format, ...)
{
    if (LOG_REGION->magic != FLASH_LOG_MAGIC)
    {
        flash_logger_init();
    }

    uint32_t pos = LOG_REGION->write_pos;
    uint32_t available = sizeof(LOG_REGION->data) - pos;

    if (available < 10) return; /* Sin espacio */

    va_list args;
    va_start(args, format);

    char tmp[256];
    int written = vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);

    if (written <= 0) return;
    if ((uint32_t)written > available) written = available - 1;

    memcpy((void*)&LOG_REGION->data[pos], tmp, written);
    LOG_REGION->write_pos += written;
}

/**
 * @brief Obtener puntero al buffer de logs
 */
const char* flash_logger_get_buffer(void)
{
    return (const char*)LOG_REGION->data;
}

/**
 * @brief Limpiar logs
 */
void flash_logger_clear(void)
{
    LOG_REGION->magic = FLASH_LOG_MAGIC;
    LOG_REGION->write_pos = 0;
    memset((void*)LOG_REGION->data, 0, sizeof(LOG_REGION->data));
}

/**
 * @brief Obtener bytes escritos
 */
uint32_t flash_logger_get_size(void)
{
    if (LOG_REGION->magic != FLASH_LOG_MAGIC) return 0;
    return LOG_REGION->write_pos;
}

/**
 * @brief Redirect printf al logger
 */
int _write(int file, char *ptr, int len)
{
    (void)file;

    /* Solo escribir al ITM si el debugger lo habilitó */
    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0) return len;
    if ((ITM->TCR & ITM_TCR_ITMENA_Msk) == 0) return len;
    if ((ITM->TER & 1UL) == 0) return len;

    for (int i = 0; i < len; i++)
    {
        /* Timeout en lugar de loop infinito */
        uint32_t timeout = 10000;
        while (ITM->PORT[0].u32 == 0 && timeout-- > 0) {}
        if (timeout > 0)
            ITM->PORT[0].u8 = (uint8_t)ptr[i];
    }

    return len;
}
