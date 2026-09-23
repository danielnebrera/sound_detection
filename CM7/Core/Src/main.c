#include "main.h"
#include "dma.h"
#include "sai.h"
#include "usart.h"
#include "gpio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE        44100U
#define AUDIO_FRAMES             44100U

#define DMA_FRAMES_PER_HALF      512U
#define DMA_HALVES               2U
#define SLOTS_PER_FRAME          2U
#define DMA_WORDS_PER_HALF       (DMA_FRAMES_PER_HALF * SLOTS_PER_FRAME)
#define DMA_WORDS_TOTAL          (DMA_WORDS_PER_HALF * DMA_HALVES)

#define PCM_BYTES_PER_SLOT       (AUDIO_FRAMES * sizeof(int16_t))
#define UART_TX_BLOCK_BYTES      4096U

#define LED_ON(pin)              HAL_GPIO_WritePin(GPIOK, (pin), GPIO_PIN_RESET)
#define LED_OFF(pin)             HAL_GPIO_WritePin(GPIOK, (pin), GPIO_PIN_SET)

__attribute__((section(".RAM_D2_bss"), aligned(32)))
static uint32_t s_dma_raw[DMA_WORDS_TOTAL];

static int16_t s_pcm_slot0[AUDIO_FRAMES];
static int16_t s_pcm_slot1[AUDIO_FRAMES];

static volatile uint32_t s_frames_captured = 0U;
static volatile uint32_t s_sai_error_count = 0U;
static volatile uint32_t s_dma_events = 0U;

static volatile uint32_t s_raw_nonzero_slot0 = 0U;
static volatile uint32_t s_raw_nonzero_slot1 = 0U;
static volatile uint32_t s_raw_or_slot0 = 0U;
static volatile uint32_t s_raw_or_slot1 = 0U;

static volatile uint32_t s_pcm_nonzero_slot0 = 0U;
static volatile uint32_t s_pcm_nonzero_slot1 = 0U;

static volatile bool s_capture_active = false;
static volatile bool s_capture_done = false;

void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);

static void uart_send_text(const char *text)
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

static bool uart_send_binary(const uint8_t *data, uint32_t size)
{
    uint32_t offset = 0U;

    while (offset < size)
    {
        const uint32_t remaining = size - offset;
        const uint16_t block_size =
            (remaining > UART_TX_BLOCK_BYTES)
                ? (uint16_t)UART_TX_BLOCK_BYTES
                : (uint16_t)remaining;

        if (HAL_UART_Transmit(
                &huart1,
                (uint8_t *)&data[offset],
                block_size,
                HAL_MAX_DELAY) != HAL_OK)
        {
            return false;
        }

        offset += block_size;
    }

    return true;
}

static inline int16_t pcm24_to_pcm16(uint32_t value)
{
    int32_t sample = (int32_t)(value & 0x00FFFFFFU);

    if ((sample & 0x00800000L) != 0)
    {
        sample |= (int32_t)0xFF000000U;
    }

    return (int16_t)(sample >> 8);
}

static void copy_dma_half_to_pcm(
    const uint32_t *source,
    uint32_t frame_count)
{
    if (!s_capture_active || s_capture_done)
    {
        return;
    }

    s_dma_events++;

    for (uint32_t frame = 0U; frame < frame_count; frame++)
    {
        if (s_frames_captured >= AUDIO_FRAMES)
        {
            s_capture_active = false;
            s_capture_done = true;
            return;
        }

        const uint32_t source_index = frame * SLOTS_PER_FRAME;
        const uint32_t destination_index = s_frames_captured;

        const uint32_t raw0 = source[source_index];
        const uint32_t raw1 = source[source_index + 1U];

        const int16_t pcm0 = pcm24_to_pcm16(raw0);
        const int16_t pcm1 = pcm24_to_pcm16(raw1);

        s_raw_or_slot0 |= raw0;
        s_raw_or_slot1 |= raw1;

        if (raw0 != 0U)
        {
            s_raw_nonzero_slot0++;
        }

        if (raw1 != 0U)
        {
            s_raw_nonzero_slot1++;
        }

        if (pcm0 != 0)
        {
            s_pcm_nonzero_slot0++;
        }

        if (pcm1 != 0)
        {
            s_pcm_nonzero_slot1++;
        }

        s_pcm_slot0[destination_index] = pcm0;
        s_pcm_slot1[destination_index] = pcm1;

        s_frames_captured++;
    }

    if (s_frames_captured >= AUDIO_FRAMES)
    {
        s_capture_active = false;
        s_capture_done = true;
    }
}

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    if ((hsai != NULL) && (hsai->Instance == SAI2_Block_A))
    {
        copy_dma_half_to_pcm(
            &s_dma_raw[0],
            DMA_FRAMES_PER_HALF
        );
    }
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    if ((hsai != NULL) && (hsai->Instance == SAI2_Block_A))
    {
        copy_dma_half_to_pcm(
            &s_dma_raw[DMA_WORDS_PER_HALF],
            DMA_FRAMES_PER_HALF
        );
    }
}

