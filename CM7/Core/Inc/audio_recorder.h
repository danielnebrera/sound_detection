/**
 * @file audio_recorder.h
 * @brief Productor-consumidor de audio con ring PCM16 en SDRAM.
 *
 * - El productor se ejecuta desde las callbacks SAI/DMA.
 * - Cada slot contiene exactamente un segundo por cuatro canales.
 * - El consumidor procesa los slots READY desde main().
 * - Los tres primeros slots logicos calibran el detector y se liberan.
 * - Hasta 16 chunks utiles se conservan para transferir al detener.
 */

#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#define RECORD_SAMPLE_RATE            44100U
#define RECORD_FRAMES                 RECORD_SAMPLE_RATE
#define RECORD_SECONDS                1U
#define RECORD_CHANNELS               4U
#define RECORD_N_CH                   RECORD_CHANNELS

#define RECORD_CALIBRATION_CHUNKS      3U
#define AUDIO_RING_SLOT_COUNT          16U
#define RECORD_MAX_OUTPUT_CHUNKS       AUDIO_RING_SLOT_COUNT
#define AUDIO_SLOT_INVALID             0xFFU

typedef struct
{
    const int16_t *channel[RECORD_CHANNELS];
    uint32_t frame_count;
} RecorderChunkView;

typedef enum
{
    AUDIO_SLOT_FREE = 0,
    AUDIO_SLOT_WRITING,
    AUDIO_SLOT_READY,
    AUDIO_SLOT_PROCESSING,
    AUDIO_SLOT_PROCESSED
} AudioSlotState;

enum
{
    AUDIO_SLOT_FLAG_NONE        = 0U,
    AUDIO_SLOT_FLAG_CALIBRATION = (1U << 0),
    AUDIO_SLOT_FLAG_RECORD      = (1U << 1)
};

typedef enum
{
    AUDIO_CLOSE_NONE = 0,
    AUDIO_CLOSE_STOP_REQUESTED,
    AUDIO_CLOSE_CAPACITY_REACHED,
    AUDIO_CLOSE_RING_FULL,
    AUDIO_CLOSE_QUEUE_FULL,
    AUDIO_CLOSE_PIPELINE_ERROR
} AudioRecorderCloseReason;

typedef struct
{
    volatile AudioSlotState state;
    uint32_t flags;
    uint32_t sequence;
    uint32_t record_order;
    uint64_t first_frame;
    uint64_t last_frame;
    uint32_t copy_cycles_max;
    uint32_t detection_cycles;
} AudioChunkDescriptor;

typedef struct
{
    uint64_t frames_written;
    uint32_t chunks_completed;
    uint32_t chunks_detected;
    uint32_t recorded_chunks;
    uint32_t ready_queue_depth;
    uint32_t ready_queue_high_water;
    uint32_t ring_full_events;
    uint32_t queue_full_events;
    uint32_t copy_cycles_last;
    uint32_t copy_cycles_max;
    uint32_t detection_cycles_last;
    uint32_t detection_cycles_max;
    bool stop_requested;
    bool capture_closed;
    AudioRecorderCloseReason close_reason;
} AudioRecorderStats;

void audio_recorder_init(void);
bool audio_recorder_start_session(void);

/* Productor: solo se llama desde callbacks SAI/DMA. */
bool audio_recorder_on_dma_pair_from_isr(
    uint8_t half,
    const uint32_t *dma_a,
    const uint32_t *dma_b
);

/* Comando S por interrupcion UART durante captura. */
bool audio_recorder_arm_stop_receiver(void);
void audio_recorder_disarm_stop_receiver(void);
void audio_recorder_request_stop(void);

bool audio_recorder_stop_requested(void);
bool audio_recorder_capture_closed(void);
AudioRecorderCloseReason audio_recorder_close_reason(void);
const char *audio_recorder_close_reason_text(AudioRecorderCloseReason reason);

/* Cola SPSC productor ISR -> consumidor main. */
bool audio_recorder_pop_ready_slot(uint8_t *slot_index);
uint32_t audio_recorder_ready_count(void);

bool audio_recorder_get_slot_descriptor(
    uint8_t slot_index,
    AudioChunkDescriptor *descriptor
);

bool audio_recorder_get_slot_view(
    uint8_t slot_index,
    RecorderChunkView *view
);

bool audio_recorder_complete_processing(
    uint8_t slot_index,
    uint32_t detection_cycles
);

uint32_t audio_recorder_recorded_chunks(void);
uint64_t audio_recorder_total_frames(void);

bool audio_recorder_get_record_chunk_view(
    uint32_t record_chunk,
    RecorderChunkView *view
);

bool audio_recorder_get_record_descriptor(
    uint32_t record_chunk,
    AudioChunkDescriptor *descriptor
);

void audio_recorder_get_stats(AudioRecorderStats *stats);

/* Protocolo UART posterior a la captura. */
void audio_recorder_send_text(const char *text);
bool audio_recorder_begin_transfer(uint32_t chunk_count);
bool audio_recorder_emit_record_chunk(
    uint32_t record_chunk,
    uint32_t order,
    const char *metadata_line
);
bool audio_recorder_end_transfer(uint32_t chunk_count);

#endif /* AUDIO_RECORDER_H */
