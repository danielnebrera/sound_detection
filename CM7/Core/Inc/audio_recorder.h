/* =================================================================
 * audio_recorder.h
 *
 * Grabacion continua de cuatro microfonos en la SDRAM integrada.
 * Los primeros 3 segundos se reservan para calibrar el baseline del
 * detector. Los siguientes segundos se entregan como chunks de salida.
 * ================================================================= */

#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#define RECORD_SAMPLE_RATE          44100U
#define RECORD_FRAMES               RECORD_SAMPLE_RATE
#define RECORD_SECONDS              1U
#define RECORD_CHANNELS             4U
#define RECORD_N_CH                 RECORD_CHANNELS

#define RECORD_CALIBRATION_CHUNKS    3U
#define RECORD_MAX_OUTPUT_CHUNKS     10U
#define RECORD_TOTAL_STORAGE_CHUNKS \
    (RECORD_CALIBRATION_CHUNKS + RECORD_MAX_OUTPUT_CHUNKS)

#define RECORD_TOTAL_STORAGE_FRAMES \
    (RECORD_TOTAL_STORAGE_CHUNKS * RECORD_FRAMES)

typedef struct
{
    const int16_t *channel[RECORD_CHANNELS];
    uint32_t frame_count;
} RecorderChunkView;

void audio_recorder_init(void);

/* Captura */
bool audio_recorder_poll_stop(void);
bool audio_recorder_accumulate(void);
bool audio_recorder_capture_complete(void);
bool audio_recorder_stop_requested(void);
uint32_t audio_recorder_total_frames(void);
uint32_t audio_recorder_total_completed_chunks(void);
uint32_t audio_recorder_recorded_chunks(void);

/* Vistas de los chunks almacenados en SDRAM. */
bool audio_recorder_get_absolute_chunk_view(
    uint32_t absolute_chunk,
    RecorderChunkView *view
);

bool audio_recorder_get_record_chunk_view(
    uint32_t record_chunk,
    RecorderChunkView *view
);

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
