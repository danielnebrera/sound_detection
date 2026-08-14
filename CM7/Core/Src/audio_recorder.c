/* =================================================================
 * audio_recorder.c
 *
 * Productor-consumidor para cuatro canales PCM16 en SDRAM.
 *
 * Productor (ISR SAI/DMA):
 *   DMA A/B -> desentrelaza -> PCM16 -> slot SDRAM -> cola READY
 *
 * Consumidor (main):
 *   cola READY -> DSP/MFCC/TFLite -> libera calibracion o retiene audio
 * ================================================================= */

#include "audio_recorder.h"
#include "audio_capture.h"
#include "portenta_sdram.h"
#include "usart.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef huart1;

/*
 * Ring fisico en SDRAM validada.
 * Layout: ring[slot][channel][frame].
 */
static int16_t (*const s_ring_pcm)
    [RECORD_CHANNELS]
    [RECORD_FRAMES] =
        (int16_t (*)[RECORD_CHANNELS][RECORD_FRAMES])
        PORTENTA_SDRAM_BASE_ADDRESS;

#define AUDIO_RING_REQUIRED_BYTES \
    ((uint32_t)AUDIO_RING_SLOT_COUNT * \
     (uint32_t)RECORD_CHANNELS * \
     (uint32_t)RECORD_FRAMES * \
     (uint32_t)sizeof(int16_t))

#define UART_BLOCK_PAYLOAD_BYTES          8192U
#define UART_BLOCK_FALLBACK_BYTES         4096U
#define UART_BLOCK_FINAL_FALLBACK_BYTES   1024U
#define UART_BLOCK_LARGE_RETRIES             1U
#define UART_BLOCK_MEDIUM_RETRIES            2U
#define UART_BLOCK_MAX_RETRIES              12U
#define UART_BLOCK_ACK_TIMEOUT             1200U
#define UART_CHANNEL_SYNC_RETRIES             3U
#define UART_CHANNEL_SYNC_TIMEOUT           1500U
#define UART_CHUNK_ACK_TIMEOUT             60000U

/*
 * Staging interno para que CRC y UART lean exactamente la misma copia.
 * Evita dos lecturas independientes desde SDRAM durante cada bloque.
 */
#if defined(__GNUC__)
__attribute__((aligned(32)))
#endif
static uint8_t s_uart_tx_block[UART_BLOCK_PAYLOAD_BYTES];

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(RECORD_FRAMES == 44100U, "Recorder expects 44100 frames");
_Static_assert(RECORD_CHANNELS == 4U, "Recorder expects four channels");
_Static_assert(
    AUDIO_RING_REQUIRED_BYTES <= PORTENTA_SDRAM_SIZE_BYTES,
    "Audio ring exceeds Portenta SDRAM capacity"
);
#endif

static volatile AudioChunkDescriptor s_descriptors[AUDIO_RING_SLOT_COUNT];

/* Cola SPSC: productor ISR escribe head, consumidor main escribe tail. */
static uint8_t s_ready_slots[AUDIO_RING_SLOT_COUNT];
static volatile uint32_t s_ready_head = 0U;
static volatile uint32_t s_ready_tail = 0U;

/* Mapa estable record_chunk -> slot fisico retenido. */
static uint8_t s_record_slots[RECORD_MAX_OUTPUT_CHUNKS];

static volatile uint8_t s_write_slot = AUDIO_SLOT_INVALID;
static volatile uint32_t s_frame_in_slot = 0U;
static volatile uint32_t s_next_sequence = 0U;
static volatile uint64_t s_total_frames = 0U;
static volatile uint32_t s_recorded_chunks = 0U;

static volatile bool s_session_active = false;
static volatile bool s_stop_requested = false;
static volatile bool s_capture_closed = false;
static volatile AudioRecorderCloseReason s_close_reason = AUDIO_CLOSE_NONE;

static volatile bool s_stop_rx_armed = false;
static uint8_t s_uart_rx_byte = 0U;
static bool s_transfer_abort = false;

