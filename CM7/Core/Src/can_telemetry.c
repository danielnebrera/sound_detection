
#include "can_telemetry.h"

#include <math.h>
#include <stddef.h>

static FDCAN_HandleTypeDef *s_fdcan = NULL;
static CanTelemetryStats s_stats = {0};
static uint8_t s_sequence = 0U;

static uint8_t round_percent(float x)
{
    if (!isfinite(x) || x <= 0.0f) return 0U;
    if (x >= 100.0f) return 100U;
    return (uint8_t)(x + 0.5f);
}

bool can_telemetry_start(FDCAN_HandleTypeDef *hfdcan)
{
    s_fdcan = NULL;
    s_sequence = 0U;
    s_stats = (CanTelemetryStats){0};
    if (hfdcan == NULL) return false;
    if (HAL_FDCAN_Start(hfdcan) != HAL_OK) {
        s_stats.hal_errors++;
        return false;
    }
    s_fdcan = hfdcan;
    s_stats.started = 1U;
    return true;
}

void can_telemetry_publish(const DroneDetectionResult *detection,
                           const Track3dOutput *track)
{
    if (detection == NULL || track == NULL || s_fdcan == NULL) return;

    uint8_t bytes[8] = {0};
    uint16_t az = CAN_TELEMETRY_INVALID_AZ;
    uint16_t elev = CAN_TELEMETRY_INVALID_ELEV;
    uint8_t flags = 0U;

    if (detection->alert_level != 0U) flags |= CAN_TELEMETRY_F_DETECTED;

    /* No atribuir nueva medicion al coasting. Respetar la validez de TRACK3D. */
    if ((track->valid != 0U) && isfinite(track->azimuth_physical_deg) &&
        isfinite(track->elevation_deg)) {
        float az_deg = fmodf(track->azimuth_physical_deg, 360.0f);
        if (az_deg < 0.0f) az_deg += 360.0f;
        if (track->elevation_deg >= -90.0f && track->elevation_deg <= 90.0f) {
            unsigned az100 = (unsigned)(az_deg * 100.0f + 0.5f);
            if (az100 >= 36000U) az100 = 0U;
            const float elevation100 = track->elevation_deg * 100.0f;
            const int32_t elev100 = (int32_t)(elevation100 >= 0.0f ? elevation100 + 0.5f : elevation100 - 0.5f);
            az = (uint16_t)az100;
            elev = (uint16_t)(int16_t)elev100;
            flags |= CAN_TELEMETRY_F_DIRECTION;
            if (track->used != 0U) flags |= CAN_TELEMETRY_F_MEASUREMENT;
            if (track->state == TRACK3D_COASTING) flags |= CAN_TELEMETRY_F_COASTING;
        }
    }

    bytes[0] = (uint8_t)(az & 0xFFU);
    bytes[1] = (uint8_t)(az >> 8);
    bytes[2] = (uint8_t)(elev & 0xFFU);
    bytes[3] = (uint8_t)(elev >> 8);
    bytes[4] = round_percent(detection->fused_probability * 100.0f);

    /* Incremento relativo maximo de canal: no equivale a dB SPL. */
    float delta_max = 0.0f;
    for (uint32_t ch = 0U; ch < RECORD_CHANNELS; ++ch) {
        if (detection->valid[ch] != 0U && isfinite(detection->delta_dbfs[ch]) &&
            detection->delta_dbfs[ch] > delta_max) {
            delta_max = detection->delta_dbfs[ch];
        }
    }
    bytes[5] = round_percent(delta_max * (100.0f / CAN_TELEMETRY_REL_DB_FULL_SCALE));
    bytes[6] = flags;
    bytes[7] = s_sequence++;  /* avanza por ventana, incluso si se descarta TX */

    if (HAL_FDCAN_GetTxFifoFreeLevel(s_fdcan) == 0U) {
        s_stats.skipped_fifo_full++;
        return;
    }

    FDCAN_TxHeaderTypeDef hdr = {0};
    hdr.Identifier = CAN_TELEMETRY_STD_ID;
    hdr.IdType = FDCAN_STANDARD_ID;
    hdr.TxFrameType = FDCAN_DATA_FRAME;
    hdr.DataLength = FDCAN_DLC_BYTES_8;
    hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    hdr.BitRateSwitch = FDCAN_BRS_OFF;
    hdr.FDFormat = FDCAN_CLASSIC_CAN;
    hdr.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    hdr.MessageMarker = 0U;

    if (HAL_FDCAN_AddMessageToTxFifoQ(s_fdcan, &hdr, bytes) == HAL_OK) {
        s_stats.sent++;
    } else {
        s_stats.hal_errors++;
    }
}

void can_telemetry_get_stats(CanTelemetryStats *out)
{
    if (out != NULL) *out = s_stats;
}
