#ifndef PORTENTA_SDRAM_H
#define PORTENTA_SDRAM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORTENTA_SDRAM_BASE_ADDRESS  0x60000000UL
#define PORTENTA_SDRAM_SIZE_BYTES    (8UL * 1024UL * 1024UL)

typedef enum
{
    PORTENTA_SDRAM_STAGE_NONE = 0,
    PORTENTA_SDRAM_STAGE_INIT,
    PORTENTA_SDRAM_STAGE_DATA_BUS,
    PORTENTA_SDRAM_STAGE_ADDRESS_BUS,
    PORTENTA_SDRAM_STAGE_PATTERN_AA,
    PORTENTA_SDRAM_STAGE_PATTERN_55,
    PORTENTA_SDRAM_STAGE_ADDRESS_PATTERN
} PortentaSdramStage;

typedef struct
{
    PortentaSdramStage stage;
    uint32_t address;
    uint32_t expected;
    uint32_t actual;
} PortentaSdramTestReport;

bool portenta_sdram_init(void);
bool portenta_sdram_quick_test(PortentaSdramTestReport *report);
bool portenta_sdram_full_test(PortentaSdramTestReport *report);
const char *portenta_sdram_stage_name(PortentaSdramStage stage);

#ifdef __cplusplus
}
#endif

#endif /* PORTENTA_SDRAM_H */
