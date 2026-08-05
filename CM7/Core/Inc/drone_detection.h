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

bool drone_detection_init(void);
bool drone_detection_process_window(const RecorderChunkView *window);
bool drone_detection_get_last_result(DroneDetectionResult *result);

#ifdef __cplusplus
}
#endif

#endif /* DRONE_DETECTION_H */
