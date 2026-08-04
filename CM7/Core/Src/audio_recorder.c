/* =================================================================
 * audio_recorder.c - binary UART transmission with CRC32
 *
 * One raw PCM16 copy is kept for each microphone. The buffers are also
 * consumed by drone_detection, avoiding a second full audio window.
 *
 * Memory placement:
 *   CH0 / Mic2 -> RAM_D1
 *   CH1 / Mic4 -> RAM_D2
 *   CH2 / Mic3 -> RAM_D2
 *   CH3 / Mic1 -> DTCM
 *
 * UART protocol per channel:
 *   [CHx_BIN]\r\n
 *   <88200 bytes int16 little-endian>
 *   <4 bytes CRC32 little-endian>
 *   [CHx_END] sent=44100 errors=0\r\n
 *   PC -> A (ACK) o N (reintento)
 * Al terminar el chunk, PC envia K despues de guardar los JSON.
 * ================================================================= */

#include "audio_recorder.h"
#include "audio_capture.h"
#include "usart.h"

#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart1;
extern AudioCaptureContext g_audio_ctx;

__attribute__((section(".RAM_D1_audio"), aligned(32)))
static int16_t s_pcm_ch0[RECORD_FRAMES];

__attribute__((section(".RAM_D2_audio"), aligned(32)))
static int16_t s_pcm_ch1[RECORD_FRAMES];

__attribute__((section(".RAM_D2_audio"), aligned(32)))
static int16_t s_pcm_ch2[RECORD_FRAMES];

__attribute__((section(".DTCM_audio"), aligned(32)))
static int16_t s_pcm_ch3[RECORD_FRAMES];

static int16_t *const s_pcm_channels[RECORD_CHANNELS] =
{
    s_pcm_ch0,
    s_pcm_ch1,
    s_pcm_ch2,
    s_pcm_ch3
};

static uint32_t s_write_frame = 0U;
static bool s_chunk_ready = false;
static bool s_stop = false;

#define UART_CHANNEL_MAX_RETRIES  3U
#define UART_CHANNEL_ACK_TIMEOUT  10000U
#define UART_CHUNK_ACK_TIMEOUT    60000U
#define UART_TX_BLOCK_BYTES       16384U

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(RECORD_FRAMES == 44100U, "Recorder expects 44100 frames");
_Static_assert(sizeof(s_pcm_ch0) == 88200U, "Unexpected channel size");
#endif

