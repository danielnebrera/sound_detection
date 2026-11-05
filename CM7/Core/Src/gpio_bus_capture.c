/**
 * @file gpio_bus_capture.c
 * @brief "Analizador logico" por software para SAI2/I2S.
 *
 * Pines del montaje actual:
 *   PI5  = SAI2_SCK_A
 *   PI7  = SAI2_FS_A / WS
 *   PI6  = SAI2_SD_A
 *   PG10 = SAI2_SD_B
 *
 * La captura lee GPIOI->IDR y GPIOG->IDR mientras los pines siguen en AF10.
 * Se detecta cada flanco ascendente de SCK y se guardan simultaneamente:
 *   - nivel WS
 *   - nivel SD_A
 *   - nivel SD_B
 *
 * Cada trama produce cuatro palabras de 32 bits:
 *   A_WS0, A_WS1, B_WS0, B_WS1.
 */

#include "gpio_bus_capture.h"

#include "main.h"
#include "sai.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * 384 tramas = 8.71 ms a 44.1 kHz.
 * Queda por debajo de los 11.61 ms de una mitad DMA de 512 tramas,
 * reduciendo el riesgo de que venza un callback mientras IRQ esta deshabilitado.
 */
#define GPIOCAP_FRAME_COUNT       384U
#define GPIOCAP_BITS_PER_FRAME     64U
#define GPIOCAP_BITS_PER_SLOT      32U

#define GPIOCAP_SCK_MASK   GPIO_PIN_5   /* PI5  */
#define GPIOCAP_SDA_MASK   GPIO_PIN_6   /* PI6  */
#define GPIOCAP_WS_MASK    GPIO_PIN_7   /* PI7  */
#define GPIOCAP_SDB_MASK   GPIO_PIN_10  /* PG10 */

/*
 * Guardas de software. A 480 MHz son muy superiores al periodo normal,
 * pero evitan quedar atrapados si desaparece SCK o WS.
 */
#define GPIOCAP_EDGE_GUARD        200000U
#define GPIOCAP_SYNC_GUARD       2000000U

/*
 * El primer prototipo leia GPIOI inmediatamente al detectar SCK alto,
 * mientras GPIOG se leia unos ciclos despues. Eso favorecio artificialmente
 * a SD_B y pudo muestrear SD_A demasiado cerca del flanco.
 *
 * Ahora se espera hasta la zona media del nivel alto de SCK y se realizan
 * dos lecturas separadas para comprobar estabilidad.
 *
 * Con SystemCoreClock ~= 400 MHz y BCLK ~= 2.8224 MHz:
 *   periodo BCLK ~= 142 ciclos
 *   semiperiodo  ~= 71 ciclos
 *   muestra 1    = 24 ciclos despues del flanco
 *   muestra 2    = 36 ciclos despues del flanco
 */
#define GPIOCAP_SAMPLE_DELAY_CYCLES       24U
#define GPIOCAP_STABILITY_DELAY_CYCLES    12U

typedef struct
{
    uint32_t a_ws0;
    uint32_t a_ws1;
    uint32_t b_ws0;
    uint32_t b_ws1;
} GpioBusFrame;

static GpioBusFrame g_gpio_frames[GPIOCAP_FRAME_COUNT];

static uint32_t g_invalid_frames = 0U;
static uint32_t g_edge_count = 0U;
static uint32_t g_cycle_sum = 0U;
static uint32_t g_cycle_min = 0xFFFFFFFFU;
static uint32_t g_cycle_max = 0U;

static uint32_t g_sda_unstable = 0U;
static uint32_t g_sdb_unstable = 0U;
static uint32_t g_ws_unstable = 0U;

static void dwt_cycle_counter_start(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
}

static bool wait_ws_level(bool high)
{
    uint32_t guard = GPIOCAP_SYNC_GUARD;

    while (guard > 0U)
    {
        const bool current =
            ((GPIOI->IDR & GPIOCAP_WS_MASK) != 0U);

        if (current == high)
        {
            return true;
        }

        guard--;
    }

    return false;
}

static inline bool wait_sck_low(void)
{
    uint32_t guard = GPIOCAP_EDGE_GUARD;

    while ((GPIOI->IDR & GPIOCAP_SCK_MASK) != 0U)
    {
        if (guard == 0U)
        {
            return false;
        }

        guard--;
    }

    return true;
}

static inline bool wait_sck_high(void)
{
    uint32_t guard = GPIOCAP_EDGE_GUARD;

    while ((GPIOI->IDR & GPIOCAP_SCK_MASK) == 0U)
    {
        if (guard == 0U)
        {
            return false;
        }

        guard--;
    }

    return true;
}

