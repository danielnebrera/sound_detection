/* =================================================================
 * main.c - unified 4-microphone recording and drone detection
 *
 * Each cycle:
 *   1. Capture exactly one second from four simultaneous microphones.
 *   2. Keep SAI DMA running continuously so SCK/FS never stop.
 *   3. Run detection over the same PCM16 buffers used by the recorder.
 *   4. Transmit the four channels through the existing binary CRC32
 *      UART protocol while capture clocks remain active.
 *   5. Discard pending DMA events and start the next stored window.
 *
 * The UART transfer is slower than real time, so stored windows remain
 * separated by processing/transmission gaps, but SAI itself never restarts.
 * ================================================================= */

#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "sai.h"
#include "usart.h"
#include "gpio.h"

#include "audio_capture.h"
#include "audio_recorder.h"
#include "drone_detection.h"
#include "fdcan.h"
#include "can_sender.h"
#include "stm32h7xx.h"

#include <stdio.h>
#include <string.h>

extern volatile uint32_t sai_half_count;
extern volatile uint32_t sai_full_count;
extern volatile uint32_t dma_irq_count;

void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);

#define LED_ON(pin)     HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_RESET)
#define LED_OFF(pin)    HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_SET)
#define LED_TOGGLE(pin) HAL_GPIO_TogglePin(GPIOK, pin)

static bool start_audio_capture_once(void)
{
    if (audio_capture_start(&g_audio_ctx) != 0)
    {
        printf("CM7: ERROR audio_capture_start\r\n");
        return false;
    }

    HAL_Delay(20U);

    if ((hsai_BlockA2.State != HAL_SAI_STATE_BUSY_RX) ||
        (hsai_BlockB2.State != HAL_SAI_STATE_BUSY_RX))
    {
        printf(
            "CM7: ERROR SAI no esta en BUSY_RX A=%d B=%d\r\n",
            (int)hsai_BlockA2.State,
            (int)hsai_BlockB2.State
        );
        return false;
    }

    return true;
}

int main(void)
{
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
    int32_t timeout;
#endif

    MPU_Config();

#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
    timeout = 0xFFFF;
    while ((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) &&
           (timeout-- > 0))
    {
    }

    if (timeout < 0)
    {
        Error_Handler();
    }
#endif

    HAL_Init();
    SystemClock_Config();
    PeriphCommonClock_Config();

#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
    __HAL_RCC_HSEM_CLK_ENABLE();
    HAL_HSEM_FastTake(HSEM_ID_0);
    HAL_HSEM_Release(HSEM_ID_0, 0);

    timeout = 0xFFFF;
    while ((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) == RESET) &&
           (timeout-- > 0))
    {
    }

    if (timeout < 0)
    {
        Error_Handler();
    }