static uint32_t crc32_update(
    uint32_t crc,
    const uint8_t *buffer,
    uint32_t length)
{
    crc = ~crc;

    for (uint32_t i = 0U; i < length; i++)
    {
        crc ^= buffer[i];

        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            crc =
                (crc >> 1U) ^
                (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }

    return ~crc;
}

static inline int16_t pcm24_to_pcm16(int32_t value)
{
    int32_t sample =
        (int32_t)((uint32_t)value & 0x00FFFFFFU);

    if ((sample & 0x00800000L) != 0)
    {
        sample |= (int32_t)0xFF000000U;
    }

    return (int16_t)(sample >> 8);
}

static void uart_puts(const char *text)
{
    HAL_UART_Transmit(
        &huart1,
        (uint8_t *)text,
        (uint16_t)strlen(text),
        HAL_MAX_DELAY
    );
}

static bool check_stop(void)
{
    uint8_t command = 0U;

    if (HAL_UART_Receive(&huart1, &command, 1U, 0U) == HAL_OK)
    {
        if (command == (uint8_t)'S')
        {
            s_stop = true;
        }
    }

    return s_stop;
}

typedef enum
{
    UART_CONTROL_TIMEOUT = 0,
    UART_CONTROL_ACK,
    UART_CONTROL_NACK,
    UART_CONTROL_CONTINUE,
    UART_CONTROL_STOP
} UartControl;

static UartControl wait_uart_control(uint32_t timeout_ms)
{
    const uint32_t start_tick = HAL_GetTick();

    while ((HAL_GetTick() - start_tick) < timeout_ms)
    {
        uint8_t command = 0U;

        if (HAL_UART_Receive(&huart1, &command, 1U, 50U) != HAL_OK)
        {
            continue;
        }

        if (command == (uint8_t)'A')
        {
            return UART_CONTROL_ACK;
        }

        if (command == (uint8_t)'N')
        {
            return UART_CONTROL_NACK;
        }

        if (command == (uint8_t)'K')
        {
            return UART_CONTROL_CONTINUE;
        }

        if (command == (uint8_t)'S')
        {
            s_stop = true;
            return UART_CONTROL_STOP;
        }
    }

    return UART_CONTROL_TIMEOUT;
}

static bool uart_transmit_checked(
    const uint8_t *data,
    uint16_t size)
{
    return HAL_UART_Transmit(
        &huart1,
        (uint8_t *)data,
        size,
        HAL_MAX_DELAY
    ) == HAL_OK;
}

static bool emit_channel_once(uint32_t channel)
{
    static const char *const channel_names[RECORD_CHANNELS] =
    {
        "CH0", "CH1", "CH2", "CH3"
    };

    char header[64];
    const uint8_t *payload =
        (const uint8_t *)s_pcm_channels[channel];
    const uint32_t payload_size =
        RECORD_FRAMES * (uint32_t)sizeof(int16_t);
    const uint32_t crc =
        crc32_update(0U, payload, payload_size);

    uint32_t offset = 0U;
    uint32_t sent_bytes = 0U;
    uint32_t errors = 0U;

    snprintf(
        header,
        sizeof(header),
        "[%s_BIN]\r\n",
        channel_names[channel]
    );

    if (!uart_transmit_checked(
            (const uint8_t *)header,
            (uint16_t)strlen(header)))
    {
        return false;
    }

    while ((offset < payload_size) && !s_stop)
    {
        uint32_t block_size = payload_size - offset;

        if (block_size > UART_TX_BLOCK_BYTES)
        {
            block_size = UART_TX_BLOCK_BYTES;
        }

        bool block_sent = false;

        for (uint32_t attempt = 0U;
             attempt < 3U;
             attempt++)
        {
            if (uart_transmit_checked(
                    payload + offset,
                    (uint16_t)block_size))
            {
                block_sent = true;
                break;
            }

            HAL_Delay(2U);
        }

        if (!block_sent)
        {
            errors += block_size / sizeof(int16_t);
            break;
        }

        sent_bytes += block_size;
        offset += block_size;
        check_stop();
    }

    const uint8_t crc_buffer[4] =
    {
        (uint8_t)(crc & 0xFFU),
        (uint8_t)((crc >> 8U) & 0xFFU),
        (uint8_t)((crc >> 16U) & 0xFFU),
        (uint8_t)((crc >> 24U) & 0xFFU)
    };

    const bool crc_sent =
        uart_transmit_checked(crc_buffer, sizeof(crc_buffer));

    snprintf(
        header,
        sizeof(header),
        "[%s_END] sent=%lu errors=%lu\r\n",
        channel_names[channel],
        (unsigned long)(sent_bytes / sizeof(int16_t)),
        (unsigned long)errors
    );

    const bool end_sent =
        uart_transmit_checked(
            (const uint8_t *)header,
            (uint16_t)strlen(header)
        );

    return
        !s_stop &&
        (sent_bytes == payload_size) &&
        (errors == 0U) &&
        crc_sent &&
        end_sent;
}

static bool emit_channel_with_retry(uint32_t channel)
{
    static const char *const channel_names[RECORD_CHANNELS] =
    {
        "CH0", "CH1", "CH2", "CH3"
    };

    for (uint32_t attempt = 1U;
         attempt <= UART_CHANNEL_MAX_RETRIES;
         attempt++)
    {
        const bool transmitted = emit_channel_once(channel);
        const UartControl response =
            wait_uart_control(UART_CHANNEL_ACK_TIMEOUT);

        if ((response == UART_CONTROL_ACK) && transmitted)
        {
            return true;
        }

        if (response == UART_CONTROL_STOP)
        {
            return false;
        }

        if (attempt < UART_CHANNEL_MAX_RETRIES)
        {
            char message[64];

            snprintf(
                message,
                sizeof(message),
                "[%s_RETRY] attempt=%lu\r\n",
                channel_names[channel],
                (unsigned long)(attempt + 1U)
            );
            uart_puts(message);
        }
    }

    {
        char message[64];

        snprintf(
            message,
            sizeof(message),
            "[%s_FAILED] retries=%u\r\n",
            channel_names[channel],
            (unsigned)UART_CHANNEL_MAX_RETRIES
        );
        uart_puts(message);
    }

    return false;
}

void audio_recorder_init(void)
{
    for (uint32_t channel = 0U; channel < RECORD_CHANNELS; channel++)
    {
        memset(
            s_pcm_channels[channel],
            0,
            RECORD_FRAMES * sizeof(int16_t)
        );
    }

    s_write_frame = 0U;
    s_chunk_ready = false;
    s_stop = false;

    uart_puts("[RECORD_READY]\r\n");
}

void audio_recorder_accumulate(void)
{
    if (s_chunk_ready)
    {
        return;
    }

    int32_t *input[RECORD_CHANNELS] =
    {
        audio_capture_get_channel(&g_audio_ctx, 0U),
        audio_capture_get_channel(&g_audio_ctx, 1U),
        audio_capture_get_channel(&g_audio_ctx, 2U),
        audio_capture_get_channel(&g_audio_ctx, 3U)
    };

    for (uint32_t channel = 0U; channel < RECORD_CHANNELS; channel++)
    {
        if (input[channel] == NULL)
        {
            return;
        }
    }

    uint32_t source_offset = 0U;

    while (source_offset < AUDIO_BUFFER_SIZE)
    {
        const uint32_t source_remaining =
            AUDIO_BUFFER_SIZE - source_offset;

        const uint32_t chunk_remaining =
            RECORD_FRAMES - s_write_frame;

        const uint32_t copy_count =
            (source_remaining < chunk_remaining)
                ? source_remaining
                : chunk_remaining;

        for (uint32_t i = 0U; i < copy_count; i++)
        {
            const uint32_t source_index = source_offset + i;
            const uint32_t destination_index = s_write_frame + i;

            for (uint32_t channel = 0U;
                 channel < RECORD_CHANNELS;
                 channel++)
            {
                s_pcm_channels[channel][destination_index] =
                    pcm24_to_pcm16(input[channel][source_index]);
            }
        }

        source_offset += copy_count;
        s_write_frame += copy_count;

        if (s_write_frame == RECORD_FRAMES)
        {
            s_chunk_ready = true;
            break;
        }
    }
}

bool audio_recorder_is_ready(void)
{
    return s_chunk_ready;
}

bool audio_recorder_get_chunk_view(RecorderChunkView *view)
{
    if ((view == NULL) || !s_chunk_ready)
    {
        return false;
    }

    for (uint32_t channel = 0U; channel < RECORD_CHANNELS; channel++)
    {
        view->channel[channel] = s_pcm_channels[channel];
    }

    view->frame_count = RECORD_FRAMES;
    return true;
}

bool audio_recorder_emit_and_reset(void)
{
    if (!s_chunk_ready)
    {
        return true;
    }

    char header[64];

    snprintf(
        header,
        sizeof(header),
        "[CHUNK_START] %lu\r\n",
        (unsigned long)RECORD_FRAMES
    );
    uart_puts(header);

    for (uint32_t channel = 0U;
         (channel < RECORD_CHANNELS) && !s_stop;
         channel++)
    {
        if (!emit_channel_with_retry(channel))
        {
            s_stop = true;
            uart_puts("[CHUNK_ABORT] channel_transfer_failed\r\n");
            return false;
        }
    }

    uart_puts("[CHUNK_END]\r\n");

    const UartControl chunk_response =
        wait_uart_control(UART_CHUNK_ACK_TIMEOUT);

    if (chunk_response != UART_CONTROL_CONTINUE)
    {
        if (chunk_response != UART_CONTROL_STOP)
        {
            uart_puts("[CHUNK_ABORT] save_ack_timeout\r\n");
        }

        s_stop = true;
        return false;
    }

    s_write_frame = 0U;
    s_chunk_ready = false;

    return true;
}

bool audio_recorder_poll_stop(void)
{
    return check_stop();
}

bool audio_recorder_stop_requested(void)
{
    return s_stop;
}
