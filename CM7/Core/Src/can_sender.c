/* =================================================================
 * can_sender.c
 * Transmisión de datos de detección de drones por FDCAN1
 * Portenta H7 — STM32H747 (CM7)
 *
 * Tramas CAN transmitidas:
 *   ID 0x100 (8 bytes): probabilidades de los 4 micrófonos
 *     byte[0-1]: Mic1 p×1000 (uint16)
 *     byte[2-3]: Mic2 p×1000 (uint16)
 *     byte[4-5]: Mic3 p×1000 (uint16)
 *     byte[6-7]: Mic4 p×1000 (uint16)
 *
 *   ID 0x101 (8 bytes): estado general
 *     byte[0-1]: EMA×1000 (uint16)
 *     byte[2]:   alerta (0=nada,1=rastreando,2=naranja,3=roja)
 *     byte[3]:   reservado
 *     byte[4-5]: dBFS Mic1 como int16 (×10 para 1 decimal)
 *     byte[6-7]: dBFS Mic2 como int16 (×10 para 1 decimal)
 * ================================================================= */

#include "can_sender.h"
#include "fdcan.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Handle FDCAN1 generado por CubeMX */
extern FDCAN_HandleTypeDef hfdcan1;

static bool s_inited = false;

/* ── Inicialización ──────────────────────────────────────────── */
bool can_sender_init(void)
{
    if (s_inited) return true;

    /* Arrancar FDCAN1 */
    if (HAL_FDCAN_Start(&hfdcan1) != HAL_OK) {
        printf("[CAN] ERROR: HAL_FDCAN_Start fallo\r\n");
        return false;
    }

    s_inited = true;
    printf("[CAN] FDCAN1 iniciado — 500 kbps Classic CAN\r\n");
    return true;
}

/* ── Helper: enviar una trama ────────────────────────────────── */
static bool can_send_frame(uint32_t id, const uint8_t *data, uint8_t len)
{
    FDCAN_TxHeaderTypeDef tx_header = {0};
    tx_header.Identifier          = id;
    tx_header.IdType              = FDCAN_STANDARD_ID;
    tx_header.TxFrameType         = FDCAN_DATA_FRAME;
    tx_header.DataLength          = FDCAN_DLC_BYTES_8;
    tx_header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx_header.BitRateSwitch       = FDCAN_BRS_OFF;
    tx_header.FDFormat            = FDCAN_CLASSIC_CAN;
    tx_header.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
    tx_header.MessageMarker       = 0;

    (void)len; /* siempre 8 bytes en Classic CAN */

    if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &tx_header, (uint8_t*)data) != HAL_OK) {
        printf("[CAN] ERROR: TX fallo (ID=0x%03lX)\r\n", id);
        return false;
    }
    return true;
}

/* ── Transmisión principal ───────────────────────────────────── */
void can_sender_transmit(const float *p_ch, const float *db_ch,
                         float ema, uint8_t alerta)
{
    if (!s_inited) return;

    uint8_t buf[8];

    /* ── Trama 0x100: probabilidades de los 4 micrófonos ─────── */
    /* Escalar [0.0-1.0] → [0-1000] como uint16 para enviar en 2 bytes */
    for (int i = 0; i < 4; i++) {
        float p = p_ch[i];
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        uint16_t val = (uint16_t)(p * 1000.0f);
        buf[i*2]     = (uint8_t)(val >> 8);
        buf[i*2 + 1] = (uint8_t)(val & 0xFF);
    }
    can_send_frame(0x100, buf, 8);

    /* ── Trama 0x101: estado general ─────────────────────────── */
    memset(buf, 0, 8);

    /* EMA × 1000 → uint16 */
    float e = ema < 0.0f ? 0.0f : (ema > 1.0f ? 1.0f : ema);
    uint16_t ema_val = (uint16_t)(e * 1000.0f);
    buf[0] = (uint8_t)(ema_val >> 8);
    buf[1] = (uint8_t)(ema_val & 0xFF);

    /* Alerta */
    buf[2] = alerta;
    buf[3] = 0x00;  /* reservado */

    /* dBFS Mic1 y Mic2 × 10 → int16 */
    int16_t db1 = (int16_t)(db_ch[0] * 10.0f);
    int16_t db2 = (int16_t)(db_ch[1] * 10.0f);
    buf[4] = (uint8_t)((uint16_t)db1 >> 8);
    buf[5] = (uint8_t)((uint16_t)db1 & 0xFF);
    buf[6] = (uint8_t)((uint16_t)db2 >> 8);
    buf[7] = (uint8_t)((uint16_t)db2 & 0xFF);

    can_send_frame(0x101, buf, 8);
}