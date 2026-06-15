#ifndef DRONE_DETECTION_H
#define DRONE_DETECTION_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool drone_detection_init(void);
void drone_detection_accumulate(void);
bool drone_detection_is_ready(void);
void drone_detection_process(void);

#ifdef __cplusplus
}
#endif

#endif /* DRONE_DETECTION_H */
