/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "stm32h7xx_it.h"

extern DMA_HandleTypeDef hdma_sai2_a;
extern DMA_HandleTypeDef hdma_sai2_b;
extern UART_HandleTypeDef huart1;

volatile uint32_t dma_irq_count = 0U;

void NMI_Handler(void)
{
  while (1) {}
}

void HardFault_Handler(void)
{
  while (1) {}
}

void MemManage_Handler(void)
{
  while (1) {}
}

void BusFault_Handler(void)
{
  while (1) {}
}

void UsageFault_Handler(void)
{
  while (1) {}
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
  HAL_IncTick();
}

void DMA1_Stream0_IRQHandler(void)
{
  dma_irq_count++;
  HAL_DMA_IRQHandler(&hdma_sai2_a);
}

void DMA1_Stream1_IRQHandler(void)
{
  dma_irq_count++;
  HAL_DMA_IRQHandler(&hdma_sai2_b);
}

void USART1_IRQHandler(void)
{
  HAL_UART_IRQHandler(&huart1);
}
