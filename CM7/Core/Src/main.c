/* =================================================================
 * main.c - Productor/consumidor SDRAM v2.1.7
 *
 * Flujo:
 *   1. Inicializa SDRAM y detector antes de capturar.
 *   2. SAI/DMA llena buffers D2; la ruta de captura publica PCM16 en el ring SDRAM.
 *   3. Cada segundo exacto se publica como slot READY.
 *   4. main procesa MFCC/TFLite mientras DMA captura el siguiente segundo.
 *   5. La captura continua sin limite de ordenes.
 *   6. STOP por UART cierra exactamente al terminar el segundo en curso.
 *   7. No se retiene ni transfiere audio: cada slot procesado vuelve a FREE.
 * ================================================================= */

#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "sai.h"
#include "usart.h"
#include "gpio.h"

#include "audio_capture.h"
#include "audio_recorder.h"

#ifndef AUDIO_RECORDER_LIVE_MODE
#error "TOA-1E requires the LIVE audio_recorder.h"
#endif

#if (AUDIO_RECORDER_LIVE_MODE != 1U)
#error "TOA-1E requires AUDIO_RECORDER_LIVE_MODE=1"
#endif

#ifndef AUDIO_RECORDER_LIVE_API_REV
#error "TOA-1E requires AUDIO_RECORDER_LIVE_API_REV"
#endif

#if (AUDIO_RECORDER_LIVE_API_REV != 0x0001E102UL)
#error "TOA-1E main/audio_recorder.h LIVE API revision mismatch"
#endif
#include "drone_detection.h"
#include "model_runner_stm32.h"
#include "portenta_sdram.h"
#include "toa_localizer.h"
#include "track3d.h"
#include "stm32h7xx.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MPU_Config(void);

#define SDRAM_RUN_QUICK_BOOT_TEST  0

/*
 * 0: no emitir detecciones durante captura.
 * 1: emitir solo actividad acustica o alerta (recomendado).
 * 2: emitir todos los chunks para diagnostico.
 */
#define LIVE_DETECTION_LOG_MODE     2

#define LED_ON(pin)     HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_RESET)
#define LED_OFF(pin)    HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_SET)
#define LED_TOGGLE(pin) HAL_GPIO_TogglePin(GPIOK, pin)

static bool s_audio_running = false;
static uint32_t s_toa_cycles_max = 0U;
static uint32_t s_processing_cycles_max = 0U;
static uint32_t s_no_target_count = 0U;
static uint32_t s_target_valid_count = 0U;
static uint32_t s_target_invalid_count = 0U;
static uint32_t s_conf_high_count = 0U;
static uint32_t s_conf_medium_count = 0U;
static uint32_t s_conf_low_count = 0U;
static uint32_t s_track_eligible_count = 0U;

typedef struct
{
    uint32_t sai_a_cr1;
    uint32_t sai_a_cr2;
    uint32_t sai_a_frcr;
    uint32_t sai_a_slotr;
    uint32_t sai_a_imr;
    uint32_t sai_a_sr;

    uint32_t sai_b_cr1;
    uint32_t sai_b_cr2;
    uint32_t sai_b_frcr;
    uint32_t sai_b_slotr;
    uint32_t sai_b_imr;
    uint32_t sai_b_sr;

    uint32_t dma0_cr;
    uint32_t dma0_ndtr;
    uint32_t dma0_par;
    uint32_t dma0_m0ar;
    uint32_t dma0_fcr;

    uint32_t dma1_cr;
    uint32_t dma1_ndtr;
    uint32_t dma1_par;
    uint32_t dma1_m0ar;
    uint32_t dma1_fcr;

    uint32_t gpioi_moder;
    uint32_t gpioi_ospeedr;
    uint32_t gpioi_pupdr;
    uint32_t gpioi_afrl;

    uint32_t gpiog_moder;
    uint32_t gpiog_ospeedr;
    uint32_t gpiog_pupdr;
    uint32_t gpiog_afrh;

    uint32_t rcc_d2ccip1r;

    uint32_t hsai_a_state;
    uint32_t hsai_a_error;
    uint32_t hsai_b_state;
    uint32_t hsai_b_error;

    uint32_t hdma_a_state;
    uint32_t hdma_a_error;
    uint32_t hdma_b_state;
    uint32_t hdma_b_error;
} SaiDmaDiagSnapshot;

static SaiDmaDiagSnapshot s_diag_pre_start;
static SaiDmaDiagSnapshot s_diag_after_start;
static SaiDmaDiagSnapshot s_diag_after_stop;
static void uart_send(const char *text);
static void sai_dma_diag_snapshot(SaiDmaDiagSnapshot *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }

    snapshot->sai_a_cr1   = SAI2_Block_A->CR1;
    snapshot->sai_a_cr2   = SAI2_Block_A->CR2;
    snapshot->sai_a_frcr  = SAI2_Block_A->FRCR;
    snapshot->sai_a_slotr = SAI2_Block_A->SLOTR;
    snapshot->sai_a_imr   = SAI2_Block_A->IMR;
    snapshot->sai_a_sr    = SAI2_Block_A->SR;

    snapshot->sai_b_cr1   = SAI2_Block_B->CR1;
    snapshot->sai_b_cr2   = SAI2_Block_B->CR2;
    snapshot->sai_b_frcr  = SAI2_Block_B->FRCR;
    snapshot->sai_b_slotr = SAI2_Block_B->SLOTR;
    snapshot->sai_b_imr   = SAI2_Block_B->IMR;
    snapshot->sai_b_sr    = SAI2_Block_B->SR;

    snapshot->dma0_cr   = DMA1_Stream0->CR;
    snapshot->dma0_ndtr = DMA1_Stream0->NDTR;
    snapshot->dma0_par  = DMA1_Stream0->PAR;
    snapshot->dma0_m0ar = DMA1_Stream0->M0AR;
    snapshot->dma0_fcr  = DMA1_Stream0->FCR;

    snapshot->dma1_cr   = DMA1_Stream1->CR;
    snapshot->dma1_ndtr = DMA1_Stream1->NDTR;
    snapshot->dma1_par  = DMA1_Stream1->PAR;
    snapshot->dma1_m0ar = DMA1_Stream1->M0AR;
    snapshot->dma1_fcr  = DMA1_Stream1->FCR;

    snapshot->gpioi_moder   = GPIOI->MODER;
    snapshot->gpioi_ospeedr = GPIOI->OSPEEDR;
    snapshot->gpioi_pupdr   = GPIOI->PUPDR;
    snapshot->gpioi_afrl    = GPIOI->AFR[0];

    snapshot->gpiog_moder   = GPIOG->MODER;
    snapshot->gpiog_ospeedr = GPIOG->OSPEEDR;
    snapshot->gpiog_pupdr   = GPIOG->PUPDR;
    snapshot->gpiog_afrh    = GPIOG->AFR[1];

    snapshot->rcc_d2ccip1r = RCC->D2CCIP1R;

    snapshot->hsai_a_state = (uint32_t)hsai_BlockA2.State;
    snapshot->hsai_a_error = (uint32_t)hsai_BlockA2.ErrorCode;
    snapshot->hsai_b_state = (uint32_t)hsai_BlockB2.State;
    snapshot->hsai_b_error = (uint32_t)hsai_BlockB2.ErrorCode;

    snapshot->hdma_a_state = (uint32_t)hdma_sai2_a.State;
    snapshot->hdma_a_error = (uint32_t)hdma_sai2_a.ErrorCode;
    snapshot->hdma_b_state = (uint32_t)hdma_sai2_b.State;
    snapshot->hdma_b_error = (uint32_t)hdma_sai2_b.ErrorCode;
}