static volatile uint32_t s_chunks_completed = 0U;
static volatile uint32_t s_chunks_detected = 0U;
static volatile uint32_t s_ready_queue_high_water = 0U;
static volatile uint32_t s_ring_full_events = 0U;
static volatile uint32_t s_queue_full_events = 0U;
static volatile uint32_t s_copy_cycles_last = 0U;
static volatile uint32_t s_copy_cycles_max = 0U;
static volatile uint32_t s_detection_cycles_last = 0U;
static volatile uint32_t s_detection_cycles_max = 0U;

static void dwt_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint32_t minimum_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
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

static void reset_descriptor(uint32_t slot)
{
    /*
     * El estado FREE se publica al final. Asi una ISR no puede reutilizar
     * el slot mientras el foreground todavia limpia sus metadatos.
     */
    s_descriptors[slot].flags = AUDIO_SLOT_FLAG_NONE;
    s_descriptors[slot].sequence = 0U;
    s_descriptors[slot].record_order = 0U;
    s_descriptors[slot].first_frame = 0U;
    s_descriptors[slot].last_frame = 0U;
    s_descriptors[slot].copy_cycles_max = 0U;
    s_descriptors[slot].detection_cycles = 0U;
    __DMB();
    s_descriptors[slot].state = AUDIO_SLOT_FREE;
}

static uint32_t ready_depth_from_indices(uint32_t head, uint32_t tail)
{
    return head - tail;
}

static bool ready_queue_push_from_isr(uint8_t slot)
{
    const uint32_t head = s_ready_head;
    const uint32_t tail = s_ready_tail;
    const uint32_t depth = ready_depth_from_indices(head, tail);

    if (depth >= AUDIO_RING_SLOT_COUNT)
    {
        s_queue_full_events++;
        return false;
    }

    s_ready_slots[head % AUDIO_RING_SLOT_COUNT] = slot;
    __DMB();
    s_ready_head = head + 1U;

    const uint32_t new_depth = depth + 1U;
    if (new_depth > s_ready_queue_high_water)
    {
        s_ready_queue_high_water = new_depth;
    }

    return true;
}

static uint8_t find_free_slot_from_isr(uint8_t start_slot)
{
    for (uint32_t offset = 0U; offset < AUDIO_RING_SLOT_COUNT; offset++)
    {
        const uint8_t slot =
            (uint8_t)((start_slot + offset) % AUDIO_RING_SLOT_COUNT);

        if (s_descriptors[slot].state == AUDIO_SLOT_FREE)
        {
            return slot;
        }
    }

    return AUDIO_SLOT_INVALID;
}

static void prepare_slot_from_isr(uint8_t slot, uint32_t sequence)
{
    volatile AudioChunkDescriptor *descriptor =
        &s_descriptors[slot];

    descriptor->state = AUDIO_SLOT_WRITING;
    descriptor->flags =
        (sequence < RECORD_CALIBRATION_CHUNKS)
            ? AUDIO_SLOT_FLAG_CALIBRATION
            : AUDIO_SLOT_FLAG_RECORD;
    descriptor->sequence = sequence;
    descriptor->record_order = 0U;
    descriptor->first_frame = (uint64_t)sequence * RECORD_FRAMES;
    descriptor->last_frame = descriptor->first_frame;
    descriptor->copy_cycles_max = 0U;
    descriptor->detection_cycles = 0U;

    s_write_slot = slot;
    s_frame_in_slot = 0U;
}

static void close_capture_from_isr(AudioRecorderCloseReason reason)
{
    s_session_active = false;
    s_capture_closed = true;
    s_close_reason = reason;
    s_write_slot = AUDIO_SLOT_INVALID;
    __DMB();
}