void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
    if ((hsai != NULL) && (hsai->Instance == SAI2_Block_A))
    {
        s_sai_error_count++;
    }
}


typedef struct
{
    uint32_t samples;

    uint32_t pi5_high;
    uint32_t pi5_transitions;

    uint32_t pi6_high;
    uint32_t pi6_transitions;

    uint32_t pi7_high;
    uint32_t pi7_transitions;
} GpioIdrDiag;

static uint32_t gpio_pin_level(GPIO_TypeDef *port, uint32_t pin)
{
    return ((port->IDR & pin) != 0U) ? 1U : 0U;
}

static void sample_sai_pins_digital(
    GpioIdrDiag *diag,
    uint32_t duration_ms)
{
    if (diag == NULL)
    {
        return;
    }

    memset(diag, 0, sizeof(*diag));

    uint32_t previous_pi5 = gpio_pin_level(GPIOI, GPIO_PIN_5);
    uint32_t previous_pi6 = gpio_pin_level(GPIOI, GPIO_PIN_6);
    uint32_t previous_pi7 = gpio_pin_level(GPIOI, GPIO_PIN_7);

    const uint32_t start_ms = HAL_GetTick();

    while ((HAL_GetTick() - start_ms) < duration_ms)
    {
        const uint32_t pi5 = gpio_pin_level(GPIOI, GPIO_PIN_5);
        const uint32_t pi6 = gpio_pin_level(GPIOI, GPIO_PIN_6);
        const uint32_t pi7 = gpio_pin_level(GPIOI, GPIO_PIN_7);

        diag->samples++;

        if (pi5 != 0U)
        {
            diag->pi5_high++;
        }

        if (pi6 != 0U)
        {
            diag->pi6_high++;
        }

        if (pi7 != 0U)
        {
            diag->pi7_high++;
        }

        if (pi5 != previous_pi5)
        {
            diag->pi5_transitions++;
        }

        if (pi6 != previous_pi6)
        {
            diag->pi6_transitions++;
        }

        if (pi7 != previous_pi7)
        {
            diag->pi7_transitions++;
        }

        previous_pi5 = pi5;
        previous_pi6 = pi6;
        previous_pi7 = pi7;
    }
}

