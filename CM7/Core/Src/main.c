/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body with audio capture on CM7
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "dma.h"
#include "sai.h"
#include "gpio.h"

/* USER CODE BEGIN Includes */
#include "audio_capture.h"
#include "uart_driver.h"
#include "flash_logger.h"
#include "stm32h7xx.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PD */
//#define DUAL_CORE_BOOT_SYNC_SEQUENCE
/* USER CODE END PD */

void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);
void audio_test_print_stats(void);

/* LEDs con logica invertida */
#define LED_ON(pin)     HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_RESET)
#define LED_OFF(pin)    HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_SET)
#define LED_TOGGLE(pin) HAL_GPIO_TogglePin(GPIOK, pin)

int main(void)
{
  MPU_Config();

  HAL_Init();
  HAL_RCC_DeInit();
  SystemClock_Config();
  PeriphCommonClock_Config();

  MX_GPIO_Init();
  MX_DMA_Init();

  /* Apagar todos los LEDs */
  LED_OFF(GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7);

  flash_logger_init();
  flash_logger_clear();
  flash_log("CM7: arranco OK\n");

  uart_init();
  MX_SAI1_Init();
  flash_log("CM7: SAI OK\n");

  if (audio_capture_init(&g_audio_ctx) != 0)
  {
      flash_log("CM7: ERROR audio_capture_init\n");
      while(1) { LED_TOGGLE(GPIO_PIN_7); HAL_Delay(500); }
  }
  flash_log("CM7: audio_capture_init OK\n");

  int result = audio_capture_start(&g_audio_ctx);
  if (result != 0)
  {
      flash_log("CM7: ERROR audio_capture_start\n");
      if (result == -1)
          while(1) { LED_TOGGLE(GPIO_PIN_5); HAL_Delay(500); } /* Rojo = SAI_A */
      else
          while(1) { LED_TOGGLE(GPIO_PIN_7); HAL_Delay(500); } /* Azul = SAI_B */
  }
  flash_log("CM7: audio_capture_start OK\n");

  LED_OFF(GPIO_PIN_5);
  LED_OFF(GPIO_PIN_6);
  LED_OFF(GPIO_PIN_7);

  uint32_t sample_count = 0;

  while (1)
  {
      AudioBufferState state = audio_capture_get_data(&g_audio_ctx);

      if (state == AUDIO_BUFFER_HALF || state == AUDIO_BUFFER_FULL)
      {
          sample_count += AUDIO_BUFFER_SIZE;
          LED_TOGGLE(GPIO_PIN_5); /* Rojo parpadea cada buffer */

          if (sample_count >= AUDIO_SAMPLE_RATE)
          {
              sample_count = 0;
              LED_TOGGLE(GPIO_PIN_6); /* Verde parpadea cada segundo */
              audio_test_print_stats();
          }
      }
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) return;

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK) return;
}

void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SAI1;
  PeriphClkInitStruct.PLL3.PLL3M = 5;
  PeriphClkInitStruct.PLL3.PLL3N = 72;
  PeriphClkInitStruct.PLL3.PLL3P = 32;
  PeriphClkInitStruct.PLL3.PLL3Q = 2;
  PeriphClkInitStruct.PLL3.PLL3R = 2;
  PeriphClkInitStruct.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  PeriphClkInitStruct.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  PeriphClkInitStruct.PLL3.PLL3FRACN = 2077;
  PeriphClkInitStruct.Sai1ClockSelection = RCC_SAI1CLKSOURCE_PLL3;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
      while(1) { LED_TOGGLE(GPIO_PIN_6); HAL_Delay(100); }
  }
}

void audio_test_print_stats(void)
{
    uint32_t frames = 0, errors = 0;
    audio_capture_get_stats(&g_audio_ctx, &frames, &errors);
    printf("[AUDIO] Frames: %lu | Errors: %lu\n", frames, errors);

    int32_t *ch0 = audio_capture_get_channel(&g_audio_ctx, 0);
    int32_t *ch1 = audio_capture_get_channel(&g_audio_ctx, 1);
    int32_t *ch2 = audio_capture_get_channel(&g_audio_ctx, 2);
    int32_t *ch3 = audio_capture_get_channel(&g_audio_ctx, 3);

    if (ch0 && ch1 && ch2 && ch3)
    {
        printf("  Mic1[0]: %ld | Mic2[0]: %ld | Mic3[0]: %ld | Mic4[0]: %ld\n",
               ch0[0], ch1[0], ch2[0], ch3[0]);
    }
}

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};
  HAL_MPU_Disable();
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

void Error_Handler(void)
{
  while(1)
  {
      LED_TOGGLE(GPIO_PIN_5);
      LED_OFF(GPIO_PIN_7);
      HAL_Delay(500);
      LED_OFF(GPIO_PIN_5);
      LED_TOGGLE(GPIO_PIN_7);
      HAL_Delay(500);
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