#endif

    MX_GPIO_Init();
    MX_DMA_Init();
    MX_SAI2_Init();
    MX_USART1_UART_Init();
    MX_I2C1_Init();
    MX_FDCAN1_Init();

    HAL_UART_Transmit(
        &huart1,
        (uint8_t *)"UART_INIT_OK\r\n",
        14U,
        100U
    );
    HAL_UART_Transmit(
        &huart1,
        (uint8_t *)"BOOT\r\n",
        6U,
        100U
    );

    LED_OFF(GPIO_PIN_5);
    LED_OFF(GPIO_PIN_6);
    LED_OFF(GPIO_PIN_7);

    {
        const uint32_t rsr = RCC->RSR;

        printf(
            "[RESET] RSR=0x%08lX POR=%lu PIN=%lu SW=%lu "
            "IWDG=%lu WWDG=%lu\r\n",
            (unsigned long)rsr,
            (unsigned long)((rsr & RCC_RSR_PORRSTF) != 0U),
            (unsigned long)((rsr & RCC_RSR_PINRSTF) != 0U),
            (unsigned long)((rsr & RCC_RSR_SFT2RSTF) != 0U),
            (unsigned long)((rsr & RCC_RSR_IWDG1RSTF) != 0U),
            (unsigned long)((rsr & RCC_RSR_WWDG1RSTF) != 0U)
        );

        __HAL_RCC_CLEAR_RESET_FLAGS();
    }

    if (!can_sender_init())
    {
        printf("[MAIN] WARN: CAN no pudo iniciar\r\n");
    }

    {
        const uint8_t pmic_address = (uint8_t)(0x08U << 1U);
        uint8_t set_voltage[2] = {0x51U, 0x0FU};
        uint8_t enable_ldo[2] = {0x4FU, 0x0FU};

        const HAL_StatusTypeDef result_voltage =
            HAL_I2C_Master_Transmit(
                &hi2c1,
                pmic_address,
                set_voltage,
                2U,
                100U
            );

        const HAL_StatusTypeDef result_enable =
            HAL_I2C_Master_Transmit(
                &hi2c1,
                pmic_address,
                enable_ldo,
                2U,
                100U
            );

        HAL_Delay(50U);

        if ((result_voltage == HAL_OK) && (result_enable == HAL_OK))
        {
            printf("PMIC: LDO2 ON OK\r\n");
        }
        else
        {
            printf(
                "PMIC: LDO2 ERROR r1=%d r2=%d\r\n",
                (int)result_voltage,
                (int)result_enable
            );
        }
    }

    if (audio_capture_init(&g_audio_ctx) != 0)
    {
        printf("CM7: ERROR audio_capture_init\r\n");
        while (1)
        {
            LED_TOGGLE(GPIO_PIN_7);
            HAL_Delay(500U);
        }
    }
    printf("CM7: audio_capture_init OK\r\n");

    if (!start_audio_capture_once())
    {
        Error_Handler();
    }
    printf("CM7: audio_capture_start OK\r\n");

    if (!drone_detection_init())
    {
        printf("CM7: ERROR drone_detection_init\r\n");
        while (1)
        {
            LED_TOGGLE(GPIO_PIN_7);
            HAL_Delay(250U);
        }
    }
    printf("CM7: drone_detection_init OK\r\n");

    printf("[UART_RX] esperando byte...\r\n");
    {
        uint8_t command = 0U;

        while (HAL_UART_Receive(
                   &huart1,
                   &command,
                   1U,
                   500U
               ) != HAL_OK)
        {
            LED_TOGGLE(GPIO_PIN_6);
        }

        HAL_Delay(20U);
        while (HAL_UART_Receive(
                   &huart1,
                   &command,
                   1U,
                   5U
               ) == HAL_OK)
        {
        }

        printf("CM7: comando recibido, iniciando grabacion\r\n");
    }

    audio_recorder_init();
    audio_capture_discard_pending(&g_audio_ctx);

    printf(
        "[SAI_RUN] A_CR1=0x%08lX A_SR=0x%08lX "
        "B_CR1=0x%08lX B_SR=0x%08lX\r\n",
        (unsigned long)SAI2_Block_A->CR1,
        (unsigned long)SAI2_Block_A->SR,
        (unsigned long)SAI2_Block_B->CR1,
        (unsigned long)SAI2_Block_B->SR
    );

    uint32_t chunk_number = 1U;

    printf(
        "[GRABANDO] Chunk %lu - 4 mics simultaneos\r\n",
        (unsigned long)chunk_number
    );

    while (1)
    {
        const AudioBufferState state =
            audio_capture_get_data(&g_audio_ctx);

        if ((state != AUDIO_BUFFER_HALF) &&
            (state != AUDIO_BUFFER_FULL))
        {
            continue;
        }

        LED_TOGGLE(GPIO_PIN_5);

        if (audio_recorder_poll_stop())
        {
            audio_capture_stop(&g_audio_ctx);
            printf("[RECORD_DONE]\r\n");
            LED_ON(GPIO_PIN_7);

            while (1)
            {
                HAL_Delay(1000U);
            }
        }

        audio_recorder_accumulate();

        if (!audio_recorder_is_ready())
        {
            continue;
        }

        RecorderChunkView window = {0};

        if (!audio_recorder_get_chunk_view(&window))
        {
            printf("[MAIN] ERROR: no se pudo obtener la ventana\r\n");
            Error_Handler();
        }

        printf(
            "[CHUNK_LISTO] Chunk %lu grabado - detectando...\r\n",
            (unsigned long)chunk_number
        );

        LED_ON(GPIO_PIN_6);

        if (!drone_detection_process_window(&window))
        {
            printf(
                "[MAIN] WARN: deteccion fallo en chunk %lu\r\n",
                (unsigned long)chunk_number
            );
        }

        if (audio_recorder_poll_stop())
        {
            audio_capture_stop(&g_audio_ctx);
            printf("[RECORD_DONE]\r\n");
            LED_ON(GPIO_PIN_7);

            while (1)
            {
                HAL_Delay(1000U);
            }
        }

        printf(
            "[TRANSMITIENDO] Chunk %lu por UART binaria...\r\n",
            (unsigned long)chunk_number
        );

        const bool continue_recording =
            audio_recorder_emit_and_reset();

        LED_OFF(GPIO_PIN_6);

        printf(
            "[CHUNK_GUARDADO] Chunk %lu completado "
            "(4 canales x %us)\r\n",
            (unsigned long)chunk_number,
            (unsigned)RECORD_SECONDS
        );

        if (!continue_recording ||
            audio_recorder_stop_requested())
        {
            audio_capture_stop(&g_audio_ctx);
            printf("[RECORD_DONE]\r\n");
            LED_ON(GPIO_PIN_7);

            while (1)
            {
                HAL_Delay(1000U);
            }
        }

        chunk_number++;
        audio_capture_discard_pending(&g_audio_ctx);

        printf(
            "[GRABANDO] Chunk %lu - 4 mics simultaneos\r\n",
            (unsigned long)chunk_number
        );
    }
}

/* ── SystemClock_Config, PeriphCommonClock_Config, MPU_Config ── */
/* (idénticas al main.c original — copiadas sin cambios)          */

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  __HAL_RCC_GPIOH_CLK_ENABLE();
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOH, &GPIO_InitStruct);
  HAL_GPIO_WritePin(GPIOH, GPIO_PIN_1, GPIO_PIN_SET);
  for(volatile uint32_t i = 0; i < 500000; i++) {}

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
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

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
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK) Error_Handler();
}

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
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) Error_Handler();
}

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};
  HAL_MPU_Disable();

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x00000000; MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x24000000; MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.SubRegionDisable = 0x00; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x20000000; MPU_InitStruct.Size = MPU_REGION_SIZE_128KB;
  MPU_InitStruct.SubRegionDisable = 0x00; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER3;
  MPU_InitStruct.BaseAddress = 0x08040000; MPU_InitStruct.Size = MPU_REGION_SIZE_1MB;
  MPU_InitStruct.SubRegionDisable = 0x00; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER4;
  MPU_InitStruct.BaseAddress = 0x30000000; MPU_InitStruct.Size = MPU_REGION_SIZE_256KB;
  MPU_InitStruct.SubRegionDisable = 0x00; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

void Error_Handler(void)
{
  __disable_irq();
  while(1) {
    HAL_GPIO_TogglePin(GPIOK, GPIO_PIN_5);
    for(volatile uint32_t i = 0; i < 1000000; i++) {}
    HAL_GPIO_TogglePin(GPIOK, GPIO_PIN_7);
    for(volatile uint32_t i = 0; i < 1000000; i++) {}
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