static bool publish_completed_slot_from_isr(uint8_t slot)
{
    volatile AudioChunkDescriptor *descriptor =
        &s_descriptors[slot];

    descriptor->last_frame = descriptor->first_frame + RECORD_FRAMES - 1U;

    if ((descriptor->flags & AUDIO_SLOT_FLAG_RECORD) != 0U)
    {
        if (s_recorded_chunks >= RECORD_MAX_OUTPUT_CHUNKS)
        {
            return false;
        }

        const uint32_t record_index = s_recorded_chunks;
        descriptor->record_order = record_index + 1U;
        s_record_slots[record_index] = slot;
        s_recorded_chunks = record_index + 1U;
    }

    descriptor->state = AUDIO_SLOT_READY;
    __DMB();

    if (!ready_queue_push_from_isr(slot))
    {
        return false;
    }

    s_chunks_completed++;
    return true;
}

static bool should_close_on_boundary(void)
{
    if (s_recorded_chunks >= RECORD_MAX_OUTPUT_CHUNKS)
    {
        return true;
    }

    return s_stop_requested && (s_recorded_chunks >= 1U);
}

static void copy_frames_to_sdram_from_isr(
    uint8_t slot,
    uint32_t destination_frame,
    uint32_t source_frame,
    uint32_t frame_count,
    uint8_t half,
    const uint32_t *dma_a,
    const uint32_t *dma_b)
{
    const uint32_t half_offset =
        (half == 0U) ? 0U : (AUDIO_DMA_BUFFER_SIZE / 2U);

    int16_t *const ch0 = &s_ring_pcm[slot][0][destination_frame];
    int16_t *const ch1 = &s_ring_pcm[slot][1][destination_frame];
    int16_t *const ch2 = &s_ring_pcm[slot][2][destination_frame];
    int16_t *const ch3 = &s_ring_pcm[slot][3][destination_frame];

    for (uint32_t i = 0U; i < frame_count; i++)
    {
        const uint32_t source_index =
            half_offset +
            ((source_frame + i) * AUDIO_SLOTS_PER_FRAME);

        ch0[i] = pcm24_to_pcm16(dma_b[source_index + 1U]); /* Mic2 */
        ch1[i] = pcm24_to_pcm16(dma_a[source_index + 1U]); /* Mic4 */
        ch2[i] = pcm24_to_pcm16(dma_a[source_index]);      /* Mic3 */
        ch3[i] = pcm24_to_pcm16(dma_b[source_index]);      /* Mic1 */
    }
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

void audio_recorder_init(void)
{
    dwt_enable();

    for (uint32_t slot = 0U; slot < AUDIO_RING_SLOT_COUNT; slot++)
    {
        reset_descriptor(slot);
        s_ready_slots[slot] = AUDIO_SLOT_INVALID;
    }

    for (uint32_t i = 0U; i < RECORD_MAX_OUTPUT_CHUNKS; i++)
    {
        s_record_slots[i] = AUDIO_SLOT_INVALID;
    }

    s_ready_head = 0U;
    s_ready_tail = 0U;
    s_write_slot = AUDIO_SLOT_INVALID;
    s_frame_in_slot = 0U;
    s_next_sequence = 0U;
    s_total_frames = 0U;
    s_recorded_chunks = 0U;
    s_session_active = false;
    s_stop_requested = false;
    s_capture_closed = false;
    s_close_reason = AUDIO_CLOSE_NONE;
    s_stop_rx_armed = false;
    s_uart_rx_byte = 0U;
    s_transfer_abort = false;

    s_chunks_completed = 0U;
    s_chunks_detected = 0U;
    s_ready_queue_high_water = 0U;
    s_ring_full_events = 0U;
    s_queue_full_events = 0U;
    s_copy_cycles_last = 0U;
    s_copy_cycles_max = 0U;
    s_detection_cycles_last = 0U;
    s_detection_cycles_max = 0U;
}

bool audio_recorder_start_session(void)
{
    audio_recorder_init();

    const uint8_t first_slot = find_free_slot_from_isr(0U);
    if (first_slot == AUDIO_SLOT_INVALID)
    {
        s_close_reason = AUDIO_CLOSE_RING_FULL;
        s_capture_closed = true;
        return false;
    }

    prepare_slot_from_isr(first_slot, 0U);
    s_next_sequence = 1U;
    s_session_active = true;
    return true;
}

bool audio_recorder_on_dma_pair_from_isr(
    uint8_t half,
    const uint32_t *dma_a,
    const uint32_t *dma_b)
{
    if ((dma_a == NULL) || (dma_b == NULL) || (half >= AUDIO_DMA_HALVES))
    {
        close_capture_from_isr(AUDIO_CLOSE_PIPELINE_ERROR);
        return false;
    }

    if (!s_session_active || s_capture_closed)
    {
        return true;
    }

    const uint32_t start_cycles = DWT->CYCCNT;
    uint32_t source_offset = 0U;

    while ((source_offset < AUDIO_BUFFER_SIZE) && s_session_active)
    {
        const uint8_t slot = s_write_slot;

        if ((slot == AUDIO_SLOT_INVALID) ||
            (s_descriptors[slot].state != AUDIO_SLOT_WRITING))
        {
            close_capture_from_isr(AUDIO_CLOSE_PIPELINE_ERROR);
            return false;
        }

        const uint32_t source_remaining =
            AUDIO_BUFFER_SIZE - source_offset;
        const uint32_t chunk_remaining =
            RECORD_FRAMES - s_frame_in_slot;
        const uint32_t copy_count =
            minimum_u32(source_remaining, chunk_remaining);

        copy_frames_to_sdram_from_isr(
            slot,
            s_frame_in_slot,
            source_offset,
            copy_count,
            half,
            dma_a,
            dma_b
        );

        source_offset += copy_count;
        s_frame_in_slot += copy_count;
        s_total_frames += copy_count;

        if (s_frame_in_slot == RECORD_FRAMES)
        {
            if (!publish_completed_slot_from_isr(slot))
            {
                close_capture_from_isr(AUDIO_CLOSE_QUEUE_FULL);
                return false;
            }

            if (should_close_on_boundary())
            {
                const AudioRecorderCloseReason reason =
                    s_stop_requested
                        ? AUDIO_CLOSE_STOP_REQUESTED
                        : AUDIO_CLOSE_CAPACITY_REACHED;

                close_capture_from_isr(reason);
                break;
            }

            const uint8_t next_start =
                (uint8_t)((slot + 1U) % AUDIO_RING_SLOT_COUNT);
            const uint8_t next_slot =
                find_free_slot_from_isr(next_start);

            if (next_slot == AUDIO_SLOT_INVALID)
            {
                s_ring_full_events++;
                close_capture_from_isr(AUDIO_CLOSE_RING_FULL);
                break;
            }

            prepare_slot_from_isr(next_slot, s_next_sequence);
            s_next_sequence++;
        }
    }

    const uint32_t elapsed = DWT->CYCCNT - start_cycles;
    s_copy_cycles_last = elapsed;

    if (elapsed > s_copy_cycles_max)
    {
        s_copy_cycles_max = elapsed;
    }

    return true;
}

bool audio_recorder_arm_stop_receiver(void)
{
    s_uart_rx_byte = 0U;
    s_stop_rx_armed = true;

    if (HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U) != HAL_OK)
    {
        s_stop_rx_armed = false;
        return false;
    }

    return true;
}

