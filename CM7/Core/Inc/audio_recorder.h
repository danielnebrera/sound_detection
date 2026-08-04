/* =================================================================
 * audio_recorder.h
 *
 * Captures exactly 1 second from 4 simultaneous channels as int16.
 * The physical channel buffers are distributed across RAM_D1, RAM_D2
 * and DTCM. The detector reads the same buffers through a read-only
 * RecorderChunkView, so audio is not duplicated in memory.
 * ================================================================= */

#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#define RECORD_SECONDS      1U
#define RECORD_SAMPLE_RATE  44100U
#define RECORD_FRAMES       (RECORD_SECONDS * RECORD_SAMPLE_RATE)
#define RECORD_CHANNELS     4U
#define RECORD_N_CH         RECORD_CHANNELS

typedef struct
{
    const int16_t *channel[RECORD_CHANNELS];
    uint32_t frame_count;
} RecorderChunkView;

void audio_recorder_init(void);
void audio_recorder_accumulate(void);
bool audio_recorder_is_ready(void);
bool audio_recorder_get_chunk_view(RecorderChunkView *view);
bool audio_recorder_emit_and_reset(void);
bool audio_recorder_poll_stop(void);
bool audio_recorder_stop_requested(void);

#endif /* AUDIO_RECORDER_H */
