/**
 * @file flash_logger.c
 * @brief Logging en SRAM4 (backup RAM) - sobrevive reset mientras haya energía
 */

#include "flash_logger.h"
#include "stm32h7xx.h"
#include "usart.h"
#include <stdarg.h>
#include <string.h>

typedef struct {
    uint32_t magic;
    uint32_t write_pos;
    char     data[FLASH_LOG_SIZE - 8];
} __attribute__((packed)) FlashLogRegion;

#define LOG_REGION  ((volatile FlashLogRegion *)FLASH_LOG_BASE)

void flash_logger_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_BDMAEN;
    if (LOG_REGION->magic != FLASH_LOG_MAGIC)
    {
        LOG_REGION->magic = FLASH_LOG_MAGIC;
        LOG_REGION->write_pos = 0;
        memset((void*)LOG_REGION->data, 0, sizeof(LOG_REGION->data));
    }
}

void flash_log(const char *format, ...)
{
    if (LOG_REGION->magic != FLASH_LOG_MAGIC) flash_logger_init();

    uint32_t pos = LOG_REGION->write_pos;
    uint32_t available = sizeof(LOG_REGION->data) - pos;
    if (available < 10) return;

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

const char* flash_logger_get_buffer(void)
{
    return (const char*)LOG_REGION->data;
}

void flash_logger_clear(void)
{
    LOG_REGION->magic = FLASH_LOG_MAGIC;
    LOG_REGION->write_pos = 0;
    memset((void*)LOG_REGION->data, 0, sizeof(LOG_REGION->data));
}

uint32_t flash_logger_get_size(void)
{
    if (LOG_REGION->magic != FLASH_LOG_MAGIC) return 0;
    return LOG_REGION->write_pos;
}

/**
 * @brief Redirect printf a UART1 (FT232RL COM6)
 * PA9=TX, 460800 8N1, clock HSI 64MHz, timeout 20ms
 */
int _write(int file, char *ptr, int len)
{
    (void)file;
    HAL_UART_Transmit(&huart1, (uint8_t*)ptr, len, 20);
    return len;
}
