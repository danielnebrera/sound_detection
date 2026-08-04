#ifndef DRONE_DETECTION_H
#define DRONE_DETECTION_H

#include <stdbool.h>
#include "audio_recorder.h"

#ifdef __cplusplus
extern "C" {
#endif

bool drone_detection_init(void);
bool drone_detection_process_window(const RecorderChunkView *window);

#ifdef __cplusplus
}
#endif

#endif /* DRONE_DETECTION_H */
