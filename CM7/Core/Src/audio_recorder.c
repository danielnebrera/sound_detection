/* =================================================================
 * audio_recorder.c
 *
 * Captura continua de 4 canales PCM16 en la SDRAM de la Portenta H7.
 * No transmite ni procesa MFCC mientras SAI esta capturando.
 * ================================================================= */

#include "audio_recorder.h"
#include "audio_capture.h"
#include "usart.h"
#include "portenta_sdram.h"

#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart1;
extern AudioCaptureContext g_audio_ctx;

/*
 * La SDRAM ya fue validada en 0x60000000. Para esta primera integración
 * accedemos mediante un puntero fijo, evitando que el linker intente
 * reservar/cargar los 4.6 MiB dentro de RAM_D1 o FLASH.
 *
 * Tipo resultante:
 *   s_session_pcm[chunk][channel][frame]
 */
static int16_t (*const s_session_pcm)
    [RECORD_CHANNELS]
    [RECORD_FRAMES] =
        (int16_t (*)[RECORD_CHANNELS][RECORD_FRAMES])
        PORTENTA_SDRAM_BASE_ADDRESS;

#define RECORD_SDRAM_REQUIRED_BYTES \
    ((uint32_t)RECORD_TOTAL_STORAGE_CHUNKS * \
     (uint32_t)RECORD_CHANNELS * \
     (uint32_t)RECORD_FRAMES * \
     (uint32_t)sizeof(int16_t))

static uint32_t s_total_frames = 0U;
static bool s_capture_stop_requested = false;
static bool s_capture_complete = false;
static bool s_transfer_abort = false;

#define UART_CHANNEL_MAX_RETRIES  3U
#define UART_CHANNEL_ACK_TIMEOUT  10000U
#define UART_CHUNK_ACK_TIMEOUT    60000U
#define UART_TX_BLOCK_BYTES       16384U

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(RECORD_FRAMES == 44100U, "Recorder expects 44100 frames");
_Static_assert(RECORD_CHANNELS == 4U, "Recorder expects four channels");
_Static_assert(
    RECORD_SDRAM_REQUIRED_BYTES <= PORTENTA_SDRAM_SIZE_BYTES,
    "Audio session buffer exceeds Portenta SDRAM capacity"
);
#endif

static uint32_t minimum_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

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
    int32_t sample = (int32_t)((uint32_t)value & 0x00FFFFFFU);

    if ((sample & 0x00800000L) != 0)
    {
        sample |= (int32_t)0xFF000000U;
    }

    return (int16_t)(sample >> 8);
}

void audio_recorder_send_text(const char *text)
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

void audio_recorder_init(void)
{
    /* No se limpia la SDRAM completa: cada muestra valida sera sobrescrita. */
    s_total_frames = 0U;
    s_capture_stop_requested = false;
    s_capture_complete = false;
    s_transfer_abort = false;
}

bool audio_recorder_poll_stop(void)
{
    uint8_t command = 0U;

    while (HAL_UART_Receive(&huart1, &command, 1U, 0U) == HAL_OK)
    {
        if (command == (uint8_t)'S')
        {
            s_capture_stop_requested = true;
        }
    }

    return s_capture_stop_requested;
}

static bool stop_boundary_reached(void)
{
    const uint32_t minimum_frames =
        (RECORD_CALIBRATION_CHUNKS + 1U) * RECORD_FRAMES;

    return
        s_capture_stop_requested &&
        (s_total_frames >= minimum_frames) &&
        ((s_total_frames % RECORD_FRAMES) == 0U);
}