static void emit_pin_and_dma_diag(void)
{
    char line[360];

    const uint32_t pi6_mode =
        (GPIOI->MODER >> (6U * 2U)) & 0x3U;

    const uint32_t pi6_pupd =
        (GPIOI->PUPDR >> (6U * 2U)) & 0x3U;

    const uint32_t pi6_af =
        (GPIOI->AFR[0] >> (6U * 4U)) & 0xFU;

    const uint32_t dcache_enabled =
        ((SCB->CCR & (1UL << 16)) != 0U) ? 1U : 0U;

    snprintf(
        line,
        sizeof(line),
        "[PINCFG] PI6 mode=%lu pupd=%lu af=%lu\r\n",
        (unsigned long)pi6_mode,
        (unsigned long)pi6_pupd,
        (unsigned long)pi6_af
    );
    uart_send_text(line);

    snprintf(
        line,
        sizeof(line),
        "[DMA_BUF] cpu_addr=0x%08lX bytes=%lu dcache=%lu "
        "dma_m0ar=0x%08lX dma_ndtr=%lu\r\n",
        (unsigned long)(uintptr_t)s_dma_raw,
        (unsigned long)sizeof(s_dma_raw),
        (unsigned long)dcache_enabled,
        (unsigned long)((DMA_Stream_TypeDef *)hsai_BlockA2.hdmarx->Instance)->M0AR,
        (unsigned long)((DMA_Stream_TypeDef *)hsai_BlockA2.hdmarx->Instance)->NDTR
    );
    uart_send_text(line);

    GpioIdrDiag diag;
    sample_sai_pins_digital(&diag, 100U);

    const uint32_t pi5_low = diag.samples - diag.pi5_high;
    const uint32_t pi6_low = diag.samples - diag.pi6_high;
    const uint32_t pi7_low = diag.samples - diag.pi7_high;

    snprintf(
        line,
        sizeof(line),
        "[GPIO_IDR] duration_ms=100 samples=%lu "
        "PI5_high=%lu PI5_low=%lu PI5_trans=%lu "
        "PI6_high=%lu PI6_low=%lu PI6_trans=%lu "
        "PI7_high=%lu PI7_low=%lu PI7_trans=%lu\r\n",
        (unsigned long)diag.samples,
        (unsigned long)diag.pi5_high,
        (unsigned long)pi5_low,
        (unsigned long)diag.pi5_transitions,
        (unsigned long)diag.pi6_high,
        (unsigned long)pi6_low,
        (unsigned long)diag.pi6_transitions,
        (unsigned long)diag.pi7_high,
        (unsigned long)pi7_low,
        (unsigned long)diag.pi7_transitions
    );
    uart_send_text(line);
}