void audio_recorder_disarm_stop_receiver(void)
{
    if (s_stop_rx_armed)
    {
        s_stop_rx_armed = false;
        (void)HAL_UART_AbortReceive_IT(&huart1);
    }
}

void audio_recorder_request_stop(void)
{
    s_stop_requested = true;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if ((huart != &huart1) || !s_stop_rx_armed)
    {
        return;
    }

    if (s_uart_rx_byte == (uint8_t)'S')
    {
        s_stop_requested = true;
    }

    s_uart_rx_byte = 0U;
    (void)HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if ((huart == &huart1) && s_stop_rx_armed)
    {
        s_uart_rx_byte = 0U;
        (void)HAL_UART_Receive_IT(&huart1, &s_uart_rx_byte, 1U);
    }
}

bool audio_recorder_stop_requested(void)
{
    return s_stop_requested;
}

bool audio_recorder_capture_closed(void)
{
    return s_capture_closed;
}

AudioRecorderCloseReason audio_recorder_close_reason(void)
{
    return s_close_reason;
}

const char *audio_recorder_close_reason_text(AudioRecorderCloseReason reason)
{
    switch (reason)
    {
        case AUDIO_CLOSE_NONE:             return "none";
        case AUDIO_CLOSE_STOP_REQUESTED:   return "stop_requested";
        case AUDIO_CLOSE_CAPACITY_REACHED: return "capacity_reached";
        case AUDIO_CLOSE_RING_FULL:        return "ring_full";
        case AUDIO_CLOSE_QUEUE_FULL:       return "queue_full";
        case AUDIO_CLOSE_PIPELINE_ERROR:   return "pipeline_error";
        default:                           return "unknown";
    }
}