bool audio_recorder_accumulate(void)
{
    if (s_capture_complete)
    {
        return true;
    }

    if (stop_boundary_reached() ||
        (s_total_frames >= RECORD_TOTAL_STORAGE_FRAMES))
    {
        s_capture_complete = true;
        return true;
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
            return false;
        }
    }

    uint32_t source_offset = 0U;

    while ((source_offset < AUDIO_BUFFER_SIZE) && !s_capture_complete)
    {
        if (stop_boundary_reached())
        {
            s_capture_complete = true;
            break;
        }

        if (s_total_frames >= RECORD_TOTAL_STORAGE_FRAMES)
        {
            s_capture_complete = true;
            break;
        }

        const uint32_t absolute_chunk = s_total_frames / RECORD_FRAMES;
        const uint32_t frame_in_chunk = s_total_frames % RECORD_FRAMES;
        const uint32_t source_remaining = AUDIO_BUFFER_SIZE - source_offset;
        const uint32_t chunk_remaining = RECORD_FRAMES - frame_in_chunk;
        const uint32_t capacity_remaining =
            RECORD_TOTAL_STORAGE_FRAMES - s_total_frames;

        uint32_t copy_count = minimum_u32(source_remaining, chunk_remaining);
        copy_count = minimum_u32(copy_count, capacity_remaining);

        for (uint32_t channel = 0U; channel < RECORD_CHANNELS; channel++)
        {
            int16_t *destination =
                &s_session_pcm[absolute_chunk][channel][frame_in_chunk];

            for (uint32_t i = 0U; i < copy_count; i++)
            {
                destination[i] =
                    pcm24_to_pcm16(input[channel][source_offset + i]);
            }
        }

        source_offset += copy_count;
        s_total_frames += copy_count;

        if (stop_boundary_reached() ||
            (s_total_frames >= RECORD_TOTAL_STORAGE_FRAMES))
        {
            s_capture_complete = true;
        }
    }

    return true;
}

bool audio_recorder_capture_complete(void)
{
    return s_capture_complete;
}

bool audio_recorder_stop_requested(void)
{
    return s_capture_stop_requested;
}

uint32_t audio_recorder_total_frames(void)
{
    return s_total_frames;
}

uint32_t audio_recorder_total_completed_chunks(void)
{
    return s_total_frames / RECORD_FRAMES;
}

uint32_t audio_recorder_recorded_chunks(void)
{
    const uint32_t completed = audio_recorder_total_completed_chunks();

    if (completed <= RECORD_CALIBRATION_CHUNKS)
    {
        return 0U;
    }

    return minimum_u32(
        completed - RECORD_CALIBRATION_CHUNKS,
        RECORD_MAX_OUTPUT_CHUNKS
    );
}

bool audio_recorder_get_absolute_chunk_view(
    uint32_t absolute_chunk,
    RecorderChunkView *view)
{
    if ((view == NULL) ||
        (absolute_chunk >= audio_recorder_total_completed_chunks()))
    {
        return false;
    }

    for (uint32_t channel = 0U; channel < RECORD_CHANNELS; channel++)
    {
        view->channel[channel] =
            &s_session_pcm[absolute_chunk][channel][0];
    }

    view->frame_count = RECORD_FRAMES;
    return true;
}

bool audio_recorder_get_record_chunk_view(
    uint32_t record_chunk,
    RecorderChunkView *view)
{
    if (record_chunk >= audio_recorder_recorded_chunks())
    {
        return false;
    }

    return audio_recorder_get_absolute_chunk_view(
        RECORD_CALIBRATION_CHUNKS + record_chunk,
        view
    );
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
            s_transfer_abort = true;
            return UART_CONTROL_STOP;
        }
    }

    return UART_CONTROL_TIMEOUT;
}

