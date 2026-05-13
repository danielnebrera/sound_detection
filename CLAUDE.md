# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Dual-core firmware for the **Arduino Portenta H7** (STM32H747XIHx — Cortex-M7 + Cortex-M4). CM7 drives an RGB LED sequence; CM4 captures 4-channel audio via SAI/DMA and runs diagnostic logging.

## Build System

This project uses **STM32CubeIDE** (Eclipse CDT + GCC `arm-none-eabi`). There is no standalone Makefile — builds are IDE-managed.

- Open `Portenta_H7_BlinkingLED.ioc` in STM32CubeMX to regenerate peripheral init code.
- Build each core as a separate project inside CubeIDE: **CM4** and **CM7** compile and link independently.
- Linker scripts per core: `STM32H747XIHX_FLASH.ld` (production) and `STM32H747XIHX_RAM.ld` (debug RAM execution).
- Optimization: `-O2` for Release builds.

## Flashing & Debugging

Two separate debug sessions must be launched — one per core:

| Core | Launch file | Interface |
|------|-------------|-----------|
| CM7  | `CM7/Portenta_H7_BlinkingLED_CM7.launch` + `.cfg` | OpenOCD + STM32-STLINK-V3, GDB port 3333 |
| CM4  | `CM4/Portenta_H7_BlinkingLED_CM4 Debug.launch` | ST-LINK GDB Server, access port AP3, GDB port 61234 |

CM7 must be flashed/started first — it is the master that wakes CM4 via HSEM.

## Dual-Core Boot Synchronization

The boot handshake is defined in `Common/system_stm32h7xx_dualcore_boot_cm4_cm7.c` and called from both `main.c` files:

1. **CM7 (master):** waits for D2 clock domain ready → takes HSEM 0 → releases it → enables `RCC_BOOT_C2`.
2. **CM4 (slave):** enters `WFE` stop mode → wakes on HSEM notification → clears flag → continues.

Do not reorder initialization before this handshake; peripherals on D2/D3 domains may not be clocked yet.

## Memory Map (CM4)

| Region | Address | Size | Use |
|--------|---------|------|-----|
| FLASH (CM4) | `0x08170000` | 576 KB | CM4 firmware |
| DTCMRAM | `0x10000000` | 288 KB | Stack, heap, logging buffer |
| Logging buffer | `0x10000200` | 4 KB | `flash_logger` ring in DTCM |
| SRAM4 (backup) | `0x38800000` | 16 KB | Survives reset while powered |

## Key Modules

### Flash Logger (`CM4/Core/Src/flash_logger.c`)
Non-volatile diagnostic log stored in DTCMRAM (survives soft-reset while power is held). Validated by magic number `0xDEADBEEF`. Linear fill — no wrap-around. API: `flash_logger_init()`, `flash_log(fmt, ...)`, `flash_logger_get_buffer()`, `flash_logger_clear()`.

### Audio Capture (`CM4/Core/Components/Audio/audio_capture.c`)
4-channel microphone input at 44.1 kHz via SAI1-A and SAI1-B (each stereo). DMA double-buffer of 1024 samples (512/half). `audio_capture_deinterleave()` separates interleaved SAI words into four mono buffers `ch0`–`ch3`. Clock source: PLL3 @ 72 MHz.

### UART / Trace (`CM4/Core/Src/uart_driver.c`)
`printf` is redirected through ITM (`ITM_SendChar`) over the SWD trace channel — no physical UART is configured.

## Error Signaling Convention (CM4)

LED color on error (200 ms fast blink via CM7 GPIO K5/K6/K7):

- Red (`GPIO K5`) → audio init failure
- Green (`GPIO K6`) → clock domain issue
- Blue (`GPIO K7`) → audio start failure

The error path does **not** call a blocking `Error_Handler`; it loops with 200 ms toggles so state can be inspected via debugger before power loss.

## Clock Configuration

- PLL1 → 240 MHz (both Cortex cores)
- PLL3 → 72 MHz (SAI1 audio, fractional-divided to produce 44.1 kHz bit clock)
- HSE: 5 MHz external oscillator on PH0/PH1
