# BasicSetUpMics2Pairs

## 1. Objetivo

`BasicSetUpMics2Pairs` valida la captura simultanea de cuatro microfonos digitales ICS-43434 sobre un STM32H747XI / Arduino Portenta H7 utilizando dos lineas de datos SAI y clocks compartidos.

La arquitectura final utiliza:

- `SAI2A` como `MASTER_RX` asincrono.
- `SAI2B` como `SLAVE_RX` sincronizado con `SAI2A`.
- `PI5 / SAI2_SCK_A` como `BCLK` compartido por los cuatro microfonos.
- `PI7 / SAI2_FS_A` como `WS / LRCL` compartido por los cuatro microfonos.
- `PI6 / SAI2_SD_A` como linea de datos del par A.
- `PG10 / SAI2_SD_B` como linea de datos del par B.
- Dos microfonos por linea DOUT, separados por `SEL`:
  - `SEL=GND -> SLOT0`
  - `SEL=VDD -> SLOT1`

El objetivo final de esta rama es confirmar que el STM32 puede adquirir cuatro canales de audio sincronizados, conservar 44,100 muestras por canal y transferirlos de forma robusta al PC.

---

## 2. Arquitectura general

![Esquema general BasicSetUpMics2Pairs](./Schema%20version%20BasicSetUpMics2Pairs.png)

```text
                            STM32H747XI / Portenta H7

                         SAI2A - MASTER_RX / ASYNC

PI5 / SAI2_SCK_A  ------------------------------------> BCLK comun
PI7 / SAI2_FS_A   ------------------------------------> WS / LRCL comun

PI6 / SAI2_SD_A   <-------------------- DOUT compartido Pair A
                                           |       |
                                         MIC6    MIC5
                                       SEL=GND  SEL=VDD
                                        SLOT0    SLOT1

PG10 / SAI2_SD_B  <-------------------- DOUT compartido Pair B
                                           |       |
                                         MIC2    MIC7
                                       SEL=GND  SEL=VDD
                                        SLOT0    SLOT1

                         SAI2B - SLAVE_RX / SYNC
```

Todos los microfonos comparten tambien `3V3` y `GND`.

La asignacion utilizada para la validacion final es:

| Canal STM32 | Linea | Slot | SEL | Microfono |
|---|---|---:|---|---|
| A_SLOT0 | PI6 / SD_A | 0 | GND | MIC6 |
| A_SLOT1 | PI6 / SD_A | 1 | VDD | MIC5 |
| B_SLOT0 | PG10 / SD_B | 0 | GND | MIC2 |
| B_SLOT1 | PG10 / SD_B | 1 | VDD | MIC7 |

MIC3 se excluyo de la integracion final porque durante las pruebas aisladas dejo de entregar audio util de forma fiable.

---

## 3. Fase 3.1 - Validacion aislada de SD_B / PG10

Antes de utilizar las dos lineas simultaneamente se valido `SAI2B / PG10` de forma aislada. `SAI2A` permanecio activo como maestro de clocks mientras los datos se recibian exclusivamente por `PG10 / SAI2_SD_B`. Esta fase comprobo que dos ICS-43434 pueden compartir una unica linea DOUT y separarse correctamente mediante `SEL`. La documentacion detallada de esta fase registra las sesiones 1052-1056 y las pruebas con MIC3/MIC7. fileciteturn159file0L5-L17

![Esquema Fase 3.1 SD_B](./Schema%20version%20BasicSetUpMicsPairs%20SD_B.png)

Configuracion utilizada:

```text
SAI2A
  Mode            = MASTER_RX
  Synchronization = ASYNCHRONOUS
  Protocol        = I2S standard
  Data size       = 24 bit
  Slots           = 2
  Sample rate     = 44.1 kHz
  DMA             = DMA1_Stream0

SAI2B
  Mode            = SLAVE_RX
  Synchronization = SYNCHRONOUS
  Protocol        = I2S standard
  Data size       = 24 bit
  Slots           = 2
  Data input      = PG10 / SAI2_SD_B
  DMA             = DMA1_Stream1
```

La fase aislada de SD_B termino en `PASS`: MIC5/MIC6 y MIC2/MIC4 funcionaron correctamente en ambas posiciones SEL, con 44,100 muestras por slot y canales distintos. fileciteturn159file0L454-L496

MIC3 se marco como no fiable para continuar la integracion porque fallo aislado tanto con `SEL=VDD` como con `SEL=GND` y perturbaba la captura al compartir DOUT con MIC7. fileciteturn159file0L398-L448

---

## 4. Fase 3.2 - Dos pares / cuatro microfonos simultaneos

La fase 3.2 conecta simultaneamente las dos lineas de datos:

```text
PAIR A
MIC6 SEL=GND --+
               +--> DOUT comun --> PI6 / SAI2_SD_A
MIC5 SEL=VDD --+

PAIR B
MIC2 SEL=GND --+
               +--> DOUT comun --> PG10 / SAI2_SD_B
MIC7 SEL=VDD --+

BCLK comun -> PI5
WS comun   -> PI7
```

`SAI2A` genera los clocks compartidos y recibe el Pair A. `SAI2B` permanece sincronizado con A y recibe el Pair B.

La configuracion SAI final relevante es:

```c
hsai_BlockA2.Init.AudioMode = SAI_MODEMASTER_RX;
hsai_BlockA2.Init.Synchro = SAI_ASYNCHRONOUS;
hsai_BlockA2.Init.TriState = SAI_OUTPUT_NOTRELEASED;

hsai_BlockB2.Init.AudioMode = SAI_MODESLAVE_RX;
hsai_BlockB2.Init.Synchro = SAI_SYNCHRONOUS;
hsai_BlockB2.Init.TriState = SAI_OUTPUT_RELEASED;
```

Ambos bloques utilizan protocolo I2S estandar, datos de 24 bits y dos slots por frame.

---

## 5. Captura y DMA

Cada bloque SAI utiliza DMA circular:

```text
SAI2A -> DMA1_Stream0 -> s_dma_raw_a
SAI2B -> DMA1_Stream1 -> s_dma_raw_b
```

Cada callback procesa dos slots por frame y genera cuatro buffers PCM16 independientes:

```text
s_pcm_a_slot0
s_pcm_a_slot1
s_pcm_b_slot0
s_pcm_b_slot1
```

La conversion utilizada es:

```c
static inline int16_t pcm24_to_pcm16(uint32_t value)
{
    int32_t sample = (int32_t)(value & 0x00FFFFFFU);

    if ((sample & 0x00800000L) != 0)
    {
        sample |= (int32_t)0xFF000000U;
    }

    return (int16_t)(sample >> 8);
}
```

La captura conserva:

- 3 s de pre-roll con clocks activos.
- 1 s exacto de audio.
- 44,100 frames por bloque SAI.
- 44,100 muestras por canal.
- DMA circular.
- Diagnostico separado para A y B.

---

## 6. Diagnostico integrado

Antes de la captura se comprueban las configuraciones GPIO y la actividad digital de las cuatro senales principales:

```text
PI5  -> BCLK
PI6  -> SD_A
PG10 -> SD_B
PI7  -> WS / LRCL
```

Los mensajes relevantes son:

```text
[PINCFG_A]
[PINCFG_B]
[DMA_A]
[DMA_B]
[GPIO_IDR]
[CAPTURE_DONE_A]
[CAPTURE_DONE_B]
```

Los contadores `PI5_trans`, `PI6_trans`, `PG10_trans` y `PI7_trans` son diagnosticos comparativos de actividad digital obtenidos mediante muestreo rapido de `GPIOx->IDR`; no representan directamente la frecuencia teorica de las senales.

---

## 7. Transferencia UART robusta

La transferencia de los cuatro canales utiliza UART a `1,000,000 baud` con CRC32 y ACK/NACK por bloque.

Canales:

```text
ch=0 -> A_SLOT0
ch=1 -> A_SLOT1
ch=2 -> B_SLOT0
ch=3 -> B_SLOT1
```

Estrategia de bloques:

```text
8192 bytes -> bloque principal
4096 bytes -> primer fallback
1024 bytes -> fallback final
```

Cada bloque incluye:

```text
[BLK] ch=<n> seq=<n> off=<n> len=<n> crc=<CRC32> try=<n>
```

El PC valida CRC32 y responde:

```text
A -> ACK
N -> NACK
```

Un NACK o timeout provoca retransmision del mismo offset utilizando el esquema de fallback. La prueba de cuatro canales demostro que el sistema puede recuperar bloques perdidos sin perder el framing del canal.

---

## 8. Incidencia de cableado durante la validacion final

Durante las primeras pruebas de Fase 3.2 se observo:

```text
BCLK / PI5  -> activo en STM32
WS / PI7    -> activo en STM32
SD_A / PI6  -> LOW
SD_B / PG10 -> LOW
```

Esto produjo buffers RAW y PCM completamente nulos en las cuatro salidas, aunque ambos DMA continuaban generando correctamente sus callbacks.

La causa final no fue firmware: se encontro un problema fisico en el empalme compartido de `BCLK`. Uno de los empates no habia quedado soldado correctamente y el empalme se habia soltado. Por ello los clocks existian en el pin STM32, pero no llegaban correctamente a los microfonos.

Despues de rehacer y soldar correctamente el empalme BCLK:

- el firmware historico v2.1.7 funciono correctamente con el montaje actual;
- `BasicSetUpMics2Pairs` tambien funciono correctamente;
- las dos lineas de datos `SD_A` y `SD_B` volvieron a entregar audio;
- la arquitectura de cuatro microfonos quedo validada.

Por tanto, las sesiones realizadas durante el fallo de BCLK que mostraban `PI6_trans=0`, `PG10_trans=0` y audio completamente nulo deben interpretarse como pruebas afectadas por una incidencia de cableado, no como evidencia de fallo del SAI o del firmware.

---

## 9. Resultado final

La arquitectura `BasicSetUpMics2Pairs` queda validada para cuatro microfonos simultaneos:

```text
MIC6 -> A_SLOT0 -> PI6 / SAI2_SD_A
MIC5 -> A_SLOT1 -> PI6 / SAI2_SD_A
MIC2 -> B_SLOT0 -> PG10 / SAI2_SD_B
MIC7 -> B_SLOT1 -> PG10 / SAI2_SD_B
```

Con:

```text
BCLK compartido -> PI5
WS compartido   -> PI7
Sample rate     -> 44.1 kHz
Formato SAI     -> I2S, 24 bit, 2 slots
Salidas PC      -> 4 canales PCM16 / JSON
UART            -> 1,000,000 baud + CRC32 + ACK/NACK
```

La comparacion final con la v2.1.7 tambien confirma que el montaje fisico corregido funciona tanto con la arquitectura historica estable como con la implementacion `BasicSetUpMics2Pairs`.

---

## 10. Archivos principales

```text
CM7/Core/Src/main.c
CM7/Core/Src/sai.c
CM7/Core/Inc/sai.h
CM7/Core/Src/dma.c
CM7/Core/Src/stm32h7xx_it.c

Python/grabar_sesion_2pairs.py

README_BasicSetUpMics2Pairs.md
README_BasicSetUpMics2Pairs-SD_B.md
Schema version BasicSetUpMicsPairs SD_B.png
Schema version BasicSetUpMics2Pairs.png
```

---

## 11. Estado

```text
Fase 3.1 - SD_B / PG10 aislado       PASS
Fase 3.2 - SD_A + SD_B simultaneos   PASS
4 microfonos simultaneos             PASS
UART robusto CRC32 + ACK/NACK         PASS
Comparacion funcional con v2.1.7     PASS
```

`BasicSetUpMics2Pairs` queda como base validada para continuar la integracion de los cuatro canales de audio en las siguientes etapas del proyecto.