static void sai_dma_diag_emit_one(
    const char *phase,
    const SaiDmaDiagSnapshot *s)
{
    char message[256];

    if ((phase == NULL) || (s == NULL))
    {
        return;
    }

    snprintf(
        message,
        sizeof(message),
        "[REG_SAI_A] phase=%s CR1=%08lX CR2=%08lX FRCR=%08lX "
        "SLOTR=%08lX IMR=%08lX SR=%08lX state=%lu err=%08lX\r\n",
        phase,
        (unsigned long)s->sai_a_cr1,
        (unsigned long)s->sai_a_cr2,
        (unsigned long)s->sai_a_frcr,
        (unsigned long)s->sai_a_slotr,
        (unsigned long)s->sai_a_imr,
        (unsigned long)s->sai_a_sr,
        (unsigned long)s->hsai_a_state,
        (unsigned long)s->hsai_a_error
    );
    uart_send(message);

    snprintf(
        message,
        sizeof(message),
        "[REG_SAI_B] phase=%s CR1=%08lX CR2=%08lX FRCR=%08lX "
        "SLOTR=%08lX IMR=%08lX SR=%08lX state=%lu err=%08lX\r\n",
        phase,
        (unsigned long)s->sai_b_cr1,
        (unsigned long)s->sai_b_cr2,
        (unsigned long)s->sai_b_frcr,
        (unsigned long)s->sai_b_slotr,
        (unsigned long)s->sai_b_imr,
        (unsigned long)s->sai_b_sr,
        (unsigned long)s->hsai_b_state,
        (unsigned long)s->hsai_b_error
    );
    uart_send(message);

    snprintf(
        message,
        sizeof(message),
        "[REG_DMA0] phase=%s CR=%08lX NDTR=%lu PAR=%08lX "
        "M0AR=%08lX FCR=%08lX state=%lu err=%08lX\r\n",
        phase,
        (unsigned long)s->dma0_cr,
        (unsigned long)s->dma0_ndtr,
        (unsigned long)s->dma0_par,
        (unsigned long)s->dma0_m0ar,
        (unsigned long)s->dma0_fcr,
        (unsigned long)s->hdma_a_state,
        (unsigned long)s->hdma_a_error
    );
    uart_send(message);

    snprintf(
        message,
        sizeof(message),
        "[REG_DMA1] phase=%s CR=%08lX NDTR=%lu PAR=%08lX "
        "M0AR=%08lX FCR=%08lX state=%lu err=%08lX\r\n",
        phase,
        (unsigned long)s->dma1_cr,
        (unsigned long)s->dma1_ndtr,
        (unsigned long)s->dma1_par,
        (unsigned long)s->dma1_m0ar,
        (unsigned long)s->dma1_fcr,
        (unsigned long)s->hdma_b_state,
        (unsigned long)s->hdma_b_error
    );
    uart_send(message);

    snprintf(
        message,
        sizeof(message),
        "[REG_GPIO] phase=%s "
        "PI_MODER=%08lX PI_SPEED=%08lX PI_PUPD=%08lX PI_AFRL=%08lX "
        "PG_MODER=%08lX PG_SPEED=%08lX PG_PUPD=%08lX PG_AFRH=%08lX\r\n",
        phase,
        (unsigned long)s->gpioi_moder,
        (unsigned long)s->gpioi_ospeedr,
        (unsigned long)s->gpioi_pupdr,
        (unsigned long)s->gpioi_afrl,
        (unsigned long)s->gpiog_moder,
        (unsigned long)s->gpiog_ospeedr,
        (unsigned long)s->gpiog_pupdr,
        (unsigned long)s->gpiog_afrh
    );
    uart_send(message);

    snprintf(
        message,
        sizeof(message),
        "[REG_RCC] phase=%s D2CCIP1R=%08lX\r\n",
        phase,
        (unsigned long)s->rcc_d2ccip1r
    );
    uart_send(message);
}

static void sai_dma_diag_emit_all(void)
{
    uart_send("[REG_DUMP_BEGIN]\r\n");
    sai_dma_diag_emit_one("PRE_START", &s_diag_pre_start);
    sai_dma_diag_emit_one("AFTER_START", &s_diag_after_start);
    sai_dma_diag_emit_one("AFTER_STOP", &s_diag_after_stop);
    uart_send("[REG_DUMP_END]\r\n");
}

static void uart_send(const char *text)
{
    if (text == NULL)
    {
        return;
    }

    HAL_UART_Transmit(
        &huart1,
        (uint8_t *)text,
        (uint16_t)strlen(text),
        HAL_MAX_DELAY
    );
}

