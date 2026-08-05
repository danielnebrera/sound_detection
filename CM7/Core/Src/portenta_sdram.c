#include "portenta_sdram.h"

#include "main.h"
#include "stm32h7xx_hal_sdram.h"

#include <stddef.h>
#include <string.h>

#define SDRAM_TIMEOUT                    0x1000UL
#define SDRAM_REFRESH_COUNT              1542UL

#define SDRAM_MODE_BURST_LENGTH_1         0x0000U
#define SDRAM_MODE_BURST_TYPE_SEQUENTIAL  0x0000U
#define SDRAM_MODE_CAS_LATENCY_2           0x0020U
#define SDRAM_MODE_OPERATING_STANDARD      0x0000U
#define SDRAM_MODE_WRITEBURST_SINGLE       0x0200U

static SDRAM_HandleTypeDef s_hsdram1;

static void report_clear(PortentaSdramTestReport *report)
{
    if (report != NULL)
    {
        memset(report, 0, sizeof(*report));
    }
}

static bool report_failure(
    PortentaSdramTestReport *report,
    PortentaSdramStage stage,
    uint32_t address,
    uint32_t expected,
    uint32_t actual)
{
    if (report != NULL)
    {
        report->stage = stage;
        report->address = address;
        report->expected = expected;
        report->actual = actual;
    }

    return false;
}

static HAL_StatusTypeDef configure_fmc_clock(void)
{
    RCC_PeriphCLKInitTypeDef clock = {0};

    /*
     * Portenta H7: HSE = 25 MHz.
     * PLL2 input  = 25 / 5   = 5 MHz.
     * PLL2 VCO    = 5 * 80   = 400 MHz.
     * FMC kernel  = 400 / 2  = 200 MHz.
     * SDRAM clock = FMC / 2  = 100 MHz.
     */
    clock.PeriphClockSelection = RCC_PERIPHCLK_FMC;
    clock.FmcClockSelection = RCC_FMCCLKSOURCE_PLL2;

    clock.PLL2.PLL2M = 5U;
    clock.PLL2.PLL2N = 80U;
    clock.PLL2.PLL2P = 2U;
    clock.PLL2.PLL2Q = 2U;
    clock.PLL2.PLL2R = 2U;
    clock.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_2;
    clock.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
    clock.PLL2.PLL2FRACN = 0U;

    return HAL_RCCEx_PeriphCLKConfig(&clock);
}

static void configure_fmc_gpio(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_MEDIUM;
    gpio.Alternate = GPIO_AF12_FMC;

    gpio.Pin =
        GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 |
        GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Pin =
        GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_7 | GPIO_PIN_8 |
        GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
        GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOE, &gpio);

    gpio.Pin =
        GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
        GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 |
        GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOF, &gpio);

    gpio.Pin =
        GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 |
        GPIO_PIN_5 | GPIO_PIN_8 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOG, &gpio);

    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOH, &gpio);
}

static bool send_sdram_command(
    uint32_t mode,
    uint32_t refresh_count,
    uint32_t mode_register)
{
    FMC_SDRAM_CommandTypeDef command = {0};

    command.CommandMode = mode;
    command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
    command.AutoRefreshNumber = refresh_count;
    command.ModeRegisterDefinition = mode_register;

    return HAL_SDRAM_SendCommand(
        &s_hsdram1,
        &command,
        SDRAM_TIMEOUT
    ) == HAL_OK;
}

static bool initialize_device_sequence(void)
{
    const uint32_t mode_register =
        SDRAM_MODE_BURST_LENGTH_1 |
        SDRAM_MODE_BURST_TYPE_SEQUENTIAL |
        SDRAM_MODE_CAS_LATENCY_2 |
        SDRAM_MODE_OPERATING_STANDARD |
        SDRAM_MODE_WRITEBURST_SINGLE;

    if (!send_sdram_command(FMC_SDRAM_CMD_CLK_ENABLE, 1U, 0U))
    {
        return false;
    }

    HAL_Delay(100U);

    if (!send_sdram_command(FMC_SDRAM_CMD_PALL, 1U, 0U))
    {
        return false;
    }

    if (!send_sdram_command(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8U, 0U))
    {
        return false;
    }

    if (!send_sdram_command(FMC_SDRAM_CMD_LOAD_MODE, 1U, mode_register))
    {
        return false;
    }

    return HAL_SDRAM_ProgramRefreshRate(
        &s_hsdram1,
        SDRAM_REFRESH_COUNT
    ) == HAL_OK;
}

