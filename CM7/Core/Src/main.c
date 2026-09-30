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

#define PCM_BYTES_PER_CHANNEL    (AUDIO_FRAMES * sizeof(int16_t))

#define UART_BLOCK_PAYLOAD_BYTES          8192U
#define UART_BLOCK_FALLBACK_BYTES         4096U
#define UART_BLOCK_FINAL_FALLBACK_BYTES   1024U
#define UART_BLOCK_MAX_RETRIES            12U
#define UART_BLOCK_ACK_TIMEOUT_MS         1000U
#define UART_CHANNEL_SYNC_RETRIES         5U
#define UART_CHANNEL_SYNC_TIMEOUT_MS      1000U

#define LED_ON(pin)              HAL_GPIO_WritePin(GPIOK, (pin), GPIO_PIN_RESET)
#define LED_OFF(pin)             HAL_GPIO_WritePin(GPIOK, (pin), GPIO_PIN_SET)

__attribute__((section(".RAM_D2_bss"), aligned(32)))
static uint32_t s_dma_raw_a[DMA_WORDS_TOTAL];

__attribute__((section(".RAM_D2_bss"), aligned(32)))
static uint32_t s_dma_raw_b[DMA_WORDS_TOTAL];

static int16_t s_pcm_a_slot0[AUDIO_FRAMES];
static int16_t s_pcm_a_slot1[AUDIO_FRAMES];

static int16_t s_pcm_b_slot0[AUDIO_FRAMES];
static int16_t s_pcm_b_slot1[AUDIO_FRAMES];

static volatile uint32_t s_frames_captured_a = 0U;
static volatile uint32_t s_frames_captured_b = 0U;

static volatile uint32_t s_dma_events_a = 0U;
static volatile uint32_t s_dma_events_b = 0U;

static volatile uint32_t s_sai_error_count_a = 0U;
static volatile uint32_t s_sai_error_count_b = 0U;

static volatile uint32_t s_raw_nonzero_a_slot0 = 0U;
static volatile uint32_t s_raw_nonzero_a_slot1 = 0U;
static volatile uint32_t s_raw_or_a_slot0 = 0U;
static volatile uint32_t s_raw_or_a_slot1 = 0U;

static volatile uint32_t s_raw_nonzero_b_slot0 = 0U;
static volatile uint32_t s_raw_nonzero_b_slot1 = 0U;
static volatile uint32_t s_raw_or_b_slot0 = 0U;
static volatile uint32_t s_raw_or_b_slot1 = 0U;

static volatile uint32_t s_pcm_nonzero_a_slot0 = 0U;
static volatile uint32_t s_pcm_nonzero_a_slot1 = 0U;

static volatile uint32_t s_pcm_nonzero_b_slot0 = 0U;
static volatile uint32_t s_pcm_nonzero_b_slot1 = 0U;

static volatile bool s_capture_active = false;
static volatile bool s_capture_done_a = false;
static volatile bool s_capture_done_b = false;

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

typedef enum
{
    UART_CONTROL_NONE = 0,
    UART_CONTROL_ACK,
    UART_CONTROL_NACK
} UartControl;

static bool uart_transmit_checked(
    const uint8_t *data,
    uint16_t length)
{
    if ((data == NULL) || (length == 0U))
    {
        return false;
    }

    return HAL_UART_Transmit(
               &huart1,
               (uint8_t *)data,
               length,
               HAL_MAX_DELAY) == HAL_OK;
}