bool audio_recorder_pop_ready_slot(uint8_t *slot_index)
{
    if (slot_index == NULL)
    {
        return false;
    }

    const uint32_t tail = s_ready_tail;
    const uint32_t head = s_ready_head;

    if (tail == head)
    {
        return false;
    }

    __DMB();
    const uint8_t slot = s_ready_slots[tail % AUDIO_RING_SLOT_COUNT];

    if ((slot >= AUDIO_RING_SLOT_COUNT) ||
        (s_descriptors[slot].state != AUDIO_SLOT_READY))
    {
        s_close_reason = AUDIO_CLOSE_PIPELINE_ERROR;
        s_capture_closed = true;
        s_session_active = false;
        return false;
    }

    s_ready_tail = tail + 1U;
    s_descriptors[slot].state = AUDIO_SLOT_PROCESSING;
    __DMB();

    *slot_index = slot;
    return true;
}

uint32_t audio_recorder_ready_count(void)
{
    return ready_depth_from_indices(s_ready_head, s_ready_tail);
}

bool audio_recorder_get_slot_descriptor(
    uint8_t slot_index,
    AudioChunkDescriptor *descriptor)
{
    if ((descriptor == NULL) || (slot_index >= AUDIO_RING_SLOT_COUNT))
    {
        return false;
    }

    *descriptor = s_descriptors[slot_index];
    return true;
}

bool audio_recorder_get_slot_view(
    uint8_t slot_index,
    RecorderChunkView *view)
{
    if ((view == NULL) || (slot_index >= AUDIO_RING_SLOT_COUNT))
    {
        return false;
    }

    const AudioSlotState state = s_descriptors[slot_index].state;

    if ((state != AUDIO_SLOT_READY) &&
        (state != AUDIO_SLOT_PROCESSING) &&
        (state != AUDIO_SLOT_PROCESSED))
    {
        return false;
    }

    for (uint32_t channel = 0U; channel < RECORD_CHANNELS; channel++)
    {
        view->channel[channel] = &s_ring_pcm[slot_index][channel][0];
    }

    view->frame_count = RECORD_FRAMES;
    return true;
}

bool audio_recorder_complete_processing(
    uint8_t slot_index,
    uint32_t detection_cycles)
{
    if ((slot_index >= AUDIO_RING_SLOT_COUNT) ||
        (s_descriptors[slot_index].state != AUDIO_SLOT_PROCESSING))
    {
        return false;
    }

    s_descriptors[slot_index].detection_cycles = detection_cycles;
    s_detection_cycles_last = detection_cycles;

    if (detection_cycles > s_detection_cycles_max)
    {
        s_detection_cycles_max = detection_cycles;
    }

    s_chunks_detected++;

    if ((s_descriptors[slot_index].flags &
         AUDIO_SLOT_FLAG_CALIBRATION) != 0U)
    {
        __DMB();
        reset_descriptor(slot_index);
    }
    else
    {
        s_descriptors[slot_index].state = AUDIO_SLOT_PROCESSED;
        __DMB();
    }

    return true;
}

uint32_t audio_recorder_recorded_chunks(void)
{
    return s_recorded_chunks;
}

