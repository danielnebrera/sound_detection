#ifndef CAN_SENDER_H
#define CAN_SENDER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inicializa FDCAN1 y arranca el bus.
 * Llamar una vez en main() después de MX_FDCAN1_Init().
 * @return true si OK
 */
bool can_sender_init(void);

/**
 * Transmite los datos de detección por CAN.
 * Envía 2 tramas:
 *   ID 0x100: [p_mic1, p_mic2, p_mic3, p_mic4] — 4 floats = 8 bytes (empaquetado)
 *   ID 0x101: [ema, alerta, db_mic1, db_mic2]  — datos de estado
 *
 * @param p_ch      Array de 4 probabilidades [0.0-1.0]
 * @param db_ch     Array de 4 niveles dBFS
 * @param ema       Valor EMA actual
 * @param alerta    0=sin dron, 1=rastreando, 2=naranja, 3=roja
 */
void can_sender_transmit(const float *p_ch, const float *db_ch,
                         float ema, uint8_t alerta);

#ifdef __cplusplus
}
#endif

#endif /* CAN_SENDER_H */