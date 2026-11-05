/**
 * @file uart_driver.h
 * @brief Simple UART driver for STM32H7
 * 
 * Configura USART3 a 115200 baud para logs
 * Pines: PD8 (TX), PD9 (RX)
 */

#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include "main.h"
#include <stdio.h>

/**
 * @brief Initialize USART3 at 115200 baud
 */
void uart_init(void);

/**
 * @brief Send a character via UART
 */
void uart_putchar(char c);

/**
 * @brief Send a string via UART
 */
void uart_puts(const char *str);

#endif /* UART_DRIVER_H */