static uint32_t cycles_to_microseconds(uint32_t cycles)
{
    if (SystemCoreClock == 0U)
    {
        return 0U;
    }

    return (uint32_t)(
        ((uint64_t)cycles * 1000000ULL) /
        (uint64_t)SystemCoreClock
    );
}

/*
 * newlib-nano puede no soportar %llu en snprintf. Si se usa sin soporte
 * de long long, los argumentos siguientes quedan desalineados y el log
 * termina leyendo memoria como si fuera una cadena.
 */
static void uint64_to_decimal(
    uint64_t value,
    char *destination,
    size_t destination_size)
{
    char reverse[21];
    size_t digits = 0U;

    if ((destination == NULL) || (destination_size == 0U))
    {
        return;
    }

    do
    {
        reverse[digits] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
        digits++;
    }
    while ((value != 0ULL) && (digits < sizeof(reverse)));

    if (digits >= destination_size)
    {
        destination[0] = '\0';
        return;
    }

    for (size_t i = 0U; i < digits; i++)
    {
        destination[i] = reverse[digits - 1U - i];
    }

    destination[digits] = '\0';
}

static bool configure_microphone_power(void)
{
    const uint8_t pmic_address = (uint8_t)(0x08U << 1U);
    uint8_t set_voltage[2] = {0x51U, 0x0FU};
    uint8_t enable_ldo[2] = {0x4FU, 0x0FU};

    const HAL_StatusTypeDef result_voltage =
        HAL_I2C_Master_Transmit(
            &hi2c1,
            pmic_address,
            set_voltage,
            2U,
            100U
        );

    const HAL_StatusTypeDef result_enable =
        HAL_I2C_Master_Transmit(
            &hi2c1,
            pmic_address,
            enable_ldo,
            2U,
            100U
        );

    HAL_Delay(50U);

    return
        (result_voltage == HAL_OK) &&
        (result_enable == HAL_OK);
}

static bool wait_for_record_command(void)
{
    uart_send(
        "[RECORD_READY] calibration=3 max_record=unlimited "
        "sample_rate=44100 channels=4 mode=producer_consumer_live "
        "stop_command=S\r\n"
    );

    while (1)
    {
        uint8_t command = 0U;

        if (HAL_UART_Receive(
                &huart1,
                &command,
                1U,
                500U) == HAL_OK)
        {
            if (command == (uint8_t)'R')
            {
                return true;
            }
        }

        LED_TOGGLE(LED_BLUE_Pin);
    }
}

static bool start_audio_capture(void)
{
    if (!audio_recorder_start_session())
    {
        return false;
    }

    if (!audio_recorder_arm_stop_receiver())
    {
        return false;
    }

    /* Solo snapshot en RAM: no se transmite nada durante la captura. */
    sai_dma_diag_snapshot(&s_diag_pre_start);

    if (audio_capture_start(&g_audio_ctx) != 0)
    {
        audio_recorder_disarm_stop_receiver();
        return false;
    }

    /* Ambos SAI/DMA ya fueron arrancados. Solo lectura de registros a RAM. */
    sai_dma_diag_snapshot(&s_diag_after_start);

    s_audio_running = true;
    HAL_Delay(20U);

    return
        (hsai_BlockA2.State == HAL_SAI_STATE_BUSY_RX) &&
        (hsai_BlockB2.State == HAL_SAI_STATE_BUSY_RX);
}

static uint8_t bit_mask_from_flags(const uint8_t flags[RECORD_CHANNELS])
{
    uint8_t mask = 0U;

    for (uint32_t channel = 0U; channel < RECORD_CHANNELS; channel++)
    {
        if (flags[channel] != 0U)
        {
            mask |= (uint8_t)(1U << channel);
        }
    }

    return mask;
}

static void emit_live_detection(
    uint32_t order,
    const DroneDetectionResult *result,
    uint32_t detection_cycles)
{
#if LIVE_DETECTION_LOG_MODE == 0
    (void)order;
    (void)result;
    (void)detection_cycles;
    return;
#else
    if (result == NULL)
    {
        return;
    }

    const uint8_t active_mask =
        bit_mask_from_flags(result->acoustic_active);

#if LIVE_DETECTION_LOG_MODE == 1
    if ((active_mask == 0U) && (result->alert_level == 0U))
    {
        return;
    }
#endif

    float rel_db_max = result->delta_dbfs[0];

    for (uint32_t channel = 1U; channel < RECORD_CHANNELS; channel++)
    {
        if (result->delta_dbfs[channel] > rel_db_max)
        {
            rel_db_max = result->delta_dbfs[channel];
        }
    }

    char message[224];

    snprintf(
        message,
        sizeof(message),
        "[LIVE_DET] order=%lu alert=%u fused=%.4f ema=%.4f "
        "rel_db=%.2f baseline=%u active=0x%02X det_us=%lu\r\n",
        (unsigned long)order,
        (unsigned)result->alert_level,
        (double)result->fused_probability,
        (double)result->ema,
        (double)rel_db_max,
        (unsigned)result->baseline_ready,
        (unsigned)active_mask,
        (unsigned long)cycles_to_microseconds(detection_cycles)
    );

    uart_send(message);
#endif
}