uint64_t audio_recorder_total_frames(void)
{
    return s_total_frames;
}

bool audio_recorder_get_record_chunk_view(
    uint32_t record_chunk,
    RecorderChunkView *view)
{
    if ((record_chunk >= s_recorded_chunks) || (view == NULL))
    {
        return false;
    }

    const uint8_t slot = s_record_slots[record_chunk];

    if ((slot >= AUDIO_RING_SLOT_COUNT) ||
        (s_descriptors[slot].state != AUDIO_SLOT_PROCESSED))
    {
        return false;
    }

    return audio_recorder_get_slot_view(slot, view);
}

bool audio_recorder_get_record_descriptor(
    uint32_t record_chunk,
    AudioChunkDescriptor *descriptor)
{
    if ((record_chunk >= s_recorded_chunks) || (descriptor == NULL))
    {
        return false;
    }

    const uint8_t slot = s_record_slots[record_chunk];
    return audio_recorder_get_slot_descriptor(slot, descriptor);
}

void audio_recorder_get_stats(AudioRecorderStats *stats)
{
    if (stats == NULL)
    {
        return;
    }

    stats->frames_written = s_total_frames;
    stats->chunks_completed = s_chunks_completed;
    stats->chunks_detected = s_chunks_detected;
    stats->recorded_chunks = s_recorded_chunks;
    stats->ready_queue_depth = audio_recorder_ready_count();
    stats->ready_queue_high_water = s_ready_queue_high_water;
    stats->ring_full_events = s_ring_full_events;
    stats->queue_full_events = s_queue_full_events;
    stats->copy_cycles_last = s_copy_cycles_last;
    stats->copy_cycles_max = s_copy_cycles_max;
    stats->detection_cycles_last = s_detection_cycles_last;
    stats->detection_cycles_max = s_detection_cycles_max;
    stats->stop_requested = s_stop_requested;
    stats->capture_closed = s_capture_closed;
    stats->close_reason = s_close_reason;
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

static bool emit_block_header(
    uint32_t channel,
    uint32_t sequence,
    uint32_t offset,
    uint32_t payload_length,
    uint32_t payload_crc,
    uint32_t attempt)
{
    char header[128];

    const int length = snprintf(
        header,
        sizeof(header),
        "[BLK] ch=%lu seq=%lu off=%lu len=%lu crc=%08lX try=%lu\r\n",
        (unsigned long)channel,
        (unsigned long)sequence,
        (unsigned long)offset,
        (unsigned long)payload_length,
        (unsigned long)payload_crc,
        (unsigned long)attempt
    );

    if ((length <= 0) || ((size_t)length >= sizeof(header)))
    {
        return false;
    }

    return uart_transmit_checked(
        (const uint8_t *)header,
        (uint16_t)length
    );
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
        (payload_length > UART_BLOCK_PAYLOAD_BYTES))
    {
        return false;
    }

    /*
     * Congelar el bloque en RAM interna antes de calcular CRC y transmitir.
     * CRC y UART consumen exactamente la misma copia.
     */
    memcpy(s_uart_tx_block, payload, payload_length);
    __DMB();

    const uint32_t payload_crc =
        crc32_update(0U, s_uart_tx_block, payload_length);

    if (!emit_block_header(
            channel,
            sequence,
            offset,
            payload_length,
            payload_crc,
            attempt))
    {
        return false;
    }

    return uart_transmit_checked(
        s_uart_tx_block,
        (uint16_t)payload_length
    );
}