static uint32_t crc32_update(
    uint32_t crc,
    const uint8_t *data,
    uint32_t length)
{
    crc = ~crc;

    for (uint32_t i = 0U; i < length; i++)
    {
        crc ^= data[i];

        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            const uint32_t mask =
                (uint32_t)(-(int32_t)(crc & 1U));

            crc =
                (crc >> 1U) ^
                (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

static UartControl wait_uart_control(
    uint32_t timeout_ms)
{
    uint8_t value = 0U;

    if (HAL_UART_Receive(
            &huart1,
            &value,
            1U,
            timeout_ms) != HAL_OK)
    {
        return UART_CONTROL_NONE;
    }

    if (value == (uint8_t)'A')
    {
        return UART_CONTROL_ACK;
    }

    if (value == (uint8_t)'N')
    {
        return UART_CONTROL_NACK;
    }

    return UART_CONTROL_NONE;
}

static uint32_t choose_block_length(
    uint32_t attempt,
    uint32_t remaining)
{
    uint32_t requested =
        UART_BLOCK_PAYLOAD_BYTES;

    if (attempt >= 3U)
    {
        requested =
            UART_BLOCK_FINAL_FALLBACK_BYTES;
    }
    else if (attempt >= 2U)
    {
        requested =
            UART_BLOCK_FALLBACK_BYTES;
    }

    return
        (remaining < requested)
            ? remaining
            : requested;
}

static bool transmit_text_with_ack(
    const char *text)
{
    if (text == NULL)
    {
        return false;
    }

    const size_t length = strlen(text);

    if ((length == 0U) ||
        (length > 0xFFFFU))
    {
        return false;
    }

    for (uint32_t attempt = 1U;
         attempt <= UART_CHANNEL_SYNC_RETRIES;
         attempt++)
    {
        if (!uart_transmit_checked(
                (const uint8_t *)text,
                (uint16_t)length))
        {
            return false;
        }

        if (wait_uart_control(
                UART_CHANNEL_SYNC_TIMEOUT_MS)
            == UART_CONTROL_ACK)
        {
            return true;
        }
    }

    return false;
}

static bool emit_block_once(
    uint32_t channel,
    uint32_t sequence,
    uint32_t offset,
    const uint8_t *payload,
    uint32_t payload_length,
    uint32_t attempt)
{
    if ((payload == NULL) ||
        (payload_length == 0U) ||
        (payload_length >
            UART_BLOCK_PAYLOAD_BYTES))
    {
        return false;
    }

    const uint32_t crc =
        crc32_update(
            0U,
            payload,
            payload_length);

    char header[160];

    const int length =
        snprintf(
            header,
            sizeof(header),
            "[BLK] ch=%lu seq=%lu off=%lu "
            "len=%lu crc=%08lX try=%lu\r\n",
            (unsigned long)channel,
            (unsigned long)sequence,
            (unsigned long)offset,
            (unsigned long)payload_length,
            (unsigned long)crc,
            (unsigned long)attempt);

    if ((length <= 0) ||
        ((size_t)length >= sizeof(header)))
    {
        return false;
    }

    if (!uart_transmit_checked(
            (const uint8_t *)header,
            (uint16_t)length))
    {
        return false;
    }

    return uart_transmit_checked(
        payload,
        (uint16_t)payload_length);
}

static bool emit_block_reliable(
    uint32_t channel,
    uint32_t sequence,
    uint32_t offset,
    const uint8_t *payload,
    uint32_t remaining,
    uint32_t *accepted_length)
{
    if ((payload == NULL) ||
        (remaining == 0U) ||
        (accepted_length == NULL))
    {
        return false;
    }

    *accepted_length = 0U;

    for (uint32_t attempt = 1U;
         attempt <= UART_BLOCK_MAX_RETRIES;
         attempt++)
    {
        const uint32_t block_length =
            choose_block_length(
                attempt,
                remaining);

        if (!emit_block_once(
                channel,
                sequence,
                offset,
                payload,
                block_length,
                attempt))
        {
            return false;
        }

        const UartControl response =
            wait_uart_control(
                UART_BLOCK_ACK_TIMEOUT_MS);

        if (response == UART_CONTROL_ACK)
        {
            *accepted_length =
                block_length;

            return true;
        }
    }

    return false;
}

static inline int16_t pcm24_to_pcm16(uint32_t value)
{
    int32_t sample =
        (int32_t)(value & 0x00FFFFFFU);

    if ((sample & 0x00800000L) != 0)
    {
        sample |= (int32_t)0xFF000000U;
    }

    return (int16_t)(sample >> 8);
}

static void copy_dma_half_to_pcm_a(
    const uint32_t *source,
    uint32_t frame_count)
{
    if (!s_capture_active || s_capture_done_a)
    {
        return;
    }

    s_dma_events_a++;

    for (uint32_t frame = 0U;
         frame < frame_count;
         frame++)
    {
        if (s_frames_captured_a >= AUDIO_FRAMES)
        {
            s_capture_done_a = true;
            return;
        }

        const uint32_t source_index =
            frame * SLOTS_PER_FRAME;

        const uint32_t destination_index =
            s_frames_captured_a;

        const uint32_t raw0 =
            source[source_index];

        const uint32_t raw1 =
            source[source_index + 1U];

        const int16_t pcm0 =
            pcm24_to_pcm16(raw0);

        const int16_t pcm1 =
            pcm24_to_pcm16(raw1);

        s_raw_or_a_slot0 |= raw0;
        s_raw_or_a_slot1 |= raw1;

        if (raw0 != 0U)
        {
            s_raw_nonzero_a_slot0++;
        }

        if (raw1 != 0U)
        {
            s_raw_nonzero_a_slot1++;
        }

        if (pcm0 != 0)
        {
            s_pcm_nonzero_a_slot0++;
        }

        if (pcm1 != 0)
        {
            s_pcm_nonzero_a_slot1++;
        }

        s_pcm_a_slot0[destination_index] = pcm0;
        s_pcm_a_slot1[destination_index] = pcm1;

        s_frames_captured_a++;
    }

    if (s_frames_captured_a >= AUDIO_FRAMES)
    {
        s_capture_done_a = true;
    }
}

static void copy_dma_half_to_pcm_b(
    const uint32_t *source,
    uint32_t frame_count)
{
    if (!s_capture_active || s_capture_done_b)
    {
        return;
    }

    s_dma_events_b++;

    for (uint32_t frame = 0U;
         frame < frame_count;
         frame++)
    {
        if (s_frames_captured_b >= AUDIO_FRAMES)
        {
            s_capture_done_b = true;
            return;
        }

        const uint32_t source_index =
            frame * SLOTS_PER_FRAME;

        const uint32_t destination_index =
            s_frames_captured_b;

        const uint32_t raw0 =
            source[source_index];

        const uint32_t raw1 =
            source[source_index + 1U];

        const int16_t pcm0 =
            pcm24_to_pcm16(raw0);

        const int16_t pcm1 =
            pcm24_to_pcm16(raw1);

        s_raw_or_b_slot0 |= raw0;
        s_raw_or_b_slot1 |= raw1;

        if (raw0 != 0U)
        {
            s_raw_nonzero_b_slot0++;
        }

        if (raw1 != 0U)
        {
            s_raw_nonzero_b_slot1++;
        }

        if (pcm0 != 0)
        {
            s_pcm_nonzero_b_slot0++;
        }

        if (pcm1 != 0)
        {
            s_pcm_nonzero_b_slot1++;
        }

        s_pcm_b_slot0[destination_index] = pcm0;
        s_pcm_b_slot1[destination_index] = pcm1;

        s_frames_captured_b++;
    }

    if (s_frames_captured_b >= AUDIO_FRAMES)
    {
        s_capture_done_b = true;
    }
}

void HAL_SAI_RxHalfCpltCallback(
    SAI_HandleTypeDef *hsai)
{
    if (hsai == NULL)
    {
        return;
    }

    if (hsai->Instance == SAI2_Block_A)
    {
        copy_dma_half_to_pcm_a(
            &s_dma_raw_a[0],
            DMA_FRAMES_PER_HALF
        );
    }
    else if (hsai->Instance == SAI2_Block_B)
    {
        copy_dma_half_to_pcm_b(
            &s_dma_raw_b[0],
            DMA_FRAMES_PER_HALF
        );
    }
}

void HAL_SAI_RxCpltCallback(
    SAI_HandleTypeDef *hsai)
{
    if (hsai == NULL)
    {
        return;
    }

    if (hsai->Instance == SAI2_Block_A)
    {
        copy_dma_half_to_pcm_a(
            &s_dma_raw_a[DMA_WORDS_PER_HALF],
            DMA_FRAMES_PER_HALF
        );
    }
    else if (hsai->Instance == SAI2_Block_B)
    {
        copy_dma_half_to_pcm_b(
            &s_dma_raw_b[DMA_WORDS_PER_HALF],
            DMA_FRAMES_PER_HALF
        );
    }
}

void HAL_SAI_ErrorCallback(
    SAI_HandleTypeDef *hsai)
{
    if (hsai == NULL)
    {
        return;
    }

    if (hsai->Instance == SAI2_Block_A)
    {
        s_sai_error_count_a++;
    }
    else if (hsai->Instance == SAI2_Block_B)
    {
        s_sai_error_count_b++;
    }
}

typedef struct
{
    uint32_t samples;

    uint32_t pi5_high;
    uint32_t pi5_transitions;

    uint32_t pi6_high;
    uint32_t pi6_transitions;

    uint32_t pg10_high;
    uint32_t pg10_transitions;

    uint32_t pi7_high;
    uint32_t pi7_transitions;
} GpioIdrDiag;

static uint32_t gpio_pin_level(
    GPIO_TypeDef *port,
    uint32_t pin)
{
    return ((port->IDR & pin) != 0U)
        ? 1U
        : 0U;
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

    uint32_t previous_pi5 =
        gpio_pin_level(GPIOI, GPIO_PIN_5);

    uint32_t previous_pi6 =
        gpio_pin_level(GPIOI, GPIO_PIN_6);

    uint32_t previous_pg10 =
        gpio_pin_level(GPIOG, GPIO_PIN_10);

    uint32_t previous_pi7 =
        gpio_pin_level(GPIOI, GPIO_PIN_7);

    const uint32_t start_ms =
        HAL_GetTick();

    while ((HAL_GetTick() - start_ms) < duration_ms)
    {
        const uint32_t pi5 =
            gpio_pin_level(GPIOI, GPIO_PIN_5);

        const uint32_t pi6 =
            gpio_pin_level(GPIOI, GPIO_PIN_6);

        const uint32_t pg10 =
            gpio_pin_level(GPIOG, GPIO_PIN_10);

        const uint32_t pi7 =
            gpio_pin_level(GPIOI, GPIO_PIN_7);

        diag->samples++;

        if (pi5 != 0U)
        {
            diag->pi5_high++;
        }

        if (pi6 != 0U)
        {
            diag->pi6_high++;
        }

        if (pg10 != 0U)
        {
            diag->pg10_high++;
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

        if (pg10 != previous_pg10)
        {
            diag->pg10_transitions++;
        }

        if (pi7 != previous_pi7)
        {
            diag->pi7_transitions++;
        }

        previous_pi5 = pi5;
        previous_pi6 = pi6;
        previous_pg10 = pg10;
        previous_pi7 = pi7;
    }
}

static void emit_pin_and_dma_diag(void)
{
    char line[512];

    const uint32_t pi6_mode =
        (GPIOI->MODER >> (6U * 2U)) & 0x3U;

    const uint32_t pi6_pupd =
        (GPIOI->PUPDR >> (6U * 2U)) & 0x3U;

    const uint32_t pi6_af =
        (GPIOI->AFR[0] >> (6U * 4U)) & 0xFU;

    const uint32_t pg10_mode =
        (GPIOG->MODER >> (10U * 2U)) & 0x3U;

    const uint32_t pg10_pupd =
        (GPIOG->PUPDR >> (10U * 2U)) & 0x3U;

    const uint32_t pg10_af =
        (GPIOG->AFR[1] >>
            ((10U - 8U) * 4U)) & 0xFU;

    const uint32_t dcache_enabled =
        ((SCB->CCR & (1UL << 16)) != 0U)
            ? 1U
            : 0U;

    snprintf(
        line,
        sizeof(line),
        "[PINCFG_A] PI6 mode=%lu pupd=%lu af=%lu\r\n",
        (unsigned long)pi6_mode,
        (unsigned long)pi6_pupd,
        (unsigned long)pi6_af
    );

    uart_send_text(line);

    snprintf(
        line,
        sizeof(line),
        "[PINCFG_B] PG10 mode=%lu pupd=%lu af=%lu\r\n",
        (unsigned long)pg10_mode,
        (unsigned long)pg10_pupd,
        (unsigned long)pg10_af
    );

    uart_send_text(line);

    snprintf(
        line,
        sizeof(line),
        "[DMA_A] cpu_addr=0x%08lX bytes=%lu dcache=%lu "
        "dma_m0ar=0x%08lX dma_ndtr=%lu\r\n",
        (unsigned long)(uintptr_t)s_dma_raw_a,
        (unsigned long)sizeof(s_dma_raw_a),
        (unsigned long)dcache_enabled,
        (unsigned long)((DMA_Stream_TypeDef *)
            hsai_BlockA2.hdmarx->Instance)->M0AR,
        (unsigned long)((DMA_Stream_TypeDef *)
            hsai_BlockA2.hdmarx->Instance)->NDTR
    );

    uart_send_text(line);

    snprintf(
        line,
        sizeof(line),
        "[DMA_B] cpu_addr=0x%08lX bytes=%lu dcache=%lu "
        "dma_m0ar=0x%08lX dma_ndtr=%lu\r\n",
        (unsigned long)(uintptr_t)s_dma_raw_b,
        (unsigned long)sizeof(s_dma_raw_b),
        (unsigned long)dcache_enabled,
        (unsigned long)((DMA_Stream_TypeDef *)
            hsai_BlockB2.hdmarx->Instance)->M0AR,
        (unsigned long)((DMA_Stream_TypeDef *)
            hsai_BlockB2.hdmarx->Instance)->NDTR
    );

    uart_send_text(line);

    GpioIdrDiag diag;

    sample_sai_pins_digital(
        &diag,
        100U
    );

    const uint32_t pi5_low =
        diag.samples - diag.pi5_high;

    const uint32_t pi6_low =
        diag.samples - diag.pi6_high;

    const uint32_t pg10_low =
        diag.samples - diag.pg10_high;

    const uint32_t pi7_low =
        diag.samples - diag.pi7_high;

    snprintf(
        line,
        sizeof(line),
        "[GPIO_IDR] duration_ms=100 samples=%lu "
        "PI5_high=%lu PI5_low=%lu PI5_trans=%lu "
        "PI6_high=%lu PI6_low=%lu PI6_trans=%lu "
        "PG10_high=%lu PG10_low=%lu PG10_trans=%lu "
        "PI7_high=%lu PI7_low=%lu PI7_trans=%lu\r\n",
        (unsigned long)diag.samples,

        (unsigned long)diag.pi5_high,
        (unsigned long)pi5_low,
        (unsigned long)diag.pi5_transitions,

        (unsigned long)diag.pi6_high,
        (unsigned long)pi6_low,
        (unsigned long)diag.pi6_transitions,

        (unsigned long)diag.pg10_high,
        (unsigned long)pg10_low,
        (unsigned long)diag.pg10_transitions,

        (unsigned long)diag.pi7_high,
        (unsigned long)pi7_low,
        (unsigned long)diag.pi7_transitions
    );

    uart_send_text(line);
}

static void reset_capture_state(void)
{
    memset(
        s_dma_raw_a,
        0,
        sizeof(s_dma_raw_a)
    );

    memset(
        s_dma_raw_b,
        0,
        sizeof(s_dma_raw_b)
    );

    memset(
        s_pcm_a_slot0,
        0,
        sizeof(s_pcm_a_slot0)
    );

    memset(
        s_pcm_a_slot1,
        0,
        sizeof(s_pcm_a_slot1)
    );

    memset(
        s_pcm_b_slot0,
        0,
        sizeof(s_pcm_b_slot0)
    );

    memset(
        s_pcm_b_slot1,
        0,
        sizeof(s_pcm_b_slot1)
    );

    s_frames_captured_a = 0U;
    s_frames_captured_b = 0U;

    s_dma_events_a = 0U;
    s_dma_events_b = 0U;

    s_sai_error_count_a = 0U;
    s_sai_error_count_b = 0U;

    s_raw_nonzero_a_slot0 = 0U;
    s_raw_nonzero_a_slot1 = 0U;
    s_raw_or_a_slot0 = 0U;
    s_raw_or_a_slot1 = 0U;

    s_raw_nonzero_b_slot0 = 0U;
    s_raw_nonzero_b_slot1 = 0U;
    s_raw_or_b_slot0 = 0U;
    s_raw_or_b_slot1 = 0U;

    s_pcm_nonzero_a_slot0 = 0U;
    s_pcm_nonzero_a_slot1 = 0U;

    s_pcm_nonzero_b_slot0 = 0U;
    s_pcm_nonzero_b_slot1 = 0U;

    s_capture_active = false;
    s_capture_done_a = false;
    s_capture_done_b = false;
}

static void reset_recording_counters(void)
{
    s_frames_captured_a = 0U;
    s_frames_captured_b = 0U;

    s_dma_events_a = 0U;
    s_dma_events_b = 0U;

    s_raw_nonzero_a_slot0 = 0U;
    s_raw_nonzero_a_slot1 = 0U;
    s_raw_or_a_slot0 = 0U;
    s_raw_or_a_slot1 = 0U;

    s_raw_nonzero_b_slot0 = 0U;
    s_raw_nonzero_b_slot1 = 0U;
    s_raw_or_b_slot0 = 0U;
    s_raw_or_b_slot1 = 0U;

    s_pcm_nonzero_a_slot0 = 0U;
    s_pcm_nonzero_a_slot1 = 0U;

    s_pcm_nonzero_b_slot0 = 0U;
    s_pcm_nonzero_b_slot1 = 0U;

    s_capture_done_a = false;
    s_capture_done_b = false;
}

static bool capture_exact_second(void)
{
    char line[512];

    reset_capture_state();

    uart_send_text(
        "[CAPTURE_STARTED] preroll_clock_ms=3000 "
        "sample_rate=44100 frames=44100 "
        "capture=SAI2A+SAI2B "
        "data_a=PI6 data_b=PG10 "
        "clock_master=SAI2A\r\n"
    );

    LED_OFF(LED_RED_Pin);
    LED_OFF(LED_BLUE_Pin);
    LED_ON(LED_GREEN_Pin);

    if (HAL_SAI_Receive_DMA(
            &hsai_BlockB2,
            (uint8_t *)s_dma_raw_b,
            (uint16_t)DMA_WORDS_TOTAL) != HAL_OK)
    {
        LED_OFF(LED_GREEN_Pin);
        LED_ON(LED_RED_Pin);

        uart_send_text(
            "[CAPTURE_START_FAIL] sai=SAI2B\r\n"
        );

        return false;
    }

    if (HAL_SAI_Receive_DMA(
            &hsai_BlockA2,
            (uint8_t *)s_dma_raw_a,
            (uint16_t)DMA_WORDS_TOTAL) != HAL_OK)
    {
        HAL_SAI_DMAStop(&hsai_BlockB2);

        LED_OFF(LED_GREEN_Pin);
        LED_ON(LED_RED_Pin);

        uart_send_text(
            "[CAPTURE_START_FAIL] sai=SAI2A\r\n"
        );

        return false;
    }

    HAL_Delay(100U);

    emit_pin_and_dma_diag();

    HAL_Delay(2800U);

    reset_recording_counters();

    const uint32_t capture_start_ms =
        HAL_GetTick();

    s_capture_active = true;

    while ((!s_capture_done_a ||
            !s_capture_done_b) &&
           (s_sai_error_count_a == 0U) &&
           (s_sai_error_count_b == 0U))
    {
        __WFI();
    }

    const uint32_t capture_elapsed_ms =
        HAL_GetTick() - capture_start_ms;

    s_capture_active = false;

    const HAL_StatusTypeDef stop_b =
        HAL_SAI_DMAStop(&hsai_BlockB2);

    const HAL_StatusTypeDef stop_a =
        HAL_SAI_DMAStop(&hsai_BlockA2);

    if ((stop_a != HAL_OK) ||
        (stop_b != HAL_OK))
    {
        LED_OFF(LED_GREEN_Pin);
        LED_ON(LED_RED_Pin);

        uart_send_text(
            "[CAPTURE_STOP_FAIL]\r\n"
        );

        return false;
    }

    LED_OFF(LED_GREEN_Pin);

    if ((s_sai_error_count_a != 0U) ||
        (s_sai_error_count_b != 0U))
    {
        LED_ON(LED_RED_Pin);

        snprintf(
            line,
            sizeof(line),
            "[CAPTURE_SAI_ERROR] "
            "errors_a=%lu errors_b=%lu\r\n",
            (unsigned long)s_sai_error_count_a,
            (unsigned long)s_sai_error_count_b
        );

        uart_send_text(line);

        return false;
    }

    if ((s_frames_captured_a != AUDIO_FRAMES) ||
        (s_frames_captured_b != AUDIO_FRAMES))
    {
        LED_ON(LED_RED_Pin);

        snprintf(
            line,
            sizeof(line),
            "[CAPTURE_FRAME_COUNT_FAIL] "
            "frames_a=%lu frames_b=%lu "
            "elapsed_ms=%lu "
            "events_a=%lu events_b=%lu\r\n",
            (unsigned long)s_frames_captured_a,
            (unsigned long)s_frames_captured_b,
            (unsigned long)capture_elapsed_ms,
            (unsigned long)s_dma_events_a,
            (unsigned long)s_dma_events_b
        );

        uart_send_text(line);

        return false;
    }

    snprintf(
        line,
        sizeof(line),
        "[CAPTURE_DONE_A] "
        "frames=%lu elapsed_ms=%lu events=%lu "
        "raw_nz0=%lu raw_nz1=%lu "
        "raw_or0=%08lX raw_or1=%08lX "
        "pcm_nz0=%lu pcm_nz1=%lu\r\n",
        (unsigned long)s_frames_captured_a,
        (unsigned long)capture_elapsed_ms,
        (unsigned long)s_dma_events_a,
        (unsigned long)s_raw_nonzero_a_slot0,
        (unsigned long)s_raw_nonzero_a_slot1,
        (unsigned long)s_raw_or_a_slot0,
        (unsigned long)s_raw_or_a_slot1,
        (unsigned long)s_pcm_nonzero_a_slot0,
        (unsigned long)s_pcm_nonzero_a_slot1
    );

    uart_send_text(line);

    snprintf(
        line,
        sizeof(line),
        "[CAPTURE_DONE_B] "
        "frames=%lu elapsed_ms=%lu events=%lu "
        "raw_nz0=%lu raw_nz1=%lu "
        "raw_or0=%08lX raw_or1=%08lX "
        "pcm_nz0=%lu pcm_nz1=%lu\r\n",
        (unsigned long)s_frames_captured_b,
        (unsigned long)capture_elapsed_ms,
        (unsigned long)s_dma_events_b,
        (unsigned long)s_raw_nonzero_b_slot0,
        (unsigned long)s_raw_nonzero_b_slot1,
        (unsigned long)s_raw_or_b_slot0,
        (unsigned long)s_raw_or_b_slot1,
        (unsigned long)s_pcm_nonzero_b_slot0,
        (unsigned long)s_pcm_nonzero_b_slot1
    );

    uart_send_text(line);

    return true;
}

static bool transfer_channel_reliable(
    uint32_t channel,
    const char *channel_name,
    const int16_t *samples)
{
    if ((channel_name == NULL) ||
        (samples == NULL))
    {
        return false;
    }

    const uint8_t *payload =
        (const uint8_t *)samples;

    const uint32_t payload_size =
        PCM_BYTES_PER_CHANNEL;

    char header[192];

    const int header_length =
        snprintf(
            header,
            sizeof(header),
            "[%s_BIN] ch=%lu bytes=%lu "
            "max_block=%lu fallback=%lu "
            "final=%lu ack=block\r\n",
            channel_name,
            (unsigned long)channel,
            (unsigned long)payload_size,
            (unsigned long)
                UART_BLOCK_PAYLOAD_BYTES,
            (unsigned long)
                UART_BLOCK_FALLBACK_BYTES,
            (unsigned long)
                UART_BLOCK_FINAL_FALLBACK_BYTES);

    if ((header_length <= 0) ||
        ((size_t)header_length >= sizeof(header)))
    {
        return false;
    }

    if (!transmit_text_with_ack(header))
    {
        return false;
    }

    uint32_t offset = 0U;
    uint32_t sequence = 0U;

    while (offset < payload_size)
    {
        const uint32_t remaining =
            payload_size - offset;

        uint32_t accepted_length = 0U;

        if (!emit_block_reliable(
                channel,
                sequence,
                offset,
                payload + offset,
                remaining,
                &accepted_length))
        {
            return false;
        }

        if ((accepted_length == 0U) ||
            (accepted_length > remaining))
        {
            return false;
        }

        offset += accepted_length;
        sequence++;
    }

    const int end_length =
        snprintf(
            header,
            sizeof(header),
            "[%s_END] sent=%lu bytes=%lu "
            "packets=%lu errors=0\r\n",
            channel_name,
            (unsigned long)AUDIO_FRAMES,
            (unsigned long)payload_size,
            (unsigned long)sequence);

    if ((end_length <= 0) ||
        ((size_t)end_length >= sizeof(header)))
    {
        return false;
    }

    return transmit_text_with_ack(header);
}

static bool transfer_capture(void)
{
    if (!transfer_channel_reliable(
            0U,
            "A_SLOT0",
            s_pcm_a_slot0))
    {
        uart_send_text(
            "[TRANSFER_FAIL] channel=A_SLOT0\r\n"
        );

        return false;
    }

    if (!transfer_channel_reliable(
            1U,
            "A_SLOT1",
            s_pcm_a_slot1))
    {
        uart_send_text(
            "[TRANSFER_FAIL] channel=A_SLOT1\r\n"
        );

        return false;
    }

    if (!transfer_channel_reliable(
            2U,
            "B_SLOT0",
            s_pcm_b_slot0))
    {
        uart_send_text(
            "[TRANSFER_FAIL] channel=B_SLOT0\r\n"
        );

        return false;
    }

    if (!transfer_channel_reliable(
            3U,
            "B_SLOT1",
            s_pcm_b_slot1))
    {
        uart_send_text(
            "[TRANSFER_FAIL] channel=B_SLOT1\r\n"
        );

        return false;
    }

    uart_send_text(
        "[BASIC_DONE]\r\n"
    );

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

    while ((__HAL_RCC_GET_FLAG(
                RCC_FLAG_D2CKRDY) != RESET) &&
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

    while ((__HAL_RCC_GET_FLAG(
                RCC_FLAG_D2CKRDY) == RESET) &&
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
        "\r\n[BOOT] BasicSetUpMics2Pairs F3.2 "
        "SAI2A+SAI2B 4-channel DMA diag\r\n"
    );

    uart_send_text(
        "[WIRING] "
        "BCLK=PI5 WS=PI7 "
        "SD_A=PI6 SD_B=PG10 "
        "clock_master=SAI2A\r\n"
    );

    uart_send_text(
        "[MAP] "
        "A_SLOT0=SD_A_GND "
        "A_SLOT1=SD_A_VDD "
        "B_SLOT0=SD_B_GND "
        "B_SLOT1=SD_B_VDD\r\n"
    );

    uart_send_text(
        "[UART_PROTO] crc32=1 ack_nack=1 "
        "block=8192 fallback=4096 final=1024\r\n"
    );

    while (1)
    {
        uint8_t command = 0U;

        uart_send_text(
            "[BASIC_READY] send=R "
            "sample_rate=44100 "
            "frames=44100 "
            "channels=4 "
            "preroll_clock_ms=3000\r\n"
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

    HAL_PWREx_ConfigSupply(
        PWR_LDO_SUPPLY
    );

    __HAL_PWR_VOLTAGESCALING_CONFIG(
        PWR_REGULATOR_VOLTAGE_SCALE1
    );

    while (!__HAL_PWR_GET_FLAG(
                PWR_FLAG_VOSRDY))
    {
    }

    __HAL_RCC_GPIOH_CLK_ENABLE();

    GPIO_InitStruct.Pin = GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(
        GPIOH,
        &GPIO_InitStruct
    );

    HAL_GPIO_WritePin(
        GPIOH,
        GPIO_PIN_1,
        GPIO_PIN_SET
    );

    for (volatile uint32_t i = 0U;
         i < 500000U;
         i++)
    {
    }

    RCC_OscInitStruct.OscillatorType =
        RCC_OSCILLATORTYPE_HSI |
        RCC_OSCILLATORTYPE_HSE;

    RCC_OscInitStruct.HSEState =
        RCC_HSE_BYPASS;

    RCC_OscInitStruct.HSIState =
        RCC_HSI_DIV1;

    RCC_OscInitStruct.HSICalibrationValue =
        RCC_HSICALIBRATION_DEFAULT;

    RCC_OscInitStruct.PLL.PLLState =
        RCC_PLL_ON;

    RCC_OscInitStruct.PLL.PLLSource =
        RCC_PLLSOURCE_HSE;

    RCC_OscInitStruct.PLL.PLLM = 5;
    RCC_OscInitStruct.PLL.PLLN = 160;
    RCC_OscInitStruct.PLL.PLLP = 2;
    RCC_OscInitStruct.PLL.PLLQ = 2;
    RCC_OscInitStruct.PLL.PLLR = 2;

    RCC_OscInitStruct.PLL.PLLRGE =
        RCC_PLL1VCIRANGE_2;

    RCC_OscInitStruct.PLL.PLLVCOSEL =
        RCC_PLL1VCOWIDE;

    RCC_OscInitStruct.PLL.PLLFRACN = 0;

    if (HAL_RCC_OscConfig(
            &RCC_OscInitStruct) != HAL_OK)
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

    RCC_ClkInitStruct.SYSCLKSource =
        RCC_SYSCLKSOURCE_PLLCLK;

    RCC_ClkInitStruct.SYSCLKDivider =
        RCC_SYSCLK_DIV1;

    RCC_ClkInitStruct.AHBCLKDivider =
        RCC_HCLK_DIV2;

    RCC_ClkInitStruct.APB3CLKDivider =
        RCC_APB3_DIV2;

    RCC_ClkInitStruct.APB1CLKDivider =
        RCC_APB1_DIV2;

    RCC_ClkInitStruct.APB2CLKDivider =
        RCC_APB2_DIV2;

    RCC_ClkInitStruct.APB4CLKDivider =
        RCC_APB4_DIV2;

    if (HAL_RCC_ClockConfig(
            &RCC_ClkInitStruct,
            FLASH_LATENCY_4) != HAL_OK)
    {
        Error_Handler();
    }
}

void PeriphCommonClock_Config(void)
{
    RCC_PeriphCLKInitTypeDef
        PeriphClkInitStruct = {0};

    PeriphClkInitStruct.PeriphClockSelection =
        RCC_PERIPHCLK_SAI23;

    PeriphClkInitStruct.PLL3.PLL3M = 5;
    PeriphClkInitStruct.PLL3.PLL3N = 72;
    PeriphClkInitStruct.PLL3.PLL3P = 32;
    PeriphClkInitStruct.PLL3.PLL3Q = 2;
    PeriphClkInitStruct.PLL3.PLL3R = 2;

    PeriphClkInitStruct.PLL3.PLL3RGE =
        RCC_PLL3VCIRANGE_2;

    PeriphClkInitStruct.PLL3.PLL3VCOSEL =
        RCC_PLL3VCOWIDE;

    PeriphClkInitStruct.PLL3.PLL3FRACN =
        2077;

    PeriphClkInitStruct.Sai23ClockSelection =
        RCC_SAI23CLKSOURCE_PLL3;

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

    MPU_InitStruct.Enable =
        MPU_REGION_ENABLE;

    MPU_InitStruct.Number =
        MPU_REGION_NUMBER0;

    MPU_InitStruct.BaseAddress =
        0x00000000U;

    MPU_InitStruct.Size =
        MPU_REGION_SIZE_4GB;

    MPU_InitStruct.SubRegionDisable =
        0x87;

    MPU_InitStruct.TypeExtField =
        MPU_TEX_LEVEL0;

    MPU_InitStruct.AccessPermission =
        MPU_REGION_NO_ACCESS;

    MPU_InitStruct.DisableExec =
        MPU_INSTRUCTION_ACCESS_DISABLE;

    MPU_InitStruct.IsShareable =
        MPU_ACCESS_SHAREABLE;

    MPU_InitStruct.IsCacheable =
        MPU_ACCESS_NOT_CACHEABLE;

    MPU_InitStruct.IsBufferable =
        MPU_ACCESS_NOT_BUFFERABLE;

    HAL_MPU_ConfigRegion(
        &MPU_InitStruct
    );

    MPU_InitStruct.Enable =
        MPU_REGION_ENABLE;

    MPU_InitStruct.Number =
        MPU_REGION_NUMBER1;

    MPU_InitStruct.BaseAddress =
        0x24000000U;

    MPU_InitStruct.Size =
        MPU_REGION_SIZE_512KB;

    MPU_InitStruct.SubRegionDisable =
        0x00;

    MPU_InitStruct.TypeExtField =
        MPU_TEX_LEVEL1;

    MPU_InitStruct.AccessPermission =
        MPU_REGION_FULL_ACCESS;

    MPU_InitStruct.DisableExec =
        MPU_INSTRUCTION_ACCESS_DISABLE;

    MPU_InitStruct.IsShareable =
        MPU_ACCESS_NOT_SHAREABLE;

    MPU_InitStruct.IsCacheable =
        MPU_ACCESS_CACHEABLE;

    MPU_InitStruct.IsBufferable =
        MPU_ACCESS_BUFFERABLE;

    HAL_MPU_ConfigRegion(
        &MPU_InitStruct
    );

    MPU_InitStruct.Enable =
        MPU_REGION_ENABLE;

    MPU_InitStruct.Number =
        MPU_REGION_NUMBER2;

    MPU_InitStruct.BaseAddress =
        0x20000000U;

    MPU_InitStruct.Size =
        MPU_REGION_SIZE_128KB;

    MPU_InitStruct.SubRegionDisable =
        0x00;

    MPU_InitStruct.TypeExtField =
        MPU_TEX_LEVEL1;

    MPU_InitStruct.AccessPermission =
        MPU_REGION_FULL_ACCESS;

    MPU_InitStruct.DisableExec =
        MPU_INSTRUCTION_ACCESS_DISABLE;

    MPU_InitStruct.IsShareable =
        MPU_ACCESS_NOT_SHAREABLE;

    MPU_InitStruct.IsCacheable =
        MPU_ACCESS_CACHEABLE;

    MPU_InitStruct.IsBufferable =
        MPU_ACCESS_BUFFERABLE;

    HAL_MPU_ConfigRegion(
        &MPU_InitStruct
    );

    MPU_InitStruct.Enable =
        MPU_REGION_ENABLE;

    MPU_InitStruct.Number =
        MPU_REGION_NUMBER3;

    MPU_InitStruct.BaseAddress =
        0x08040000U;

    MPU_InitStruct.Size =
        MPU_REGION_SIZE_1MB;

    MPU_InitStruct.SubRegionDisable =
        0x00;

    MPU_InitStruct.TypeExtField =
        MPU_TEX_LEVEL0;

    MPU_InitStruct.AccessPermission =
        MPU_REGION_FULL_ACCESS;

    MPU_InitStruct.DisableExec =
        MPU_INSTRUCTION_ACCESS_ENABLE;

    MPU_InitStruct.IsShareable =
        MPU_ACCESS_NOT_SHAREABLE;

    MPU_InitStruct.IsCacheable =
        MPU_ACCESS_CACHEABLE;

    MPU_InitStruct.IsBufferable =
        MPU_ACCESS_NOT_BUFFERABLE;

    HAL_MPU_ConfigRegion(
        &MPU_InitStruct
    );

    MPU_InitStruct.Enable =
        MPU_REGION_ENABLE;

    MPU_InitStruct.Number =
        MPU_REGION_NUMBER4;

    MPU_InitStruct.BaseAddress =
        0x30000000U;

    MPU_InitStruct.Size =
        MPU_REGION_SIZE_256KB;

    MPU_InitStruct.SubRegionDisable =
        0x00;

    MPU_InitStruct.TypeExtField =
        MPU_TEX_LEVEL0;

    MPU_InitStruct.AccessPermission =
        MPU_REGION_FULL_ACCESS;

    MPU_InitStruct.DisableExec =
        MPU_INSTRUCTION_ACCESS_DISABLE;

    MPU_InitStruct.IsShareable =
        MPU_ACCESS_SHAREABLE;

    MPU_InitStruct.IsCacheable =
        MPU_ACCESS_NOT_CACHEABLE;

    MPU_InitStruct.IsBufferable =
        MPU_ACCESS_NOT_BUFFERABLE;

    HAL_MPU_ConfigRegion(
        &MPU_InitStruct
    );

    HAL_MPU_Enable(
        MPU_PRIVILEGED_DEFAULT
    );
}

void Error_Handler(void)
{
    __disable_irq();

    while (1)
    {
        HAL_GPIO_TogglePin(
            GPIOK,
            GPIO_PIN_5
        );

        for (volatile uint32_t i = 0U;
             i < 1000000U;
             i++)
        {
        }

        HAL_GPIO_TogglePin(
            GPIOK,
            GPIO_PIN_7
        );

        for (volatile uint32_t i = 0U;
             i < 1000000U;
             i++)
        {
        }
    }
}

#ifdef USE_FULL_ASSERT

void assert_failed(
    uint32_t line)
{
    (void)file;
    (void)line;
}

#endif