static void emit_live_toa(
    uint32_t order,
    const ToaResult *result,
    uint32_t detection_cycles,
    uint32_t toa_cycles)
{
    if (result == NULL)
    {
        return;
    }

    char message[448];

    snprintf(
        message,
        sizeof(message),
        "[TOA] seq=%lu valid=%u cvalid=%u flags=0x%02X dir=%s "
        "az=%.2f H=%.2f V=%.2f cl=%.2f "
        "e1=%.2f e2=%.2f e3=%.2f e4=%.2f "
        "LT=%.2f LB=%.2f LR=%.2f TB=%.2f TR=%.2f BR=%.2f "
        "cLT=%.3f cLB=%.3f cLR=%.3f cTB=%.3f cTR=%.3f cBR=%.3f "
        "det_us=%lu toa_us=%lu proc_us=%lu\r\n",
        (unsigned long)order,
        (unsigned)result->valid,
        (unsigned)result->closure_valid,
        (unsigned)result->invalid_flags,
        toa_direction_text(result->direction),
        (double)result->azimuth_deg,
        (double)result->h_samples,
        (double)result->v_samples,
        (double)result->closure_error_samples,
        (double)result->closure_e1_samples,
        (double)result->closure_e2_samples,
        (double)result->closure_e3_samples,
        (double)result->closure_e4_samples,
        (double)result->pair[TOA_PAIR_LEFT_TOP].lag_samples,
        (double)result->pair[TOA_PAIR_LEFT_BACK].lag_samples,
        (double)result->pair[TOA_PAIR_LEFT_RIGHT].lag_samples,
        (double)result->pair[TOA_PAIR_TOP_BACK].lag_samples,
        (double)result->pair[TOA_PAIR_TOP_RIGHT].lag_samples,
        (double)result->pair[TOA_PAIR_BACK_RIGHT].lag_samples,
        (double)result->pair[TOA_PAIR_LEFT_TOP].correlation,
        (double)result->pair[TOA_PAIR_LEFT_BACK].correlation,
        (double)result->pair[TOA_PAIR_LEFT_RIGHT].correlation,
        (double)result->pair[TOA_PAIR_TOP_BACK].correlation,
        (double)result->pair[TOA_PAIR_TOP_RIGHT].correlation,
        (double)result->pair[TOA_PAIR_BACK_RIGHT].correlation,
        (unsigned long)cycles_to_microseconds(detection_cycles),
        (unsigned long)cycles_to_microseconds(toa_cycles),
        (unsigned long)cycles_to_microseconds(detection_cycles + toa_cycles)
    );

    uart_send(message);
}

static const char *toa_pair_text(ToaPair pair)
{
    switch (pair)
    {
        case TOA_PAIR_LEFT_TOP:
            return "LT";
        case TOA_PAIR_LEFT_BACK:
            return "LB";
        case TOA_PAIR_LEFT_RIGHT:
            return "LR";
        case TOA_PAIR_TOP_BACK:
            return "TB";
        case TOA_PAIR_TOP_RIGHT:
            return "TR";
        case TOA_PAIR_BACK_RIGHT:
            return "BR";
        default:
            return "??";
    }
}

static void emit_toa_peak_diagnostics(
    uint32_t order,
    const ToaResult *result)
{
    if (result == NULL)
    {
        return;
    }

    for (uint32_t pair_index = 0U;
         pair_index < TOA_PAIR_COUNT;
         pair_index++)
    {
        const ToaPairResult *pair = &result->pair[pair_index];
        char message[320];
        int offset = snprintf(
            message,
            sizeof(message),
            "[TOA_PEAKS] seq=%lu pair=%s",
            (unsigned long)order,
            toa_pair_text((ToaPair)pair_index)
        );

        if ((offset < 0) || ((size_t)offset >= sizeof(message)))
        {
            continue;
        }

        for (uint32_t candidate_index = 0U;
             candidate_index < TOA_PEAK_CANDIDATE_COUNT;
             candidate_index++)
        {
            const ToaPeakCandidate *candidate =
                &pair->candidate[candidate_index];

            if (candidate->valid != 0U)
            {
                const int written = snprintf(
                    message + offset,
                    sizeof(message) - (size_t)offset,
                    " p%lu=%.2f/%d/%.3f/%u",
                    (unsigned long)(candidate_index + 1U),
                    (double)candidate->lag_samples,
                    (int)candidate->integer_lag,
                    (double)candidate->correlation,
                    (unsigned)candidate->peak_at_search_boundary
                );

                if ((written < 0) ||
                    ((size_t)written >= (sizeof(message) - (size_t)offset)))
                {
                    offset = -1;
                    break;
                }

                offset += written;
            }
            else
            {
                const int written = snprintf(
                    message + offset,
                    sizeof(message) - (size_t)offset,
                    " p%lu=NA",
                    (unsigned long)(candidate_index + 1U)
                );

                if ((written < 0) ||
                    ((size_t)written >= (sizeof(message) - (size_t)offset)))
                {
                    offset = -1;
                    break;
                }

                offset += written;
            }
        }

        if (offset < 0)
        {
            continue;
        }

        snprintf(
            message + offset,
            sizeof(message) - (size_t)offset,
            "\r\n"
        );
        uart_send(message);
    }
}


static void emit_toa_solver_diagnostics(
    uint32_t order,
    const ToaResult *result)
{
    if (result == NULL)
    {
        return;
    }

    char message[320];

    snprintf(
        message,
        sizeof(message),
        "[TOA_SOLVER] seq=%lu raw_valid=%u used=%u recovered=%u "
        "raw_cl=%.2f final_cl=%.2f raw_score=%.3f final_score=%.3f "
        "ratio=%.3f tested=%u accepted=%u "
        "sel=%u,%u,%u,%u,%u,%u\r\n",
        (unsigned long)order,
        (unsigned)result->raw_valid,
        (unsigned)result->solver_used,
        (unsigned)result->solver_recovered,
        (double)result->raw_closure_error_samples,
        (double)result->closure_error_samples,
        (double)result->solver_raw_score,
        (double)result->solver_selected_score,
        (double)result->solver_score_ratio,
        (unsigned)result->solver_combinations_tested,
        (unsigned)result->solver_valid_combinations,
        (unsigned)result->selected_candidate[TOA_PAIR_LEFT_TOP],
        (unsigned)result->selected_candidate[TOA_PAIR_LEFT_BACK],
        (unsigned)result->selected_candidate[TOA_PAIR_LEFT_RIGHT],
        (unsigned)result->selected_candidate[TOA_PAIR_TOP_BACK],
        (unsigned)result->selected_candidate[TOA_PAIR_TOP_RIGHT],
        (unsigned)result->selected_candidate[TOA_PAIR_BACK_RIGHT]
    );

    uart_send(message);
}

