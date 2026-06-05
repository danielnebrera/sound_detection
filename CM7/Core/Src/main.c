
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body with audio capture on CM7
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "sai.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "audio_capture.h"
#include "flash_logger.h"
#include "stm32h7xx.h"
#include <string.h>
extern uint32_t dma_buf_a[];
extern uint32_t dma_buf_b[];
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
//#define DUAL_CORE_BOOT_SYNC_SEQUENCE
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables -----------------------------------------------------------*/
/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* LEDs con logica invertida - Portenta H7 */
#define LED_ON(pin)     HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_RESET)
#define LED_OFF(pin)    HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_SET)
#define LED_TOGGLE(pin) HAL_GPIO_TogglePin(GPIOK, pin)

/* USER CODE END 0 */

int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

/* USER CODE BEGIN Boot_Mode_Sequence_0 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
  int32_t timeout;
#endif
/* USER CODE END Boot_Mode_Sequence_0 */

  MPU_Config();

/* USER CODE BEGIN Boot_Mode_Sequence_1 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
  timeout = 0xFFFF;
  while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) && (timeout-- > 0));
  if (timeout < 0) { Error_Handler(); }
#endif
/* USER CODE END Boot_Mode_Sequence_1 */

  HAL_Init();

  /* USER CODE BEGIN Init */
  MX_USART1_UART_Init();
  HAL_UART_Transmit(&huart1, (uint8_t*)"UART_INIT_OK\r\n", 14, 100);
  HAL_UART_Transmit(&huart1, (uint8_t*)"BOOT\r\n", 6, 100);
  /* USER CODE END Init */

  SystemClock_Config();
  PeriphCommonClock_Config();

/* USER CODE BEGIN Boot_Mode_Sequence_2 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
  __HAL_RCC_HSEM_CLK_ENABLE();
  HAL_HSEM_FastTake(HSEM_ID_0);
  HAL_HSEM_Release(HSEM_ID_0, 0);
  timeout = 0xFFFF;
  while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) == RESET) && (timeout-- > 0));
  if (timeout < 0) { Error_Handler(); }
#endif
/* USER CODE END Boot_Mode_Sequence_2 */

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  MX_GPIO_Init();
  MX_DMA_Init();
  MX_SAI2_Init();
  MX_USART1_UART_Init();
  MX_I2C1_Init();

  /* USER CODE BEGIN 2 */
  LED_OFF(GPIO_PIN_5);
  LED_OFF(GPIO_PIN_6);
  LED_OFF(GPIO_PIN_7);

  /* --- ENCENDIDO PMIC LDO2 (3.3V para micrófonos en Breakout Board) --- */
  {
    uint8_t pmic_addr    = 0x08 << 1;          /* 0x10 write address */
    uint8_t set_volt[2]   = {0x51, 0x0F};       /* LDO2_VOLT: 3.3V   */
    uint8_t enable_ldo[2] = {0x4F, 0x0F};       /* LDO2_CTRL: enable */

    HAL_StatusTypeDef r1 = HAL_I2C_Master_Transmit(&hi2c1, pmic_addr, set_volt,   2, 100);
    HAL_StatusTypeDef r2 = HAL_I2C_Master_Transmit(&hi2c1, pmic_addr, enable_ldo, 2, 100);
    HAL_Delay(50);

    if (r1 == HAL_OK && r2 == HAL_OK)
      printf("PMIC: LDO2 ON OK\r\n");
    else
      printf("PMIC: LDO2 ERROR r1=%d r2=%d\r\n", (int)r1, (int)r2);
  }
  /* -------------------------------------------------------------------- */

  printf("CM7: arranque OK\r\n");

  if (audio_capture_init(&g_audio_ctx) != 0)
  {
    printf("CM7: ERROR audio_capture_init\r\n");
    while(1) { LED_TOGGLE(GPIO_PIN_7); HAL_Delay(500); }
  }
  printf("CM7: audio_capture_init OK\r\n");

  int result = audio_capture_start(&g_audio_ctx);
  if (result != 0)
  {
    printf("CM7: ERROR audio_capture_start result=%d\r\n", result);
    while(1) { LED_TOGGLE(GPIO_PIN_5); HAL_Delay(200); }
  }
  printf("CM7: audio_capture_start OK\r\n");

  HAL_Delay(100);
  if (hsai_BlockA2.State == HAL_SAI_STATE_BUSY_RX)
    printf("CM7: SAI BUSY_RX OK\r\n");
  else
    printf("CM7: SAI NO esta en BUSY_RX\r\n");

  LED_OFF(GPIO_PIN_5);
  LED_OFF(GPIO_PIN_6);
  LED_OFF(GPIO_PIN_7);
  printf("CM7: entrando al loop\r\n");
  /* USER CODE END 2 */

  /* USER CODE BEGIN WHILE */
  uint32_t sample_count = 0;
  while (1)
  {
    /* USER CODE END WHILE */
    /* USER CODE BEGIN 3 */

	  AudioBufferState state = audio_capture_get_data(&g_audio_ctx);

	  if (state == AUDIO_BUFFER_HALF || state == AUDIO_BUFFER_FULL)
	  {
	    sample_count += AUDIO_BUFFER_SIZE;
	    LED_TOGGLE(GPIO_PIN_5);

	    if (sample_count >= AUDIO_SAMPLE_RATE)
	    {
	      sample_count = 0;
	      LED_TOGGLE(GPIO_PIN_6);

	      uint32_t frames = 0, errors = 0;
	      audio_capture_get_stats(&g_audio_ctx, &frames, &errors);
	      printf("[AUDIO] frames=%lu errors=%lu\r\n", frames, errors);

	      int32_t *ch0 = audio_capture_get_channel(&g_audio_ctx, 0);
	      int32_t *ch1 = audio_capture_get_channel(&g_audio_ctx, 1);
	      int32_t *ch2 = audio_capture_get_channel(&g_audio_ctx, 2);
	      int32_t *ch3 = audio_capture_get_channel(&g_audio_ctx, 3);

	      if (ch0 && ch1 && ch2 && ch3)
	      {
	        printf("  Mic1=%ld Mic2=%ld Mic3=%ld Mic4=%ld\r\n",
	               ch0[0], ch1[0], ch2[0], ch3[0]);
	      }
	    }
	  }

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /* USER CODE BEGIN SystemClock_OSC_Enable */
  /* Habilitar oscilador externo 25MHz (OSCEN en PH1) */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOH, &GPIO_InitStruct);
  HAL_GPIO_WritePin(GPIOH, GPIO_PIN_1, GPIO_PIN_SET);
  for(volatile uint32_t i = 0; i < 500000; i++) {}
  /* USER CODE END SystemClock_OSC_Enable */

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 160;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) { Error_Handler(); }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2
                              | RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK) { Error_Handler(); }
}