static bool capture_exact_second(void)
{
    char line[320];

    memset(s_dma_raw, 0, sizeof(s_dma_raw));
    memset(s_pcm_slot0, 0, sizeof(s_pcm_slot0));
    memset(s_pcm_slot1, 0, sizeof(s_pcm_slot1));

    s_frames_captured = 0U;
    s_sai_error_count = 0U;
    s_dma_events = 0U;

    s_raw_nonzero_slot0 = 0U;
    s_raw_nonzero_slot1 = 0U;
    s_raw_or_slot0 = 0U;
    s_raw_or_slot1 = 0U;

    s_pcm_nonzero_slot0 = 0U;
    s_pcm_nonzero_slot1 = 0U;

    s_capture_done = false;
    s_capture_active = false;

    uart_send_text(
        "[CAPTURE_STARTED] preroll_clock_ms=3000 sample_rate=44100 "
        "frames=44100 sai=SAI2A data=PI6\r\n"
    );

    LED_OFF(LED_RED_Pin);
    LED_OFF(LED_BLUE_Pin);
    LED_ON(LED_GREEN_Pin);

    /*
     * Arrancar SAI2A ANTES del pre-roll.
     * Durante estos 3 s PI5/PI7 ya generan BCLK/WS y el micro puede
     * salir de power-down/estabilizarse. Los callbacks se descartan
     * porque s_capture_active sigue en false.
     */
    if (HAL_SAI_Receive_DMA(
            &hsai_BlockA2,
            (uint8_t *)s_dma_raw,
            (uint16_t)DMA_WORDS_TOTAL) != HAL_OK)
    {
        LED_OFF(LED_GREEN_Pin);
        LED_ON(LED_RED_Pin);
        uart_send_text("[CAPTURE_START_FAIL]\r\n");
        return false;
    }

    /*
     * Dejar estabilizar brevemente los clocks y después comprobar el nivel
     * digital que el propio STM32 ve en PI5/PI6/PI7 mientras siguen en AF10.
     */
    HAL_Delay(100U);
    emit_pin_and_dma_diag();

    /*
     * Completar aproximadamente 3 s de pre-roll total antes de guardar audio.
     */
    HAL_Delay(2800U);

    /*
     * Comenzar ahora el segundo que realmente se guarda.
     */
    s_frames_captured = 0U;
    s_raw_nonzero_slot0 = 0U;
    s_raw_nonzero_slot1 = 0U;
    s_raw_or_slot0 = 0U;
    s_raw_or_slot1 = 0U;
    s_pcm_nonzero_slot0 = 0U;
    s_pcm_nonzero_slot1 = 0U;
    s_dma_events = 0U;
    s_capture_done = false;

    const uint32_t capture_start_ms = HAL_GetTick();

    s_capture_active = true;

    while (!s_capture_done && (s_sai_error_count == 0U))
    {
        __WFI();
    }

    const uint32_t capture_elapsed_ms =
        HAL_GetTick() - capture_start_ms;

    s_capture_active = false;

    if (HAL_SAI_DMAStop(&hsai_BlockA2) != HAL_OK)
    {
        LED_OFF(LED_GREEN_Pin);
        LED_ON(LED_RED_Pin);
        uart_send_text("[CAPTURE_STOP_FAIL]\r\n");
        return false;
    }

    LED_OFF(LED_GREEN_Pin);

    if (s_sai_error_count != 0U)
    {
        LED_ON(LED_RED_Pin);
        uart_send_text("[CAPTURE_SAI_ERROR]\r\n");
        return false;
    }

    if (s_frames_captured != AUDIO_FRAMES)
    {
        LED_ON(LED_RED_Pin);

        snprintf(
            line,
            sizeof(line),
            "[CAPTURE_FRAME_COUNT_FAIL] frames=%lu elapsed_ms=%lu "
            "events=%lu\r\n",
            (unsigned long)s_frames_captured,
            (unsigned long)capture_elapsed_ms,
            (unsigned long)s_dma_events
        );

        uart_send_text(line);
        return false;
    }

    snprintf(
        line,
        sizeof(line),
        "[CAPTURE_DONE] frames=%lu elapsed_ms=%lu events=%lu "
        "raw_nz0=%lu raw_nz1=%lu "
        "raw_or0=%08lX raw_or1=%08lX "
        "pcm_nz0=%lu pcm_nz1=%lu\r\n",
        (unsigned long)s_frames_captured,
        (unsigned long)capture_elapsed_ms,
        (unsigned long)s_dma_events,
        (unsigned long)s_raw_nonzero_slot0,
        (unsigned long)s_raw_nonzero_slot1,
        (unsigned long)s_raw_or_slot0,
        (unsigned long)s_raw_or_slot1,
        (unsigned long)s_pcm_nonzero_slot0,
        (unsigned long)s_pcm_nonzero_slot1
    );

    uart_send_text(line);
    return true;
}

static bool transfer_slot(
    uint32_t slot,
    const int16_t *samples)
{
    if (slot == 0U)
    {
        uart_send_text("[SLOT0_BIN] bytes=88200\r\n");
    }
    else
    {
        uart_send_text("[SLOT1_BIN] bytes=88200\r\n");
    }

    if (!uart_send_binary(
            (const uint8_t *)samples,
            PCM_BYTES_PER_SLOT))
    {
        return false;
    }

    if (slot == 0U)
    {
        uart_send_text("[SLOT0_END]\r\n");
    }
    else
    {
        uart_send_text("[SLOT1_END]\r\n");
    }

    return true;
}