static void emit_toa_confidence(
    uint32_t order,
    const ToaResult *result)
{
    if (result == NULL)
    {
        return;
    }

    char message[256];

    if (result->confidence_gap_pair_count != 0U)
    {
        snprintf(
            message,
            sizeof(message),
            "[TOA_CONF] seq=%lu level=%s track=%u amb=%u gap_min=%.3f "
            "gap_pairs=%u corr_min=%.3f corr_mean=%.3f cl=%.2f "
            "solver=%u ratio=%.3f\r\n",
            (unsigned long)order,
            toa_confidence_text(result->confidence),
            (unsigned)result->track_eligible,
            (unsigned)result->confidence_ambiguous_pairs,
            (double)result->confidence_peak_gap_min,
            (unsigned)result->confidence_gap_pair_count,
            (double)result->confidence_selected_corr_min,
            (double)result->confidence_selected_corr_mean,
            (double)result->closure_error_samples,
            (unsigned)result->solver_used,
            (double)result->solver_score_ratio
        );
    }
    else
    {
        snprintf(
            message,
            sizeof(message),
            "[TOA_CONF] seq=%lu level=%s track=%u amb=%u gap_min=NA "
            "gap_pairs=0 corr_min=%.3f corr_mean=%.3f cl=%.2f "
            "solver=%u ratio=%.3f\r\n",
            (unsigned long)order,
            toa_confidence_text(result->confidence),
            (unsigned)result->track_eligible,
            (unsigned)result->confidence_ambiguous_pairs,
            (double)result->confidence_selected_corr_min,
            (double)result->confidence_selected_corr_mean,
            (double)result->closure_error_samples,
            (unsigned)result->solver_used,
            (double)result->solver_score_ratio
        );
    }

    uart_send(message);
}

static void emit_operational_target(
    uint32_t order,
    const DroneDetectionResult *detection,
    const ToaResult *toa,
    uint32_t detection_cycles,
    uint32_t toa_cycles)
{
    if (detection == NULL)
    {
        return;
    }

    char message[256];

    if (detection->alert_level == 0U)
    {
        snprintf(
            message,
            sizeof(message),
            "[TARGET] seq=%lu detected=0 alert=0 toa=NOT_EVALUATED "
            "loc_valid=0 dir=NONE az=NA cl=NA conf=NONE track=0 "
            "det_us=%lu toa_us=0 proc_us=%lu\r\n",
            (unsigned long)order,
            (unsigned long)cycles_to_microseconds(detection_cycles),
            (unsigned long)cycles_to_microseconds(detection_cycles)
        );

        uart_send(message);
        return;
    }

    if (toa == NULL)
    {
        return;
    }

    if (toa->valid != 0U)
    {
        snprintf(
            message,
            sizeof(message),
            "[TARGET] seq=%lu detected=1 alert=%u toa=VALID "
            "loc_valid=1 dir=%s az=%.2f cl=%.2f conf=%s track=%u "
            "det_us=%lu toa_us=%lu proc_us=%lu\r\n",
            (unsigned long)order,
            (unsigned)detection->alert_level,
            toa_direction_text(toa->direction),
            (double)toa->azimuth_deg,
            (double)toa->closure_error_samples,
            toa_confidence_text(toa->confidence),
            (unsigned)toa->track_eligible,
            (unsigned long)cycles_to_microseconds(detection_cycles),
            (unsigned long)cycles_to_microseconds(toa_cycles),
            (unsigned long)cycles_to_microseconds(
                detection_cycles + toa_cycles
            )
        );
    }
    else
    {
        snprintf(
            message,
            sizeof(message),
            "[TARGET] seq=%lu detected=1 alert=%u toa=INVALID "
            "loc_valid=0 dir=UNKNOWN az=NA cl=%.2f flags=0x%02X "
            "conf=NONE track=0 "
            "det_us=%lu toa_us=%lu proc_us=%lu\r\n",
            (unsigned long)order,
            (unsigned)detection->alert_level,
            (double)toa->closure_error_samples,
            (unsigned)toa->invalid_flags,
            (unsigned long)cycles_to_microseconds(detection_cycles),
            (unsigned long)cycles_to_microseconds(toa_cycles),
            (unsigned long)cycles_to_microseconds(
                detection_cycles + toa_cycles
            )
        );
    }

    uart_send(message);
}

static void emit_detection_profile(void)
{
    DroneDetectionProfile profile = {0};

    if (!drone_detection_get_profile(&profile))
    {
        uart_send("[P1_PROFILE_FAIL]\r\n");
        return;
    }

    for (uint32_t channel = 0U;
         channel < RECORD_CHANNELS;
         channel++)
    {
        char message[224];

        snprintf(
            message,
            sizeof(message),
            "[P1_CH%lu] "
            "pre_last_us=%lu pre_max_us=%lu "
            "mfcc_last_us=%lu mfcc_max_us=%lu mfcc_runs=%lu "
            "tflite_last_us=%lu tflite_max_us=%lu tflite_runs=%lu "
            "total_last_us=%lu total_max_us=%lu\r\n",
            (unsigned long)channel,
            (unsigned long)cycles_to_microseconds(
                profile.preprocess_last_cycles[channel]
            ),
            (unsigned long)cycles_to_microseconds(
                profile.preprocess_max_cycles[channel]
            ),
            (unsigned long)cycles_to_microseconds(
                profile.mfcc_last_cycles[channel]
            ),
            (unsigned long)cycles_to_microseconds(
                profile.mfcc_max_cycles[channel]
            ),
            (unsigned long)profile.mfcc_runs[channel],
            (unsigned long)cycles_to_microseconds(
                profile.tflite_last_cycles[channel]
            ),
            (unsigned long)cycles_to_microseconds(
                profile.tflite_max_cycles[channel]
            ),
            (unsigned long)profile.tflite_runs[channel],
            (unsigned long)cycles_to_microseconds(
                profile.total_last_cycles[channel]
            ),
            (unsigned long)cycles_to_microseconds(
                profile.total_max_cycles[channel]
            )
        );

        uart_send(message);
    }

    for (uint32_t channel = 0U;
         channel < RECORD_CHANNELS;
         channel++)
    {
        char message[256];

        snprintf(
            message,
            sizeof(message),
            "[P1_CAL_CH%lu] c0_dbfs=%.2f c1_dbfs=%.2f c2_dbfs=%.2f baseline_dbfs=%.2f\r\n",
            (unsigned long)channel,
            (double)profile.calibration_dbfs[channel][0],
            (double)profile.calibration_dbfs[channel][1],
            (double)profile.calibration_dbfs[channel][2],
            (double)profile.gate_baseline_dbfs[channel]
        );

        uart_send(message);

        snprintf(
            message,
            sizeof(message),
            "[P1_GATE_CH%lu] margin_db=%.2f last_dbfs=%.2f last_delta_db=%.2f max_delta_db=%.2f gate_windows=%lu active_count=%lu\r\n",
            (unsigned long)channel,
            (double)profile.gate_margin_db[channel],
            (double)profile.gate_last_dbfs[channel],
            (double)profile.gate_last_delta_db[channel],
            (double)profile.gate_max_delta_db[channel],
            (unsigned long)profile.gate_windows[channel],
            (unsigned long)profile.gate_active_count[channel]
        );

        uart_send(message);
    }

    {
        char message[160];

        snprintf(
            message,
            sizeof(message),
            "[P1_WINDOW] last_us=%lu max_us=%lu windows=%lu\r\n",
            (unsigned long)cycles_to_microseconds(
                profile.window_last_cycles
            ),
            (unsigned long)cycles_to_microseconds(
                profile.window_max_cycles
            ),
            (unsigned long)profile.windows_profiled
        );

        uart_send(message);
    }
}





static void stop_with_error(const char *message)
{
    audio_recorder_disarm_stop_receiver();

    if (s_audio_running)
    {
        (void)audio_capture_stop(&g_audio_ctx);
        s_audio_running = false;
    }

    LED_OFF(LED_GREEN_Pin);
    LED_OFF(LED_BLUE_Pin);
    LED_ON(LED_RED_Pin);

    uart_send(message);

    while (1)
    {
        LED_TOGGLE(LED_RED_Pin);
        HAL_Delay(250U);
    }
}

/* TRACK3D-R1: telemetria paralela, formato aditivo; nunca afecta F1.1. */
static void emit_track3d(uint32_t seq, const Track3dOutput *t)
{
    char message[224];
    if (t == NULL) return;
    if (t->valid != 0U) {
        if (t->innovation_valid != 0U) {
            snprintf(message, sizeof(message),
                "[TRACK3D] seq=%lu state=%s valid=1 used=%u rejected=%u "
                "az=%.2f elev=%.2f az_phys=%.2f innovation=%.2f\r\n",
                (unsigned long)seq, track3d_state_text(t->state),
                (unsigned)t->used, (unsigned)t->rejected,
                (double)t->azimuth_native_deg, (double)t->elevation_deg,
                (double)t->azimuth_physical_deg, (double)t->innovation_deg);
        } else {
            snprintf(message, sizeof(message),
                "[TRACK3D] seq=%lu state=%s valid=1 used=%u rejected=%u "
                "az=%.2f elev=%.2f az_phys=%.2f innovation=NA\r\n",
                (unsigned long)seq, track3d_state_text(t->state),
                (unsigned)t->used, (unsigned)t->rejected,
                (double)t->azimuth_native_deg, (double)t->elevation_deg,
                (double)t->azimuth_physical_deg);
        }
    } else {
        snprintf(message, sizeof(message),
            "[TRACK3D] seq=%lu state=%s valid=0 used=%u rejected=%u "
            "az=NA elev=NA az_phys=NA innovation=NA\r\n",
            (unsigned long)seq, track3d_state_text(t->state),
            (unsigned)t->used, (unsigned)t->rejected);
    }
    uart_send(message);
}

static bool process_ready_slot(uint8_t slot)
{
    AudioChunkDescriptor descriptor = {0};
    RecorderChunkView view = {0};
    DroneDetectionResult result = {0};

    if (!audio_recorder_get_slot_descriptor(slot, &descriptor) ||
        !audio_recorder_get_slot_view(slot, &view))
    {
        return false;
    }

    const uint32_t start_cycles = DWT->CYCCNT;

    if (!drone_detection_process_window(&view) ||
        !drone_detection_get_last_result(&result))
    {
        return false;
    }

    const uint32_t detection_cycles = DWT->CYCCNT - start_cycles;
    uint32_t toa_cycles = 0U;

    if ((descriptor.flags & AUDIO_SLOT_FLAG_RECORD) != 0U)
    {
        if (descriptor.record_order == 0U)
        {
            return false;
        }

        emit_live_detection(
            descriptor.record_order,
            &result,
            detection_cycles
        );

        if (result.alert_level == 0U)
        {
            s_no_target_count++;

            emit_operational_target(
                descriptor.record_order,
                &result,
                NULL,
                detection_cycles,
                0U
            );
            {
                const Track3dOutput t = track3d_step(
                    descriptor.record_order, 0U, 0U, 0U, 0.0f, 0.0f);
                emit_track3d(descriptor.record_order, &t);
            }
        }
        else
        {
            ToaResult toa_result = {0};
            const uint32_t toa_start_cycles = DWT->CYCCNT;

            if (!toa_localizer_process(&view, &toa_result))
            {
                return false;
            }

            toa_cycles = DWT->CYCCNT - toa_start_cycles;

            if (toa_cycles > s_toa_cycles_max)
            {
                s_toa_cycles_max = toa_cycles;
            }

            emit_live_toa(
                descriptor.record_order,
                &toa_result,
                detection_cycles,
                toa_cycles
            );

            {
                char tetra_line[160];
                snprintf(tetra_line, sizeof(tetra_line),
                         "[TOA_TETRA] seq=%lu fit=%u elev=%.2f norm=%.4f rms=%.3f\r\n",
                         (unsigned long)descriptor.record_order,
                         (unsigned)toa_result.tetra_fit_ok,
                         (double)toa_result.tetra_elevation_deg,
                         (double)toa_result.tetra_norm,
                         (double)toa_result.tetra_rms_samples);
                uart_send(tetra_line);
            }

            emit_toa_peak_diagnostics(
                descriptor.record_order,
                &toa_result
            );

            emit_toa_solver_diagnostics(
                descriptor.record_order,
                &toa_result
            );

            emit_toa_confidence(
                descriptor.record_order,
                &toa_result
            );

            emit_operational_target(
                descriptor.record_order,
                &result,
                &toa_result,
                detection_cycles,
                toa_cycles
            );
            {
                const Track3dOutput t = track3d_step(
                    descriptor.record_order,
                    toa_result.valid,
                    toa_result.track_eligible,
                    toa_result.tetra_fit_ok,
                    toa_result.azimuth_deg,
                    toa_result.tetra_elevation_deg);
                emit_track3d(descriptor.record_order, &t);
            }

            if (toa_result.valid != 0U)
            {
                s_target_valid_count++;

                if (toa_result.confidence == TOA_CONFIDENCE_HIGH)
                {
                    s_conf_high_count++;
                }
                else if (toa_result.confidence == TOA_CONFIDENCE_MEDIUM)
                {
                    s_conf_medium_count++;
                }
                else if (toa_result.confidence == TOA_CONFIDENCE_LOW)
                {
                    s_conf_low_count++;
                }

                if (toa_result.track_eligible != 0U)
                {
                    s_track_eligible_count++;
                }
            }
            else
            {
                s_target_invalid_count++;
            }
        }

        const uint32_t processing_cycles = detection_cycles + toa_cycles;
        if (processing_cycles > s_processing_cycles_max)
        {
            s_processing_cycles_max = processing_cycles;
        }
    }

    return audio_recorder_complete_processing(slot, detection_cycles);
}

int main(void)
{
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
    int32_t timeout;
#endif

    MPU_Config();

#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
    timeout = 0xFFFF;
    while ((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) &&
           (timeout-- > 0))
    {
    }

    if (timeout < 0)
    {
        Error_Handler();
    }
#endif

    HAL_Init();
    SystemClock_Config();
    PeriphCommonClock_Config();

#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
    __HAL_RCC_HSEM_CLK_ENABLE();
    HAL_HSEM_FastTake(HSEM_ID_0);
    HAL_HSEM_Release(HSEM_ID_0, 0);

    timeout = 0xFFFF;
    while ((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) == RESET) &&
           (timeout-- > 0))
    {
    }

    if (timeout < 0)
    {
        Error_Handler();
    }
#endif

    MX_GPIO_Init();
    MX_USART1_UART_Init();

    LED_OFF(LED_RED_Pin);
    LED_OFF(LED_GREEN_Pin);
    LED_OFF(LED_BLUE_Pin);

    uart_send("[BOOT] SDRAM producer-consumer recorder v2.1.7\r\n");

    if (!portenta_sdram_init())
    {
        stop_with_error("[SDRAM_INIT_FAIL]\r\n");
    }

#if SDRAM_RUN_QUICK_BOOT_TEST
    {
        PortentaSdramTestReport report = {0};

        if (!portenta_sdram_quick_test(&report))
        {
            stop_with_error("[SDRAM_QUICK_TEST_FAIL]\r\n");
        }
    }
#endif

    MX_I2C1_Init();
    MX_DMA_Init();
    MX_SAI2_Init();

    if (!configure_microphone_power())
    {
        stop_with_error("[PMIC_FAIL]\r\n");
    }

    if (audio_capture_init(&g_audio_ctx) != 0)
    {
        stop_with_error("[AUDIO_CAPTURE_INIT_FAIL]\r\n");
    }

    audio_recorder_init();

    if (!audio_recorder_is_free_running() ||
        (audio_recorder_runtime_api_revision() != AUDIO_RECORDER_LIVE_API_REV))
    {
        stop_with_error("[AUDIO_RECORDER_MODE_FAIL]\r\n");
    }

    {
        char message[160];
        snprintf(
            message,
            sizeof(message),
            "[RECORDER_MODE] free_running=1 retain_audio=0 stop_boundary=1 api=0x%08lX\r\n",
            (unsigned long)AUDIO_RECORDER_LIVE_API_REV
        );
        uart_send(message);
    }

    /* El modelo debe estar listo antes de completar el primer chunk. */
    if (!drone_detection_init())
    {
        stop_with_error("[DETECTION_INIT_FAIL]\r\n");
    }


    track3d_reset();

    if (!wait_for_record_command())
    {
        stop_with_error("[RECORD_COMMAND_FAIL]\r\n");
    }


    uart_send(
        "[CAPTURE_STARTED] calibration=3 max_record=unlimited "
        "stop_at_second_boundary=1 detection=concurrent "
        "toa_gate=alert_gt_0 live=1 visual_telemetry=1 peak_diag=3 "
        "peak_solver=global_corr_closure ratio_min=0.95 "
        "confidence=peak_ambiguity_v1 track_gate=low_reject\r\n"
    );

    if (!start_audio_capture())
    {
        stop_with_error("[AUDIO_CAPTURE_START_FAIL]\r\n");
    }

    LED_ON(LED_GREEN_Pin);
    LED_OFF(LED_BLUE_Pin);

    while (1)
    {
        if (audio_capture_has_fatal_error(&g_audio_ctx))
        {
            stop_with_error("[CAPTURE_OVERRUN_OR_PAIR_ERROR]\r\n");
        }

        if (audio_recorder_capture_closed() && s_audio_running)
        {
            audio_recorder_disarm_stop_receiver();

            if (audio_capture_stop(&g_audio_ctx) != 0)
            {
                stop_with_error("[AUDIO_CAPTURE_STOP_FAIL]\r\n");
            }

            sai_dma_diag_snapshot(&s_diag_after_stop);

            s_audio_running = false;
            LED_OFF(LED_GREEN_Pin);
            LED_ON(LED_BLUE_Pin);
        }

        uint8_t slot = AUDIO_SLOT_INVALID;

        if (audio_recorder_pop_ready_slot(&slot))
        {
            if (!process_ready_slot(slot))
            {
                stop_with_error("[DETECTION_PROCESS_FAIL]\r\n");
            }

            LED_TOGGLE(LED_GREEN_Pin);
            continue;
        }

        if (audio_recorder_capture_closed() &&
            (audio_recorder_ready_count() == 0U))
        {
            break;
        }

        __WFI();
    }

    const uint32_t recorded_chunks =
        audio_recorder_recorded_chunks();

    AudioRecorderStats recorder_stats = {0};
    AudioCaptureStats capture_stats = {0};
    audio_recorder_get_stats(&recorder_stats);
    audio_capture_get_stats(&g_audio_ctx, &capture_stats);

    /*
     * La captura y DMA ya estan detenidos. Ahora si imprimimos los snapshots.
     * No se hace UART/printf durante la fase critica de grabacion.
     */
    sai_dma_diag_emit_all();

    {
        char message[320];
        char frames_text[24];

        uint64_to_decimal(
            recorder_stats.frames_written,
            frames_text,
            sizeof(frames_text)
        );

        snprintf(
            message,
            sizeof(message),
            "[CAPTURE_DONE] frames=%s completed=%lu detected=%lu "
            "record_chunks=%lu reason=%s stop_requested=%u "
            "queue_high=%lu copy_us_max=%lu detect_us_max=%lu "
            "pairs=%lu pair_skew=%lu errors=%lu\r\n",
            frames_text,
            (unsigned long)recorder_stats.chunks_completed,
            (unsigned long)recorder_stats.chunks_detected,
            (unsigned long)recorded_chunks,
            audio_recorder_close_reason_text(recorder_stats.close_reason),
            (unsigned)recorder_stats.stop_requested,
            (unsigned long)recorder_stats.ready_queue_high_water,
            (unsigned long)cycles_to_microseconds(recorder_stats.copy_cycles_max),
            (unsigned long)cycles_to_microseconds(recorder_stats.detection_cycles_max),
            (unsigned long)capture_stats.paired_blocks,
            (unsigned long)capture_stats.maximum_pair_skew,
            (unsigned long)capture_stats.errors
        );
        uart_send(message);
    }

    emit_detection_profile();


    {
        char message[256];
        snprintf(
            message,
            sizeof(message),
            "[LIVE_SUMMARY] records=%lu no_target=%lu target_valid=%lu "
            "target_invalid=%lu toa_us_max=%lu proc_us_max=%lu\r\n",
            (unsigned long)recorded_chunks,
            (unsigned long)s_no_target_count,
            (unsigned long)s_target_valid_count,
            (unsigned long)s_target_invalid_count,
            (unsigned long)cycles_to_microseconds(s_toa_cycles_max),
            (unsigned long)cycles_to_microseconds(s_processing_cycles_max)
        );
        uart_send(message);
    }

    {
        char message[256];
        snprintf(
            message,
            sizeof(message),
            "[CONF_SUMMARY] high=%lu medium=%lu low=%lu track_eligible=%lu "
            "amb_gap_max=0.020 high_cl_max=0.80\r\n",
            (unsigned long)s_conf_high_count,
            (unsigned long)s_conf_medium_count,
            (unsigned long)s_conf_low_count,
            (unsigned long)s_track_eligible_count
        );
        uart_send(message);
    }

    {
        char message[192];
        snprintf(
            message,
            sizeof(message),
            "[TOA1E3_END] reason=%s live=stopped transfer=disabled json=disabled\r\n",
            audio_recorder_close_reason_text(recorder_stats.close_reason)
        );
        uart_send(message);
    }

    LED_OFF(LED_RED_Pin);
    LED_OFF(LED_BLUE_Pin);
    LED_ON(LED_GREEN_Pin);

    while (1)
    {
        HAL_Delay(1000U);
    }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  __HAL_RCC_GPIOH_CLK_ENABLE();
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOH, &GPIO_InitStruct);
  HAL_GPIO_WritePin(GPIOH, GPIO_PIN_1, GPIO_PIN_SET);
  for(volatile uint32_t i = 0; i < 500000; i++) {}

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 160;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2
                              | RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK) Error_Handler();
}

void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SAI23;
  PeriphClkInitStruct.PLL3.PLL3M = 5;
  PeriphClkInitStruct.PLL3.PLL3N = 72;
  PeriphClkInitStruct.PLL3.PLL3P = 32;
  PeriphClkInitStruct.PLL3.PLL3Q = 2;
  PeriphClkInitStruct.PLL3.PLL3R = 2;
  PeriphClkInitStruct.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  PeriphClkInitStruct.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  PeriphClkInitStruct.PLL3.PLL3FRACN = 2077;
  PeriphClkInitStruct.Sai23ClockSelection = RCC_SAI23CLKSOURCE_PLL3;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK) Error_Handler();
}

static void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};
  HAL_MPU_Disable();

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x00000000; MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x24000000; MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.SubRegionDisable = 0x00; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x20000000; MPU_InitStruct.Size = MPU_REGION_SIZE_128KB;
  MPU_InitStruct.SubRegionDisable = 0x00; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER3;
  MPU_InitStruct.BaseAddress = 0x08000000; MPU_InitStruct.Size = MPU_REGION_SIZE_2MB;
  MPU_InitStruct.SubRegionDisable = 0x00; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER4;
  MPU_InitStruct.BaseAddress = 0x30000000; MPU_InitStruct.Size = MPU_REGION_SIZE_256KB;
  MPU_InitStruct.SubRegionDisable = 0x00; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  MPU_InitStruct.Enable = MPU_REGION_ENABLE; MPU_InitStruct.Number = MPU_REGION_NUMBER5;
  MPU_InitStruct.BaseAddress = PORTENTA_SDRAM_BASE_ADDRESS; MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.SubRegionDisable = 0x00; MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS; MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE; MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE; HAL_MPU_ConfigRegion(&MPU_InitStruct);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

void Error_Handler(void)
{
  __disable_irq();
  while(1) {
    HAL_GPIO_TogglePin(GPIOK, GPIO_PIN_5);
    for(volatile uint32_t i = 0; i < 1000000; i++) {}
    HAL_GPIO_TogglePin(GPIOK, GPIO_PIN_7);
    for(volatile uint32_t i = 0; i < 1000000; i++) {}
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif
