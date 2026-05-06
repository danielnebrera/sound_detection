/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body with audio capture
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "sai.h"
#include "gpio.h"
#include "audio_capture.h"
#include "uart_driver.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stm32h7xx_hal_pwr_ex.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define DUAL_CORE_BOOT_SYNC_SEQUENCE

#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
#ifndef HSEM_ID_0
#define HSEM_ID_0 (0U) /* HW semaphore 0*/
#endif
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void PeriphCommonClock_Config(void);

/* USER CODE BEGIN PFP */
void audio_test_print_stats(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */


int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

/* USER CODE BEGIN Boot_Mode_Sequence_1 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
  /*HW semaphore Clock enable*/
  __HAL_RCC_HSEM_CLK_ENABLE();
  /* Activate HSEM notification for Cortex-M4*/
  HAL_HSEM_ActivateNotification(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_0));
  /*
  Domain D2 goes to STOP mode (Cortex-M4 in deep-sleep) waiting for Cortex-M7 to
  perform system initialization (system clock config, external memory configuration.. )
  */
  HAL_PWREx_ClearPendingEvent();
  HAL_PWREx_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFE, PWR_D2_DOMAIN);
  /* Clear HSEM flag */
  __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_0));

#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */
/* USER CODE END Boot_Mode_Sequence_1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  PeriphCommonClock_Config();  // PLL3 for SAI clock
  uart_init();
  MX_SAI1_Init();
  
  /* USER CODE BEGIN 2 */
  
  /* Initialize audio capture */
  if (audio_capture_init(&g_audio_ctx) != 0)
  {
      Error_Handler();
  }
  
  if (audio_capture_start(&g_audio_ctx) != 0)
  {
      Error_Handler();
  }
  
  /* Pequeño delay para que UART se estabilice */
  HAL_Delay(100);
  
  printf("\n\n");
  printf("=== Audio Capture Started ===\n");
  printf("Sample Rate: %d Hz\n", AUDIO_SAMPLE_RATE);
  printf("Channels: %d\n", AUDIO_CHANNELS);
  printf("Buffer Size: %d samples\n", AUDIO_BUFFER_SIZE);
  printf("DMA Buffer Size: %d samples\n\n", AUDIO_DMA_BUFFER_SIZE);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t sample_count = 0;
  
  while (1)
  {
      /* Poll for new audio data */
      AudioBufferState state = audio_capture_get_data(&g_audio_ctx);
      
      if (state == AUDIO_BUFFER_HALF || state == AUDIO_BUFFER_FULL)
      {
          sample_count += AUDIO_BUFFER_SIZE;
          
          /* Print stats every 44100 samples (~1 second at 44.1kHz) */
          if (sample_count >= AUDIO_SAMPLE_RATE)
          {
              sample_count = 0;
              audio_test_print_stats();
          }
          
          /* TODO: Process audio here
           * 
           * int32_t *mic1 = audio_capture_get_channel(&g_audio_ctx, 0);
           * int32_t *mic2 = audio_capture_get_channel(&g_audio_ctx, 1);
           * int32_t *mic3 = audio_capture_get_channel(&g_audio_ctx, 2);
           * int32_t *mic4 = audio_capture_get_channel(&g_audio_ctx, 3);
           * 
           * // Process MFCC, TDOA, etc.
           * mfcc_compute(mic1, AUDIO_BUFFER_SIZE);
           * gcc_phat_compute(mic1, mic2, ...);
           */
      }
      
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief Peripherals Common Clock Configuration
  * @retval None
  */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock
  */
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
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/**
 * @brief Print audio statistics (for debugging)
 */
void audio_test_print_stats(void)
{
    uint32_t frames = 0, errors = 0;
    audio_capture_get_stats(&g_audio_ctx, &frames, &errors);
    
    printf("[AUDIO] Frames: %lu | Errors: %lu\n", frames, errors);
    
    /* Optional: Print first few samples of each channel */
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

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
