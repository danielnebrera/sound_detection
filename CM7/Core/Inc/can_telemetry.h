
#ifndef CAN_TELEMETRY_H
#define CAN_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>
#include "fdcan.h"
#include "drone_detection.h"
#include "track3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CAN clasico 11-bit, 500 kbit/s (segun fdcan.c / .ioc actual).
 * ID PROVISIONAL: confirmar que 0x450 esta libre en la red real.
 * DLC=8, little endian:
 *   [0:1] azimut fisico centideg 0..35999; 0xFFFF sin direccion
 *   [2:3] elevacion centideg int16, little endian; 0x7FFF sin direccion
 *   [4]   probabilidad detector 0..100 (% de fused_probability)
 *   [5]   intensidad RELATIVA 0..100; 0..30 dB sobre baseline
 *   [6]   bits: 0 deteccion, 1 direccion disponible,
 *               2 medicion TRACK3D utilizada, 3 coasting
 *   [7]   contador de trama, modulo 256
 * WARNING: la escala de intensidad es PROVISIONAL, NO dB SPL.
 * En ausencia de TOA valido puede salir una direccion predicha en COASTING,
 * marcada con bit 3, pero nunca con bit 2 como medicion nueva.
 */
#define CAN_TELEMETRY_STD_ID           0x450U
#define CAN_TELEMETRY_INVALID_AZ       0xFFFFU
#define CAN_TELEMETRY_INVALID_ELEV     0x7FFFU
#define CAN_TELEMETRY_REL_DB_FULL_SCALE 30.0f

#define CAN_TELEMETRY_F_DETECTED       (1U << 0)
#define CAN_TELEMETRY_F_DIRECTION      (1U << 1)
#define CAN_TELEMETRY_F_MEASUREMENT    (1U << 2)
#define CAN_TELEMETRY_F_COASTING       (1U << 3)

typedef struct {
    uint32_t sent;
    uint32_t skipped_fifo_full;
    uint32_t hal_errors;
    uint8_t started;
} CanTelemetryStats;

/* Devuelve false si HAL_FDCAN_Start falla. No espera ni reintenta. */
bool can_telemetry_start(FDCAN_HandleTypeDef *hfdcan);

/* Invocar solo desde foreground, una vez por segundo util procesado. */
void can_telemetry_publish(const DroneDetectionResult *detection,
                           const Track3dOutput *track);

void can_telemetry_get_stats(CanTelemetryStats *out);

#ifdef __cplusplus
}
#endif
#endif /* CAN_TELEMETRY_H */