bool portenta_sdram_init(void)
{
    FMC_SDRAM_TimingTypeDef timing = {0};

    if (configure_fmc_clock() != HAL_OK)
    {
        return false;
    }

    __HAL_RCC_FMC_CLK_ENABLE();
    configure_fmc_gpio();

    s_hsdram1.Instance = FMC_SDRAM_DEVICE;
    s_hsdram1.Init.SDBank = FMC_SDRAM_BANK1;
    s_hsdram1.Init.ColumnBitsNumber = FMC_SDRAM_COLUMN_BITS_NUM_8;
    s_hsdram1.Init.RowBitsNumber = FMC_SDRAM_ROW_BITS_NUM_12;
    s_hsdram1.Init.MemoryDataWidth = FMC_SDRAM_MEM_BUS_WIDTH_16;
    s_hsdram1.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;
    s_hsdram1.Init.CASLatency = FMC_SDRAM_CAS_LATENCY_2;
    s_hsdram1.Init.WriteProtection = FMC_SDRAM_WRITE_PROTECTION_DISABLE;
    s_hsdram1.Init.SDClockPeriod = FMC_SDRAM_CLOCK_PERIOD_2;
    s_hsdram1.Init.ReadBurst = FMC_SDRAM_RBURST_ENABLE;
    s_hsdram1.Init.ReadPipeDelay = FMC_SDRAM_RPIPE_DELAY_0;

    timing.LoadToActiveDelay = 2U;
    timing.ExitSelfRefreshDelay = 7U;
    timing.SelfRefreshTime = 5U;
    timing.RowCycleDelay = 7U;
    timing.WriteRecoveryTime = 2U;
    timing.RPDelay = 3U;
    timing.RCDDelay = 3U;

    if (HAL_SDRAM_Init(&s_hsdram1, &timing) != HAL_OK)
    {
        return false;
    }

    if (!initialize_device_sequence())
    {
        return false;
    }

    /*
     * FMC SDRAM Bank1 normally appears at 0xC0000000.
     * Portenta maps it to 0x60000000 using the FMC bank swap.
     */
    HAL_SetFMCMemorySwappingConfig(FMC_SWAPBMAP_SDRAM_SRAM);
    __DSB();
    __ISB();

    return true;
}

bool portenta_sdram_quick_test(PortentaSdramTestReport *report)
{
    volatile uint16_t *const base16 =
        (volatile uint16_t *)PORTENTA_SDRAM_BASE_ADDRESS;

    volatile uint32_t *const base32 =
        (volatile uint32_t *)PORTENTA_SDRAM_BASE_ADDRESS;

    const uint32_t word_count =
        PORTENTA_SDRAM_SIZE_BYTES / sizeof(uint32_t);

    const uint32_t pattern = 0xAAAAAAAAUL;
    const uint32_t antipattern = 0x55555555UL;

    report_clear(report);

    /* Data bus: walk one bit over the physical 16-bit bus. */
    for (uint16_t value = 1U; value != 0U; value <<= 1U)
    {
        base16[0] = value;
        __DSB();

        const uint16_t actual = base16[0];
        if (actual != value)
        {
            return report_failure(
                report,
                PORTENTA_SDRAM_STAGE_DATA_BUS,
                PORTENTA_SDRAM_BASE_ADDRESS,
                value,
                actual
            );
        }
    }

    /* Put a pattern on every power-of-two address line. */
    for (uint32_t offset = 1U;
         offset < word_count;
         offset <<= 1U)
    {
        base32[offset] = pattern;
    }

    base32[0] = antipattern;
    __DSB();

    for (uint32_t offset = 1U;
         offset < word_count;
         offset <<= 1U)
    {
        const uint32_t actual = base32[offset];
        if (actual != pattern)
        {
            return report_failure(
                report,
                PORTENTA_SDRAM_STAGE_ADDRESS_BUS,
                PORTENTA_SDRAM_BASE_ADDRESS + offset * sizeof(uint32_t),
                pattern,
                actual
            );
        }
    }

    /* Check that toggling one address does not alter another one. */
    for (uint32_t test_offset = 1U;
         test_offset < word_count;
         test_offset <<= 1U)
    {
        base32[test_offset] = antipattern;
        __DSB();

        if (base32[0] != antipattern)
        {
            return report_failure(
                report,
                PORTENTA_SDRAM_STAGE_ADDRESS_BUS,
                PORTENTA_SDRAM_BASE_ADDRESS,
                antipattern,
                base32[0]
            );
        }

        for (uint32_t verify_offset = 1U;
             verify_offset < word_count;
             verify_offset <<= 1U)
        {
            if (verify_offset == test_offset)
            {
                continue;
            }

            const uint32_t actual = base32[verify_offset];
            if (actual != pattern)
            {
                return report_failure(
                    report,
                    PORTENTA_SDRAM_STAGE_ADDRESS_BUS,
                    PORTENTA_SDRAM_BASE_ADDRESS +
                        verify_offset * sizeof(uint32_t),
                    pattern,
                    actual
                );
            }
        }

        base32[test_offset] = pattern;
    }

    return true;
}