static bool transfer_capture(void)
{
    if (!transfer_slot(0U, s_pcm_slot0))
    {
        uart_send_text("[TRANSFER_FAIL] slot=0\r\n");
        return false;
    }

    if (!transfer_slot(1U, s_pcm_slot1))
    {
        uart_send_text("[TRANSFER_FAIL] slot=1\r\n");
        return false;
    }

    uart_send_text("[BASIC_DONE]\r\n");
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
    MX_USART1_UART_Init();
    MX_DMA_Init();
    MX_SAI2_Init();

    LED_OFF(LED_RED_Pin);
    LED_OFF(LED_GREEN_Pin);
    LED_OFF(LED_BLUE_Pin);

    uart_send_text(
        "\r\n[BOOT] BasicSetupMics P0.2c "
        "single-mic SAI2A PI6 GPIO-IDR DMA diag\r\n"
    );

    uart_send_text(
        "[WIRING] BCLK=PI5 WS=PI7 DOUT=PI6 PG10=unused SEL=VDD\r\n"
    );

    while (1)
    {
        uint8_t command = 0U;

        uart_send_text(
            "[BASIC_READY] send=R sample_rate=44100 "
            "frames=44100 slots=2 preroll_clock_ms=3000\r\n"
        );

        if (HAL_UART_Receive(
                &huart1,
                &command,
                1U,
                500U) != HAL_OK)
        {
            continue;
        }

        if (command != (uint8_t)'R')
        {
            continue;
        }

        if (!capture_exact_second())
        {
            continue;
        }

        if (!transfer_capture())
        {
            LED_ON(LED_RED_Pin);
            continue;
        }

        LED_OFF(LED_RED_Pin);
        LED_OFF(LED_GREEN_Pin);
        LED_ON(LED_BLUE_Pin);
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
    {
    }

    __HAL_RCC_GPIOH_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOH, &GPIO_InitStruct);

    HAL_GPIO_WritePin(GPIOH, GPIO_PIN_1, GPIO_PIN_SET);

    for (volatile uint32_t i = 0U; i < 500000U; i++)
    {
    }

    RCC_OscInitStruct.OscillatorType =
        RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSE;
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

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_PCLK1 |
        RCC_CLOCKTYPE_PCLK2 |
        RCC_CLOCKTYPE_D3PCLK1 |
        RCC_CLOCKTYPE_D1PCLK1;

    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
    RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

    if (HAL_RCC_ClockConfig(
            &RCC_ClkInitStruct,
            FLASH_LATENCY_4) != HAL_OK)
    {
        Error_Handler();
    }
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

    if (HAL_RCCEx_PeriphCLKConfig(
            &PeriphClkInitStruct) != HAL_OK)
    {
        Error_Handler();
    }
}

static void MPU_Config(void)
{
    MPU_Region_InitTypeDef MPU_InitStruct = {0};

    HAL_MPU_Disable();

    MPU_InitStruct.Enable = MPU_REGION_ENABLE;
    MPU_InitStruct.Number = MPU_REGION_NUMBER0;
    MPU_InitStruct.BaseAddress = 0x00000000U;
    MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
    MPU_InitStruct.SubRegionDisable = 0x87;
    MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
    MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
    MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
    MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    MPU_InitStruct.Enable = MPU_REGION_ENABLE;
    MPU_InitStruct.Number = MPU_REGION_NUMBER1;
    MPU_InitStruct.BaseAddress = 0x24000000U;
    MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
    MPU_InitStruct.SubRegionDisable = 0x00;
    MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
    MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    MPU_InitStruct.Enable = MPU_REGION_ENABLE;
    MPU_InitStruct.Number = MPU_REGION_NUMBER2;
    MPU_InitStruct.BaseAddress = 0x20000000U;
    MPU_InitStruct.Size = MPU_REGION_SIZE_128KB;
    MPU_InitStruct.SubRegionDisable = 0x00;
    MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
    MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    MPU_InitStruct.Enable = MPU_REGION_ENABLE;
    MPU_InitStruct.Number = MPU_REGION_NUMBER3;
    MPU_InitStruct.BaseAddress = 0x08040000U;
    MPU_InitStruct.Size = MPU_REGION_SIZE_1MB;
    MPU_InitStruct.SubRegionDisable = 0x00;
    MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
    MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
    MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    MPU_InitStruct.Enable = MPU_REGION_ENABLE;
    MPU_InitStruct.Number = MPU_REGION_NUMBER4;
    MPU_InitStruct.BaseAddress = 0x30000000U;
    MPU_InitStruct.Size = MPU_REGION_SIZE_256KB;
    MPU_InitStruct.SubRegionDisable = 0x00;
    MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
    MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

void Error_Handler(void)
{
    __disable_irq();

    while (1)
    {
        HAL_GPIO_TogglePin(GPIOK, GPIO_PIN_5);

        for (volatile uint32_t i = 0U; i < 1000000U; i++)
        {
        }

        HAL_GPIO_TogglePin(GPIOK, GPIO_PIN_7);

        for (volatile uint32_t i = 0U; i < 1000000U; i++)
        {
        }
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
