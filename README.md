# Portenta H7 — Captura de Audio 4 Canales

Guía resumida del proyecto **Modelo de Localización de Sonido**: captura simultánea de 4 micrófonos I2S sobre el bus SAI2 de un Arduino Portenta H7, con debug por UART/FTDI y programación vía ST-LINK.

---

## 1. Hardware

| Componente | Descripción | Notas |
|---|---|---|
| Arduino Portenta H7 | STM32H747XIH6 dual-core CM7 + CM4 | Núcleo activo: **CM7 a 480 MHz** |
| Portenta Breakout Board | Expone conectores HD J1/J2 | Pines SAI2 en **J2** |
| 4× Micrófono ICS-43434 | I2S 24 bits, 1.7–3.6 V | 2 mics por bus SAI |
| Adaptador FTDI | FT232RL, 3.3 V | Debug UART por puerto COM |
| ST-LINK V3 | Programador / Debugger | SWD por USB independiente |

> Durante el desarrollo se usan **3 puertos USB del PC al mismo tiempo**.

---

## 2. Conexiones USB simultáneas

| USB del PC | Conectado a | Función |
|---|---|---|
| USB 1 | Portenta H7 (USB-C nativo) | Alimentación + DFU bootloader |
| USB 2 | ST-LINK V2/V3 | Flash + Debug SWD |
| USB 3 | Adaptador FTDI | UART debug (printf/logs) |

### Conexión FTDI → Portenta Breakout

| Pin FTDI | Pin Breakout | Descripción |
|---|---|---|
| GND | GND (zona UART1) | Tierra |
| RX | J1-33 (UART1_TX / PA9) | El TX del STM32 alimenta el RX del FTDI |
| 3V3 | **NO conectar** | La Portenta tiene su propia alimentación |

Para escribir **desde** la Portenta hacia el FTDI se habilita **PA10 (USART1_RX)** y se conecta TX del FTDI → UART1_RX.

**PuTTY:** puerto COM correcto (COM6 en el ejemplo), 115200 baudios, *Flow Control* = None.

---

## 3. Micrófonos ICS-43434

Los 4 micrófonos comparten el bus **SAI2**; solo difieren en la línea de datos (SD) y en el pin **SEL**.

### Señales compartidas (iguales para los 4)

| Señal | Pin STM32 | Pin Breakout |
|---|---|---|
| SAI2_SCK (reloj) | PI5 | J2-49 |
| SAI2_FS (word select) | PI7 | J2-51 |
| 3.3 V | — | Sección SAI (VCC) |
| GND | — | Sección SAI |

### Señales individuales

| Micrófono | SEL (LR) | Línea de datos | Pin STM32 | Pin Breakout |
|---|---|---|---|---|
| Mic 1 | GND (slot L) | SAI2_SD_B | PG10 | J2-58 (GPIO6) |
| Mic 2 | 3.3 V (slot R) | SAI2_SD_B | PG10 | J2-58 (GPIO6) |
| Mic 3 | GND (slot L) | SAI2_SD_A | PI6 | J2-53 (SAI_D0) |
| Mic 4 | 3.3 V (slot R) | SAI2_SD_A | PI6 | J2-53 (SAI_D0) |

> Mic1+Mic2 comparten físicamente **SD_B**; Mic3+Mic4 comparten **SD_A**. Se distinguen por el pin SEL: GND = slot izquierdo (L), 3.3 V = slot derecho (R).

---

## 4. Configuración STM32CubeMX

### Reloj
- **PLLN = 192**, **VOLTAGE_SCALE1**.
- CPU a 480 MHz.

### SAI2
- **SAI A**: Master Receive, I2S Standard, 32 bits, 2 slots, 44.1 kHz.
- **SAI B**: Synchronous Slave (sincronizado con SAI A), Slave Receive, I2S Standard, 32 bits.
- Protocolo I2S/PCM activado en ambos bloques.

### GPIO Settings SAI2

| Pin | Señal |
|---|---|
| PG10 | SAI2_SD_B |
| PI5 | SAI2_SCK_A |
| PI6 | SAI2_SD_A |
| PI7 | SAI2_FS_A |

