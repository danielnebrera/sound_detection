#ifndef DRONE_DETECTION_H
#define DRONE_DETECTION_H

#include <stdbool.h>
#include <stdint.h>
#include "audio_recorder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    float probability[RECORD_CHANNELS];
    float dbfs[RECORD_CHANNELS];
    float delta_dbfs[RECORD_CHANNELS];
    float fused_probability;
    float ema;

    uint8_t valid[RECORD_CHANNELS];
    uint8_t acoustic_active[RECORD_CHANNELS];
    uint8_t alert_level;     /* 0=sin alerta, 1=mantenida, 2=lejana, 3=cercana */
    uint8_t baseline_ready;
} DroneDetectionResult;

typedef struct
{
    uint32_t preprocess_last_cycles[RECORD_CHANNELS];
    uint32_t mfcc_last_cycles[RECORD_CHANNELS];
    uint32_t tflite_last_cycles[RECORD_CHANNELS];
    uint32_t total_last_cycles[RECORD_CHANNELS];

    uint32_t preprocess_max_cycles[RECORD_CHANNELS];
    uint32_t mfcc_max_cycles[RECORD_CHANNELS];
    uint32_t tflite_max_cycles[RECORD_CHANNELS];
    uint32_t total_max_cycles[RECORD_CHANNELS];

    uint32_t mfcc_runs[RECORD_CHANNELS];
    uint32_t tflite_runs[RECORD_CHANNELS];

    float calibration_dbfs[RECORD_CHANNELS][3];
    float gate_baseline_dbfs[RECORD_CHANNELS];
    float gate_margin_db[RECORD_CHANNELS];
    float gate_last_dbfs[RECORD_CHANNELS];
    float gate_last_delta_db[RECORD_CHANNELS];
    float gate_max_delta_db[RECORD_CHANNELS];
    uint32_t gate_windows[RECORD_CHANNELS];
    uint32_t gate_active_count[RECORD_CHANNELS];

    uint32_t window_last_cycles;
    uint32_t window_max_cycles;
    uint32_t windows_profiled;
} DroneDetectionProfile;

bool drone_detection_init(void);
bool drone_detection_process_window(const RecorderChunkView *window);
bool drone_detection_get_last_result(DroneDetectionResult *result);
bool drone_detection_get_profile(DroneDetectionProfile *profile);

#ifdef __cplusplus
}
#endif

#endif /* DRONE_DETECTION_H */