static uint32_t choose_block_length(
    uint32_t attempt,
    uint32_t remaining)
{
    uint32_t requested = UART_BLOCK_PAYLOAD_BYTES;

    if (attempt > UART_BLOCK_MEDIUM_RETRIES)
    {
        requested = UART_BLOCK_FINAL_FALLBACK_BYTES;
    }
    else if (attempt > UART_BLOCK_LARGE_RETRIES)
    {
        requested = UART_BLOCK_FALLBACK_BYTES;
    }

    return minimum_u32(requested, remaining);
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
            choose_block_length(attempt, remaining);

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
            wait_uart_control(UART_BLOCK_ACK_TIMEOUT);

        if (response == UART_CONTROL_ACK)
        {
            *accepted_length = block_length;
            return true;
        }

        if ((response == UART_CONTROL_STOP) || s_transfer_abort)
        {
            return false;
        }

        /*
         * NACK o timeout: se repite exactamente el mismo offset.
         * El fallback es inmediato: intento 1 usa 8192 bytes, intento 2 usa
         * 4096 bytes y desde el intento 3 usa 1024 bytes. Como no se envia
         * ningun bloque posterior antes del ACK, el receptor conserva siempre
         * la frontera del flujo aunque un bloque llegue incompleto.
         */
    }

    return false;
}

static bool transmit_text_with_ack(
    const uint8_t *text,
    uint16_t text_length)
{
    if ((text == NULL) || (text_length == 0U))
    {
        return false;
    }

    for (uint32_t attempt = 1U;
         attempt <= UART_CHANNEL_SYNC_RETRIES;
         attempt++)
    {
        if (!uart_transmit_checked(text, text_length))
        {
            return false;
        }

        const UartControl response =
            wait_uart_control(UART_CHANNEL_SYNC_TIMEOUT);

        if (response == UART_CONTROL_ACK)
        {
            return true;
        }

        if ((response == UART_CONTROL_STOP) || s_transfer_abort)
        {
            return false;
        }
    }

    return false;
}

static bool emit_channel_reliable(
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

    const uint8_t *payload = (const uint8_t *)samples;
    const uint32_t payload_size =
        RECORD_FRAMES * (uint32_t)sizeof(int16_t);

    char header[160];
    const int header_length = snprintf(
        header,
        sizeof(header),
        "[%s_BIN] bytes=%lu max_block=%lu fallback=%lu final=%lu ack=block\r\n",
        channel_names[channel],
        (unsigned long)payload_size,
        (unsigned long)UART_BLOCK_PAYLOAD_BYTES,
        (unsigned long)UART_BLOCK_FALLBACK_BYTES,
        (unsigned long)UART_BLOCK_FINAL_FALLBACK_BYTES
    );

    if ((header_length <= 0) ||
        ((size_t)header_length >= sizeof(header)) ||
        !transmit_text_with_ack(
            (const uint8_t *)header,
            (uint16_t)header_length))
    {
        return false;
    }

    uint32_t offset = 0U;
    uint32_t sequence = 0U;

    while ((offset < payload_size) && !s_transfer_abort)
    {
        uint32_t accepted_length = 0U;
        const uint32_t remaining = payload_size - offset;

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

    const int end_length = snprintf(
        header,
        sizeof(header),
        "[%s_END] sent=%lu bytes=%lu packets=%lu errors=0\r\n",
        channel_names[channel],
        (unsigned long)RECORD_FRAMES,
        (unsigned long)payload_size,
        (unsigned long)sequence
    );

    if ((end_length <= 0) || ((size_t)end_length >= sizeof(header)))
    {
        return false;
    }

    return transmit_text_with_ack(
        (const uint8_t *)header,
        (uint16_t)end_length
    );
}

bool audio_recorder_begin_transfer(uint32_t chunk_count)
{
    char message[192];

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

    snprintf(
        message,
        sizeof(message),
        "[UART_TX_CFG] baud=%lu block=%lu fallback=%lu final=%lu retries=%u ack_timeout_ms=%lu adaptive=1 staging=1 ack=per_block\r\n",
        (unsigned long)huart1.Init.BaudRate,
        (unsigned long)UART_BLOCK_PAYLOAD_BYTES,
        (unsigned long)UART_BLOCK_FALLBACK_BYTES,
        (unsigned long)UART_BLOCK_FINAL_FALLBACK_BYTES,
        (unsigned)UART_BLOCK_MAX_RETRIES,
        (unsigned long)UART_BLOCK_ACK_TIMEOUT
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
        if (!emit_channel_reliable(channel, view.channel[channel]))
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
