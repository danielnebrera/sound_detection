#ifndef TRACK3D_H
#define TRACK3D_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* R2: prediccion 3D con velocidad angular; interfaz compatible con R1. */
typedef enum {
    TRACK3D_NO_MEASUREMENT=0, TRACK3D_ACQUIRED,
    TRACK3D_TRACKING, TRACK3D_COASTING,
    TRACK3D_REACQUIRED, TRACK3D_LOST
} Track3dState;
typedef struct {
    float x, y, z;              /* X oeste, Y norte, Z arriba */
    float azimuth_native_deg;    /* firmware: 0 NORTE, 90 RIGHT */
    float azimuth_physical_deg;  /* 0 NORTE, 90 OESTE */
    float elevation_deg;
    float innovation_deg;       /* significativo si innovation_valid */
    uint32_t seq;
    uint8_t valid, used, rejected, innovation_valid;
    Track3dState state;
} Track3dOutput;
void track3d_reset(void);
Track3dOutput track3d_step(uint32_t seq, uint8_t toa_valid,
                           uint8_t eligible, uint8_t tetra_fit_ok,
                           float azimuth_native_deg, float elevation_deg);
const char *track3d_state_text(Track3dState state);
#ifdef __cplusplus
}
#endif
#endif