static bool capture_one_frame(
    GpioBusFrame *frame,
    uint32_t *previous_rise_cycle
)
{
    if ((frame == NULL) || (previous_rise_cycle == NULL))
    {
        return false;
    }

    /*
     * Sincronizacion al comienzo de WS=0:
     * primero se observa WS=1 y luego la transicion a WS=0.
     */
    if (!wait_ws_level(true))
    {
        return false;
    }

    if (!wait_ws_level(false))
    {
        return false;
    }

    uint32_t a_ws0 = 0U;
    uint32_t a_ws1 = 0U;
    uint32_t b_ws0 = 0U;
    uint32_t b_ws1 = 0U;

    uint32_t ws0_count = 0U;
    uint32_t ws1_count = 0U;

    for (uint32_t bit = 0U; bit < GPIOCAP_BITS_PER_FRAME; bit++)
    {
        if (!wait_sck_low())
        {
            return false;
        }

        if (!wait_sck_high())
        {
            return false;
        }

        /*
         * Registrar el flanco y esperar hasta la zona media del nivel alto.
         * Esto evita leer SD_A inmediatamente sobre el flanco mientras SD_B
         * se beneficiaba accidentalmente del retraso de la segunda lectura.
         */
        const uint32_t rise_cycle = DWT->CYCCNT;

        if (*previous_rise_cycle != 0U)
        {
            const uint32_t delta =
                rise_cycle - *previous_rise_cycle;

            g_cycle_sum += delta;
            g_edge_count++;

            if (delta < g_cycle_min)
            {
                g_cycle_min = delta;
            }

            if (delta > g_cycle_max)
            {
                g_cycle_max = delta;
            }
        }

        *previous_rise_cycle = rise_cycle;

        while ((uint32_t)(DWT->CYCCNT - rise_cycle) <
               GPIOCAP_SAMPLE_DELAY_CYCLES)
        {
            __NOP();
        }

        const uint32_t gpio_i_first = GPIOI->IDR;
        const uint32_t gpio_g_first = GPIOG->IDR;

        while ((uint32_t)(DWT->CYCCNT - rise_cycle) <
               (GPIOCAP_SAMPLE_DELAY_CYCLES +
                GPIOCAP_STABILITY_DELAY_CYCLES))
        {
            __NOP();
        }

        /*
         * Se usa la segunda lectura, mas alejada del flanco, como dato final.
         * La primera solo sirve para detectar si la linea cambio dentro de la
         * misma mitad alta de SCK.
         */
        const uint32_t gpio_i = GPIOI->IDR;
        const uint32_t gpio_g = GPIOG->IDR;

        if (((gpio_i_first ^ gpio_i) & GPIOCAP_SDA_MASK) != 0U)
        {
            g_sda_unstable++;
        }

        if (((gpio_g_first ^ gpio_g) & GPIOCAP_SDB_MASK) != 0U)
        {
            g_sdb_unstable++;
        }

        if (((gpio_i_first ^ gpio_i) & GPIOCAP_WS_MASK) != 0U)
        {
            g_ws_unstable++;
        }

        const uint32_t ws =
            ((gpio_i & GPIOCAP_WS_MASK) != 0U) ? 1U : 0U;

        const uint32_t sd_a =
            ((gpio_i & GPIOCAP_SDA_MASK) != 0U) ? 1U : 0U;

        const uint32_t sd_b =
            ((gpio_g & GPIOCAP_SDB_MASK) != 0U) ? 1U : 0U;

        if (ws == 0U)
        {
            if (ws0_count < GPIOCAP_BITS_PER_SLOT)
            {
                a_ws0 = (a_ws0 << 1U) | sd_a;
                b_ws0 = (b_ws0 << 1U) | sd_b;
            }

            ws0_count++;
        }
        else
        {
            if (ws1_count < GPIOCAP_BITS_PER_SLOT)
            {
                a_ws1 = (a_ws1 << 1U) | sd_a;
                b_ws1 = (b_ws1 << 1U) | sd_b;
            }

            ws1_count++;
        }
    }

    frame->a_ws0 = a_ws0;
    frame->a_ws1 = a_ws1;
    frame->b_ws0 = b_ws0;
    frame->b_ws1 = b_ws1;

    if ((ws0_count != GPIOCAP_BITS_PER_SLOT) ||
        (ws1_count != GPIOCAP_BITS_PER_SLOT))
    {
        g_invalid_frames++;
    }

    return true;
}

