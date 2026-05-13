#include "uart_driver.h"
#include "core_cm7.h"

void uart_init(void)
{
    /* ITM se inicializa automáticamente en HAL_Init() */
}

void uart_putchar(char c)
{
    ITM_SendChar((uint32_t)c);
}

void uart_puts(const char *str)
{
    if (!str) return;
    while (*str)
    {
        uart_putchar(*str++);
    }
}

int __io_putchar(int ch)
{
    uart_putchar((char)ch);
    return ch;
}