static bool fill_and_verify(
    uint32_t pattern,
    PortentaSdramStage stage,
    PortentaSdramTestReport *report)
{
    volatile uint32_t *const memory =
        (volatile uint32_t *)PORTENTA_SDRAM_BASE_ADDRESS;

    const uint32_t word_count =
        PORTENTA_SDRAM_SIZE_BYTES / sizeof(uint32_t);

    for (uint32_t i = 0U; i < word_count; i++)
    {
        memory[i] = pattern;
    }

    __DSB();

    for (uint32_t i = 0U; i < word_count; i++)
    {
        const uint32_t actual = memory[i];

        if (actual != pattern)
        {
            return report_failure(
                report,
                stage,
                PORTENTA_SDRAM_BASE_ADDRESS + i * sizeof(uint32_t),
                pattern,
                actual
            );
        }
    }

    return true;
}

bool portenta_sdram_full_test(PortentaSdramTestReport *report)
{
    volatile uint32_t *const memory =
        (volatile uint32_t *)PORTENTA_SDRAM_BASE_ADDRESS;

    const uint32_t word_count =
        PORTENTA_SDRAM_SIZE_BYTES / sizeof(uint32_t);

    report_clear(report);

    if (!fill_and_verify(
            0xAAAAAAAAUL,
            PORTENTA_SDRAM_STAGE_PATTERN_AA,
            report))
    {
        return false;
    }

    if (!fill_and_verify(
            0x55555555UL,
            PORTENTA_SDRAM_STAGE_PATTERN_55,
            report))
    {
        return false;
    }

    for (uint32_t i = 0U; i < word_count; i++)
    {
        memory[i] = 0xA5A50000UL ^ i;
    }

    __DSB();

    for (uint32_t i = 0U; i < word_count; i++)
    {
        const uint32_t expected = 0xA5A50000UL ^ i;
        const uint32_t actual = memory[i];

        if (actual != expected)
        {
            return report_failure(
                report,
                PORTENTA_SDRAM_STAGE_ADDRESS_PATTERN,
                PORTENTA_SDRAM_BASE_ADDRESS + i * sizeof(uint32_t),
                expected,
                actual
            );
        }
    }

    return true;
}

const char *portenta_sdram_stage_name(PortentaSdramStage stage)
{
    switch (stage)
    {
        case PORTENTA_SDRAM_STAGE_INIT:
            return "INIT";

        case PORTENTA_SDRAM_STAGE_DATA_BUS:
            return "DATA_BUS";

        case PORTENTA_SDRAM_STAGE_ADDRESS_BUS:
            return "ADDRESS_BUS";

        case PORTENTA_SDRAM_STAGE_PATTERN_AA:
            return "PATTERN_AA";

        case PORTENTA_SDRAM_STAGE_PATTERN_55:
            return "PATTERN_55";

        case PORTENTA_SDRAM_STAGE_ADDRESS_PATTERN:
            return "ADDRESS_PATTERN";

        case PORTENTA_SDRAM_STAGE_NONE:
        default:
            return "NONE";
    }
}