### Otros periféricos
- **I2C1** (PB7 SDA / PB8 SCL): bus privado interno → PMIC PF1550 (dir 0x08) + Cryptochip. 100 kHz, solo Cortex-M7.

---

## 5. ⚠️ Problema crítico: regenerar código desde CubeMX

Cada regeneración **sobrescribe archivos clave**. Qué revisar/restaurar:

| Archivo | Qué se pierde | Acción |
|---|---|---|
| `main.c` | Código fuera de los bloques `USER CODE BEGIN/END` | Restaurar desde git |
| `Drivers/` | HAL_Driver y CMSIS | `git restore --source=no-dual -- Drivers/` |
| `sai.c` / `i2c.c` | Se regeneran bien | Solo verificar pines y parámetros |
| `SystemClock_Config` | Puede cambiar PLLN / VOLTAGE_SCALE | Verificar **PLLN=192, SCALE1** |
| `PeriphCommonClock` | Puede volver a SAI1 | Verificar **Sai23ClockSelection** |

### Include Paths correctas (CM7)
```
../Core/Inc
../../Drivers/STM32H7xx_HAL_Driver/Inc
../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy
../../Drivers/CMSIS/Device/ST/STM32H7xx/Include
../../Drivers/CMSIS/Include
```

### Debug Configurations (Initialization commands)
```
set *(unsigned int *)0xE000ED94 = 0
set $sp = 0x24080000
set $pc = 0x080413b4
set $primask = 0
```

---

## 6. Verificación del sistema

Arranque correcto por UART (resumen): `UART_INIT_OK` → `BOOT` → `CM7: arranque OK` → `audio_capture_init OK` → `audio_capture_start OK` → `SAI BUSY_RX OK` → `entrando al loop`, seguido de líneas `[AUDIO] frames=... errors=0` con valores Mic1–Mic4.

> En silencio los valores son bajos pero **no constantes**; deben variar con el sonido ambiente.

### Diagnóstico de problemas comunes

| Síntoma | Causa probable | Solución |
|---|---|---|
| `PMIC: LDO2 ERROR` | I2C1 no inicializado o pines mal | Verificar `MX_I2C1_Init()` y PB7/PB8 |
| SAI no entra en `BUSY_RX` | Pines PE3/PE5 en lugar de PI5/PI7 | Revisar GPIO Settings SAI2 |
| No detecta micrófonos (FFFFFFFF / 00000000) | SCK/FS no llega a los mics | Confirmar J2-49 y J2-51 con multímetro/osciloscopio |
| Valores constantes -1 y 0 | Mics sin alimentación o sin clock | Medir 3 V en VCC y ~1.5 V en SCK |
| ~1746 errores de compilación | Drivers desactualizados tras CubeMX | Restaurar drivers |
| Error `Sai2ClockSelection` | Campo incorrecto (SAI2/SAI3 comparten clock) | Usar **`Sai23ClockSelection`** |

---

## 7. Glosario rápido

| Término | Significado |
|---|---|
| **SAI** | Serial Audio Interface — periférico del STM32 |
| **DMA** | Direct Memory Access — transfiere datos sin CPU |
| **PMIC** | Power Management IC (PF1550) — gestiona voltajes de la Portenta |
| **LDO2** | Regulador del PMIC que da 3.3 V a los micrófonos |
| **I2C1** | Bus I2C privado interno (PB7/PB8) → PMIC + Cryptochip |
| **BlockA / BlockB** | Sub-bloques del SAI2: A = Master, B = Slave sincronizado |
| **RAM_D2** | SRAM2 en 0x30000000 — accesible por DMA1, usada para buffers |
| **SEL / LR** | Pin del ICS-43434: GND = slot L, 3.3 V = slot R |
| **SAI23** | En el HAL del H747, SAI2 y SAI3 comparten el campo de clock RCC |
| **FTDI** | Adaptador USB-Serial → UART 3.3 V |
| **SWD** | Single Wire Debug — interfaz del ST-LINK |
| **CubeMX** | Herramienta de ST para configurar periféricos y generar código HAL |