static bool emit_channel_once(
    uint32_t channel,
    const int16_t *samples)
{
    static const char *const channel_names[RECORD_CHANNELS] =
    {
        "CH0", "CH1", "CH2", "CH3"
    };

    if ((channel >= RECORD_CHANNELS) || (samples == NULL))
    {
        return false;
    }

    char header[64];
    const uint8_t *payload = (const uint8_t *)samples;
    const uint32_t payload_size = RECORD_FRAMES * sizeof(int16_t);
    const uint32_t crc = crc32_update(0U, payload, payload_size);

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

    uint32_t offset = 0U;

    while ((offset < payload_size) && !s_transfer_abort)
    {
        uint32_t block_size = payload_size - offset;

        if (block_size > UART_TX_BLOCK_BYTES)
        {
            block_size = UART_TX_BLOCK_BYTES;
        }

        if (!uart_transmit_checked(
                payload + offset,
                (uint16_t)block_size))
        {
            return false;
        }

        offset += block_size;
    }

    const uint8_t crc_buffer[4] =
    {
        (uint8_t)(crc & 0xFFU),
        (uint8_t)((crc >> 8U) & 0xFFU),
        (uint8_t)((crc >> 16U) & 0xFFU),
        (uint8_t)((crc >> 24U) & 0xFFU)
    };

    if (!uart_transmit_checked(crc_buffer, sizeof(crc_buffer)))
    {
        return false;
    }

    snprintf(
        header,
        sizeof(header),
        "[%s_END] sent=%lu errors=0\r\n",
        channel_names[channel],
        (unsigned long)RECORD_FRAMES
    );

    return uart_transmit_checked(
        (const uint8_t *)header,
        (uint16_t)strlen(header)
    );
}

static bool emit_channel_with_retry(
    uint32_t channel,
    const int16_t *samples)
{
    static const char *const channel_names[RECORD_CHANNELS] =
    {
        "CH0", "CH1", "CH2", "CH3"
    };

    for (uint32_t attempt = 1U;
         attempt <= UART_CHANNEL_MAX_RETRIES;
         attempt++)
    {
        const bool transmitted = emit_channel_once(channel, samples);
        const UartControl response =
            wait_uart_control(UART_CHANNEL_ACK_TIMEOUT);

        if (transmitted && (response == UART_CONTROL_ACK))
        {
            return true;
        }

        if ((response == UART_CONTROL_STOP) || s_transfer_abort)
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
            audio_recorder_send_text(message);
        }
    }

    return false;
}

bool audio_recorder_begin_transfer(uint32_t chunk_count)
{
    char message[128];

    s_transfer_abort = false;

    snprintf(
        message,
        sizeof(message),
        "[SESSION_START] chunks=%lu sample_rate=%lu channels=%u\r\n",
        (unsigned long)chunk_count,
        (unsigned long)RECORD_SAMPLE_RATE,
        (unsigned)RECORD_CHANNELS
    );

    audio_recorder_send_text(message);
    return true;
}

bool audio_recorder_emit_record_chunk(
    uint32_t record_chunk,
    uint32_t order,
    const char *metadata_line)
{
    RecorderChunkView view = {0};

    if (!audio_recorder_get_record_chunk_view(record_chunk, &view))
    {
        return false;
    }

    char header[96];

    snprintf(
        header,
        sizeof(header),
        "[CHUNK_START] %lu order=%lu\r\n",
        (unsigned long)RECORD_FRAMES,
        (unsigned long)order
    );
    audio_recorder_send_text(header);

    if (metadata_line != NULL)
    {
        audio_recorder_send_text(metadata_line);
    }

    for (uint32_t channel = 0U;
         (channel < RECORD_CHANNELS) && !s_transfer_abort;
         channel++)
    {
        if (!emit_channel_with_retry(channel, view.channel[channel]))
        {
            audio_recorder_send_text(
                "[CHUNK_ABORT] channel_transfer_failed\r\n"
            );
            return false;
        }
    }

    audio_recorder_send_text("[CHUNK_END]\r\n");

    const UartControl response =
        wait_uart_control(UART_CHUNK_ACK_TIMEOUT);

    if (response != UART_CONTROL_CONTINUE)
    {
        if (response != UART_CONTROL_STOP)
        {
            audio_recorder_send_text(
                "[CHUNK_ABORT] save_ack_timeout\r\n"
            );
        }
        return false;
    }

    return true;
}

bool audio_recorder_end_transfer(uint32_t chunk_count)
{
    char message[96];

    snprintf(
        message,
        sizeof(message),
        "[RECORD_DONE] chunks=%lu\r\n",
        (unsigned long)chunk_count
    );
    audio_recorder_send_text(message);

    return !s_transfer_abort;
}
