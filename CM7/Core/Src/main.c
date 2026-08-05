/* =================================================================
 * main.c
 *
 * Flujo:
 *   1. Inicializa SDRAM integrada.
 *   2. Espera R desde grabar_sesion.py.
 *   3. Captura 3 s de calibracion + hasta 10 s de audio continuo.
 *   4. Ctrl+C envia S y la captura cierra en el proximo segundo exacto.
 *   5. Detiene SAI, ejecuta DSP + MFCC + modelo en STM32.
 *   6. Transmite los chunks con CRC32 y resultados reales.
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
#include "portenta_sdram.h"
#include "stm32h7xx.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);

#define SDRAM_RUN_QUICK_BOOT_TEST  0

#define LED_ON(pin)     HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_RESET)
#define LED_OFF(pin)    HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_SET)
#define LED_TOGGLE(pin) HAL_GPIO_TogglePin(GPIOK, pin)

static bool s_audio_running = false;

static void uart_send(const char *text)
{
    if (text == NULL)
    {
        return;
    }

    HAL_UART_Transmit(
        &huart1,
        (uint8_t *)text,
        (uint16_t)strlen(text),
        HAL_MAX_DELAY
    );
}

static bool configure_microphone_power(void)
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

    return
        (result_voltage == HAL_OK) &&
        (result_enable == HAL_OK);
}

static bool wait_for_record_command(void)
{
    uart_send(
        "[RECORD_READY] calibration=3 max_record=10 "
        "sample_rate=44100 channels=4\r\n"
    );

    while (1)
    {
        uint8_t command = 0U;

        if (HAL_UART_Receive(
                &huart1,
                &command,
                1U,
                500U) == HAL_OK)
        {
            if (command == (uint8_t)'R')
            {
                return true;
            }
        }

        LED_TOGGLE(LED_BLUE_Pin);
    }
}

static bool start_audio_capture(void)
{
    if (audio_capture_start(&g_audio_ctx) != 0)
    {
        return false;
    }

    s_audio_running = true;
    HAL_Delay(20U);

    return
        (hsai_BlockA2.State == HAL_SAI_STATE_BUSY_RX) &&
        (hsai_BlockB2.State == HAL_SAI_STATE_BUSY_RX);
}

static uint8_t bit_mask_from_flags(const uint8_t flags[RECORD_CHANNELS])
{
    uint8_t mask = 0U;

    for (uint32_t channel = 0U; channel < RECORD_CHANNELS; channel++)
    {
        if (flags[channel] != 0U)
        {
            mask |= (uint8_t)(1U << channel);
        }
    }

    return mask;
}

static void format_detection_line(
    char *destination,
    size_t destination_size,
    uint32_t order,
    const DroneDetectionResult *result)
{
    if ((destination == NULL) ||
        (destination_size == 0U) ||
        (result == NULL))
    {
        return;
    }

    const uint8_t valid_mask = bit_mask_from_flags(result->valid);
    const uint8_t active_mask =
        bit_mask_from_flags(result->acoustic_active);

    snprintf(
        destination,
        destination_size,
        "[DET_RESULT] order=%lu alert=%u baseline=%u "
        "fused=%.6f ema=%.6f valid=0x%02X active=0x%02X "
        "p0=%.6f p1=%.6f p2=%.6f p3=%.6f "
        "db0=%.3f db1=%.3f db2=%.3f db3=%.3f "
        "delta0=%.3f delta1=%.3f delta2=%.3f delta3=%.3f\r\n",
        (unsigned long)order,
        (unsigned)result->alert_level,
        (unsigned)result->baseline_ready,
        (double)result->fused_probability,
        (double)result->ema,
        (unsigned)valid_mask,
        (unsigned)active_mask,
        (double)result->probability[0],
        (double)result->probability[1],
        (double)result->probability[2],
        (double)result->probability[3],
        (double)result->dbfs[0],
        (double)result->dbfs[1],
        (double)result->dbfs[2],
        (double)result->dbfs[3],
        (double)result->delta_dbfs[0],
        (double)result->delta_dbfs[1],
        (double)result->delta_dbfs[2],
        (double)result->delta_dbfs[3]
    );
}

static void stop_with_error(const char *message)
{
    if (s_audio_running)
    {
        (void)audio_capture_stop(&g_audio_ctx);
        s_audio_running = false;
    }

    LED_OFF(LED_GREEN_Pin);
    LED_OFF(LED_BLUE_Pin);
    LED_ON(LED_RED_Pin);

    uart_send(message);

    while (1)
    {
        LED_TOGGLE(LED_RED_Pin);
        HAL_Delay(250U);
    }
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
    MX_USART1_UART_Init();

    LED_OFF(LED_RED_Pin);
    LED_OFF(LED_GREEN_Pin);
    LED_OFF(LED_BLUE_Pin);

    uart_send("[BOOT] Continuous SDRAM recorder v1\r\n");

    if (!portenta_sdram_init())
    {
        stop_with_error("[SDRAM_INIT_FAIL]\r\n");
    }

#if SDRAM_RUN_QUICK_BOOT_TEST
    {
        PortentaSdramTestReport report = {0};

        if (!portenta_sdram_quick_test(&report))
        {
            stop_with_error("[SDRAM_QUICK_TEST_FAIL]\r\n");
        }
    }
#endif

    MX_I2C1_Init();
    MX_DMA_Init();
    MX_SAI2_Init();

    if (!configure_microphone_power())
    {
        stop_with_error("[PMIC_FAIL]\r\n");
    }

    if (audio_capture_init(&g_audio_ctx) != 0)
    {
        stop_with_error("[AUDIO_CAPTURE_INIT_FAIL]\r\n");
    }

    audio_recorder_init();

    if (!wait_for_record_command())
    {
        stop_with_error("[RECORD_COMMAND_FAIL]\r\n");
    }

    uart_send(
        "[CAPTURE_STARTED] calibration=3 max_record=10 "
        "stop_at_second_boundary=1\r\n"
    );

    if (!start_audio_capture())
    {
        stop_with_error("[AUDIO_CAPTURE_START_FAIL]\r\n");
    }

    LED_ON(LED_GREEN_Pin);
    LED_OFF(LED_BLUE_Pin);

    uint32_t last_completed_second = 0U;

    while (!audio_recorder_capture_complete())
    {
        audio_recorder_poll_stop();

        const AudioBufferState state =
            audio_capture_get_data(&g_audio_ctx);

        if (state == AUDIO_BUFFER_OVERRUN)
        {
            stop_with_error("[CAPTURE_OVERRUN]\r\n");
        }

        if ((state != AUDIO_BUFFER_HALF) &&
            (state != AUDIO_BUFFER_FULL))
        {
            continue;
        }

        if (!audio_recorder_accumulate())
        {
            stop_with_error("[CAPTURE_COPY_FAIL]\r\n");
        }

        const uint32_t completed_second =
            audio_recorder_total_completed_chunks();

        if (completed_second != last_completed_second)
        {
            last_completed_second = completed_second;
            LED_TOGGLE(LED_GREEN_Pin);
        }
    }

    if (audio_capture_stop(&g_audio_ctx) != 0)
    {
        stop_with_error("[AUDIO_CAPTURE_STOP_FAIL]\r\n");
    }
    s_audio_running = false;

    LED_OFF(LED_GREEN_Pin);
    LED_ON(LED_BLUE_Pin);

    const uint32_t recorded_chunks =
        audio_recorder_recorded_chunks();

    {
        char message[160];

        snprintf(
            message,
            sizeof(message),
            "[CAPTURE_DONE] total_frames=%lu total_chunks=%lu "
            "calibration_chunks=%u record_chunks=%lu stop_requested=%u\r\n",
            (unsigned long)audio_recorder_total_frames(),
            (unsigned long)audio_recorder_total_completed_chunks(),
            (unsigned)RECORD_CALIBRATION_CHUNKS,
            (unsigned long)recorded_chunks,
            (unsigned)audio_recorder_stop_requested()
        );
        uart_send(message);
    }

    if (recorded_chunks == 0U)
    {
        stop_with_error("[NO_COMPLETE_RECORD_CHUNKS]\r\n");
    }

    if (!drone_detection_init())
    {
        stop_with_error("[DETECTION_INIT_FAIL]\r\n");
    }

    uart_send("[PROCESSING_START] calibration=3\r\n");

    for (uint32_t chunk = 0U;
         chunk < RECORD_CALIBRATION_CHUNKS;
         chunk++)
    {
        RecorderChunkView view = {0};

        if (!audio_recorder_get_absolute_chunk_view(chunk, &view) ||
            !drone_detection_process_window(&view))
        {
            stop_with_error("[CALIBRATION_PROCESS_FAIL]\r\n");
        }
    }

    if (!audio_recorder_begin_transfer(recorded_chunks))
    {
        stop_with_error("[TRANSFER_START_FAIL]\r\n");
    }

    for (uint32_t chunk = 0U; chunk < recorded_chunks; chunk++)
    {
        RecorderChunkView view = {0};
        DroneDetectionResult result = {0};
        char detection_line[512];
        const uint32_t order = chunk + 1U;

        if (!audio_recorder_get_record_chunk_view(chunk, &view) ||
            !drone_detection_process_window(&view) ||
            !drone_detection_get_last_result(&result))
        {
            stop_with_error("[DETECTION_PROCESS_FAIL]\r\n");
        }

        format_detection_line(
            detection_line,
            sizeof(detection_line),
            order,
            &result
        );

        if (!audio_recorder_emit_record_chunk(
                chunk,
                order,
                detection_line))
        {
            stop_with_error("[TRANSFER_CHUNK_FAIL]\r\n");
        }
    }

    audio_recorder_end_transfer(recorded_chunks);

    LED_OFF(LED_RED_Pin);
    LED_OFF(LED_BLUE_Pin);
    LED_ON(LED_GREEN_Pin);

    while (1)
    {
        HAL_Delay(1000U);
    }
}

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

static void MPU_Config(void)
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

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER5;
  MPU_InitStruct.BaseAddress = PORTENTA_SDRAM_BASE_ADDRESS; MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
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
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