static void print_gpio_register_state(void)
{
    const uint32_t moder_i = GPIOI->MODER;
    const uint32_t afr_i_0 = GPIOI->AFR[0];
    const uint32_t moder_g = GPIOG->MODER;
    const uint32_t afr_g_1 = GPIOG->AFR[1];

    printf(
        "[GPIOCAP_CFG] "
        "PI_MODER=%08lX PI_AFRL=%08lX "
        "PG_MODER=%08lX PG_AFRH=%08lX\r\n",
        (unsigned long)moder_i,
        (unsigned long)afr_i_0,
        (unsigned long)moder_g,
        (unsigned long)afr_g_1
    );

    printf(
        "[GPIOCAP_PINS] "
        "SCK=PI5 WS=PI7 SD_A=PI6 SD_B=PG10 "
        "sample=rising_mid_high keep_mode=AF10\r\n"
    );

    printf(
        "[GPIOCAP_TIMING] first_delay=%u cycles "
        "second_delay=%u cycles core=%luHz\r\n",
        (unsigned int)GPIOCAP_SAMPLE_DELAY_CYCLES,
        (unsigned int)(
            GPIOCAP_SAMPLE_DELAY_CYCLES +
            GPIOCAP_STABILITY_DELAY_CYCLES
        ),
        (unsigned long)SystemCoreClock
    );
}

__attribute__((noreturn))
void gpio_bus_capture_run_and_halt(void)
{
    printf(
        "[GPIOCAP_TEST] Direct GPIO IDR capture V2 mid-high, "
        "frames=%u bits/frame=%u\r\n",
        (unsigned int)GPIOCAP_FRAME_COUNT,
        (unsigned int)GPIOCAP_BITS_PER_FRAME
    );

    print_gpio_register_state();

    /*
     * Dejar que SAI2 y ambos DMA alcancen estado estable.
     * audio_capture_start() debe haberse ejecutado antes.
     */
    HAL_Delay(5U);

    dwt_cycle_counter_start();

    const uint32_t previous_primask = __get_PRIMASK();
    __disable_irq();
    __DSB();
    __ISB();

    uint32_t previous_rise_cycle = 0U;
    uint32_t captured_frames = 0U;
    bool capture_ok = true;

    for (uint32_t frame = 0U;
         frame < GPIOCAP_FRAME_COUNT;
         frame++)
    {
        if (!capture_one_frame(
                &g_gpio_frames[frame],
                &previous_rise_cycle))
        {
            capture_ok = false;
            break;
        }

        captured_frames++;
    }

    __DSB();
    __ISB();

    /*
     * Detener primero el bloque esclavo B y luego el maestro A.
     * Ya no se necesitan clocks despues de la captura.
     */
    (void)HAL_SAI_DMAStop(&hsai_BlockB2);
    (void)HAL_SAI_DMAStop(&hsai_BlockA2);

    if (previous_primask == 0U)
    {
        __enable_irq();
    }

    printf(
        "[GPIOCAP_BEGIN] captured=%lu requested=%u "
        "invalid_frames=%lu status=%s\r\n",
        (unsigned long)captured_frames,
        (unsigned int)GPIOCAP_FRAME_COUNT,
        (unsigned long)g_invalid_frames,
        capture_ok ? "OK" : "TIMEOUT"
    );

    if (g_edge_count > 0U)
    {
        const float average_cycles =
            (float)g_cycle_sum / (float)g_edge_count;

        const float estimated_bclk =
            (average_cycles > 0.0f)
                ? ((float)SystemCoreClock / average_cycles)
                : 0.0f;

        printf(
            "[GPIOCAP_CLK] core=%luHz edges=%lu "
            "cycles_min=%lu cycles_avg=%.2f cycles_max=%lu "
            "bclk_est=%.1fHz\r\n",
            (unsigned long)SystemCoreClock,
            (unsigned long)g_edge_count,
            (unsigned long)g_cycle_min,
            (double)average_cycles,
            (unsigned long)g_cycle_max,
            (double)estimated_bclk
        );
    }

    printf(
        "[GPIOCAP_STABILITY] SD_A=%lu SD_B=%lu WS=%lu "
        "comparisons=%lu\r\n",
        (unsigned long)g_sda_unstable,
        (unsigned long)g_sdb_unstable,
        (unsigned long)g_ws_unstable,
        (unsigned long)(captured_frames * GPIOCAP_BITS_PER_FRAME)
    );

    for (uint32_t frame = 0U;
         frame < captured_frames;
         frame++)
    {
        printf(
            "[GPIOCAP] %03lu "
            "B_WS0=%08lX B_WS1=%08lX "
            "A_WS0=%08lX A_WS1=%08lX\r\n",
            (unsigned long)frame,
            (unsigned long)g_gpio_frames[frame].b_ws0,
            (unsigned long)g_gpio_frames[frame].b_ws1,
            (unsigned long)g_gpio_frames[frame].a_ws0,
            (unsigned long)g_gpio_frames[frame].a_ws1
        );
    }

    printf("[GPIOCAP_END]\r\n");
    printf("[DET] Exportacion completa\r\n");

    while (1)
    {
        __WFI();
    }
}