/**
  * @brief Peripherals Common Clock Configuration
  */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SAI23;
  PeriphClkInitStruct.PLL3.PLL3M = 5;
  PeriphClkInitStruct.PLL3.PLL3N = 72;
  PeriphClkInitStruct.PLL3.PLL3P = 32;
  PeriphClkInitStruct.PLL3.PLL3Q = 2;
  PeriphClkInitStruct.PLL3.PLL3R = 2;
  PeriphClkInitStruct.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  PeriphClkInitStruct.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  PeriphClkInitStruct.PLL3.PLL3FRACN = 2077;
  PeriphClkInitStruct.Sai23ClockSelection = RCC_SAI23CLKSOURCE_PLL3;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) { Error_Handler(); }
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};
  HAL_MPU_Disable();

  /* Region 0: bloquear todo el espacio de direcciones por defecto */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress      = 0x00000000;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* USER CODE BEGIN MPU_Config_Extra */

  /* Region 1: RAM_D1 (0x24000000, 512KB) - cache habilitada */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress      = 0x24000000;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* Region 2: DTCMRAM (0x20000000, 128KB) */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress      = 0x20000000;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_128KB;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* Region 3: FLASH CM7 (0x08040000, 1MB) - ejecutable */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER3;
  MPU_InitStruct.BaseAddress      = 0x08040000;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_1MB;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* Region 4: RAM_D2 (0x30000000, 256KB) - buffers DMA, sin cache */
  MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
  MPU_InitStruct.Number           = MPU_REGION_NUMBER4;
  MPU_InitStruct.BaseAddress      = 0x30000000;
  MPU_InitStruct.Size             = MPU_REGION_SIZE_256KB;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable      = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* USER CODE END MPU_Config_Extra */

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

/**
  * @brief  This function is executed in case of error occurrence.
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while(1)
  {
    HAL_GPIO_TogglePin(GPIOK, GPIO_PIN_5);
    for(volatile uint32_t i = 0; i < 1000000; i++) {}
    HAL_GPIO_TogglePin(GPIOK, GPIO_PIN_7);
    for(volatile uint32_t i = 0; i < 1000000; i++) {}
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif
