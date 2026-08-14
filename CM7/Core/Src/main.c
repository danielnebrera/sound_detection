/* =================================================================
 * main.c - Productor/consumidor SDRAM v2.1.7
 *
 * Flujo:
 *   1. Inicializa SDRAM y detector antes de capturar.
 *   2. SAI/DMA copia cada par A/B directamente al ring PCM16 en SDRAM.
 *   3. Cada segundo exacto se publica como slot READY.
 *   4. main procesa MFCC/TFLite mientras DMA captura el siguiente segundo.
 *   5. Ctrl+C llega por interrupcion UART y cierra en el proximo segundo.
 *   6. Al cerrar, transmite con ACK por bloque y fallback adaptativo.
 * ================================================================= */

#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "sai.h"
#include "usart.h"
#include "gpio.h"

#include "audio_capture.h"
#include "audio_recorder.h"
#include "drone_detection.h"
#include "portenta_sdram.h"
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
#define LIVE_DETECTION_LOG_MODE     1

#define LED_ON(pin)     HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_RESET)
#define LED_OFF(pin)    HAL_GPIO_WritePin(GPIOK, pin, GPIO_PIN_SET)
#define LED_TOGGLE(pin) HAL_GPIO_TogglePin(GPIOK, pin)

static bool s_audio_running = false;
static DroneDetectionResult s_record_results[RECORD_MAX_OUTPUT_CHUNKS];
static bool s_record_result_valid[RECORD_MAX_OUTPUT_CHUNKS];

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
        "[RECORD_READY] calibration=3 max_record=16 "
        "sample_rate=44100 channels=4 mode=producer_consumer\r\n"
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

static void format_detection_line(
    char *destination,
    size_t destination_size,
    uint32_t order,
    const DroneDetectionResult *result)
{
    if ((destination == NULL) ||
        (destination_size == 0U) ||
        (result == NULL))
    {
        return;
    }

    const uint8_t valid_mask = bit_mask_from_flags(result->valid);
    const uint8_t active_mask =
        bit_mask_from_flags(result->acoustic_active);

    snprintf(
        destination,
        destination_size,
        "[DET_RESULT] order=%lu alert=%u baseline=%u "
        "fused=%.6f ema=%.6f valid=0x%02X active=0x%02X "
        "p0=%.6f p1=%.6f p2=%.6f p3=%.6f "
        "db0=%.3f db1=%.3f db2=%.3f db3=%.3f "
        "delta0=%.3f delta1=%.3f delta2=%.3f delta3=%.3f\r\n",
        (unsigned long)order,
        (unsigned)result->alert_level,
        (unsigned)result->baseline_ready,
        (double)result->fused_probability,
        (double)result->ema,
        (unsigned)valid_mask,
        (unsigned)active_mask,
        (double)result->probability[0],
        (double)result->probability[1],
        (double)result->probability[2],
        (double)result->probability[3],
        (double)result->dbfs[0],
        (double)result->dbfs[1],
        (double)result->dbfs[2],
        (double)result->dbfs[3],
        (double)result->delta_dbfs[0],
        (double)result->delta_dbfs[1],
        (double)result->delta_dbfs[2],
        (double)result->delta_dbfs[3]
    );
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

    char message[192];

    snprintf(
        message,
        sizeof(message),
        "[LIVE_DET] order=%lu alert=%u fused=%.4f ema=%.4f "
        "active=0x%02X det_us=%lu\r\n",
        (unsigned long)order,
        (unsigned)result->alert_level,
        (double)result->fused_probability,
        (double)result->ema,
        (unsigned)active_mask,
        (unsigned long)cycles_to_microseconds(detection_cycles)
    );

    uart_send(message);
#endif
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

    if ((descriptor.flags & AUDIO_SLOT_FLAG_RECORD) != 0U)
    {
        if ((descriptor.record_order == 0U) ||
            (descriptor.record_order > RECORD_MAX_OUTPUT_CHUNKS))
        {
            return false;
        }

        const uint32_t result_index = descriptor.record_order - 1U;
        s_record_results[result_index] = result;
        s_record_result_valid[result_index] = true;

        emit_live_detection(
            descriptor.record_order,
            &result,
            detection_cycles
        );
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

    /* El modelo debe estar listo antes de completar el primer chunk. */
    if (!drone_detection_init())
    {
        stop_with_error("[DETECTION_INIT_FAIL]\r\n");
    }

    memset(s_record_results, 0, sizeof(s_record_results));
    memset(s_record_result_valid, 0, sizeof(s_record_result_valid));

    if (!wait_for_record_command())
    {
        stop_with_error("[RECORD_COMMAND_FAIL]\r\n");
    }

    uart_send(
        "[CAPTURE_STARTED] calibration=3 max_record=16 "
        "stop_at_second_boundary=1 detection=concurrent\r\n"
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

    if (recorded_chunks == 0U)
    {
        stop_with_error("[NO_COMPLETE_RECORD_CHUNKS]\r\n");
    }

    for (uint32_t chunk = 0U; chunk < recorded_chunks; chunk++)
    {
        if (!s_record_result_valid[chunk])
        {
            stop_with_error("[MISSING_DETECTION_RESULT]\r\n");
        }
    }

    if (!audio_recorder_begin_transfer(recorded_chunks))
    {
        stop_with_error("[TRANSFER_START_FAIL]\r\n");
    }

    for (uint32_t chunk = 0U; chunk < recorded_chunks; chunk++)
    {
        char detection_line[512];
        const uint32_t order = chunk + 1U;

        format_detection_line(
            detection_line,
            sizeof(detection_line),
            order,
            &s_record_results[chunk]
        );

        if (!audio_recorder_emit_record_chunk(
                chunk,
                order,
                detection_line))
        {
            stop_with_error("[TRANSFER_CHUNK_FAIL]\r\n");
        }
    }

    if (!audio_recorder_end_transfer(recorded_chunks))
    {
        stop_with_error("[TRANSFER_END_FAIL]\r\n");
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
  MPU_InitStruct.BaseAddress = 0x08040000; MPU_InitStruct.Size = MPU_REGION_SIZE_1MB;
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
