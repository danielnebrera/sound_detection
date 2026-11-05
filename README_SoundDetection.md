# SoundDetection — STM32 + Khamex

**Checkpoint: `SPATIAL-P3.4-RPIPE1-PCM48` — 30 de octubre de 2026**  
**Estado:** captura LIVE y exportación de snapshots PCM verificadas; localización 3D aún experimental.

Este README describe **el estado operativo actual** para continuar el desarrollo. Sustituye las cronologías extensas de ensayos por las decisiones y resultados consolidados. La base histórica del firmware es **v2.1.7**, commit `26ba5c6`.

## 1. Plataforma y arquitectura

- **Placa:** Arduino Portenta H7, STM32H747XI, núcleo CM7.
- **Micrófonos:** 4 × ICS-43434, adquisición sincronizada a **44.100 Hz** por canal.
- **Entrada:** SAI2A maestro y SAI2B esclavo, BCLK/WS compartidos y dos líneas de datos.
- **Captura:** DMA a buffers ping-pong en SRAM D2; copia desde CPU/ISR al anillo en SDRAM. **No es DMA directo a SDRAM.**
- **SDRAM:** 8 MiB desde `0x60000000`; región configurada como no cacheable en MPU.
- **LIVE:** productor-consumidor continuo, procesamiento por segundo, cierre limpio al solicitar STOP al final del segundo; sin UART durante la fase crítica de captura.
- **Detección:** MFCC + TFLite Micro, alerta de dron y TOA sólo cuando hay objetivo operativo.
- **PC:** monitor de radar 3D experimental, tracker temporal y recepción PCM por UART a 1.000.000 baudios.

### Mapeo lógico vigente

| Canal | Micrófono actual | Etiqueta lógica |
|---|---|---|
| CH0 | MIC2 | LEFT |
| CH1 | MIC6 | TOP |
| CH2 | MIC5 | BACK |
| CH3 | MIC7 | RIGHT |

Las etiquetas lógicas proceden del software; no equivalen necesariamente a las posiciones geométricas del montaje. Conservar este mapeo antes de modificar correlaciones o visualizaciones.

## 2. Detección y localización: estado actual

- **TOA W100:** ventana central de **4.410 muestras (100 ms)** por canal, dentro de cada bloque de un segundo.
- **TDOA:** correlación normalizada, estimación submuestra, tres picos candidatos por pareja y selección global de los seis pares.
- **Validación:** coherencia geométrica (*closure*) máxima **1,50 muestras**; la recuperación multi-pico exige un score de al menos el 95 % del candidato principal.
- **Confidence:** `HIGH`, `MEDIUM`, `LOW`, `NONE`. Únicamente `HIGH/MEDIUM` pueden actualizar el tracker; `LOW/INVALID` no se convierten en posiciones válidas.
- **Tracking:** TOA-1F.1 como referencia 2D; monitor y seguimiento 3D experimentales (`TRACK3D-R2`). Estados como `COASTING/LOST` no representan nuevas mediciones acústicas.
- **Compás del monitor:** NORTE = 0°, OESTE = 90°, SUR = 180°, ESTE = 270°.
- **Elevación:** modelos tetraédricos y evaluación offline todavía en desarrollo. No existe calibración absoluta independiente ni estimación fiable de distancia al dron.

**Pendiente separado:** continuidad TOA, selección de picos ambiguos, validación geométrica de elevación y precisión con posiciones medidas. Los fallos `TOA INVALID` no equivalen a errores de transferencia PCM.

## 3. SPATIAL-P3.4 — problema de integridad SDRAM resuelto en la campaña

### Hallazgo

Durante la exportación de snapshots PCM16 almacenados en SDRAM, las copias repetidas de una misma ventana producían **CRC32 diferentes**, provocando:

```text
[SPCM_TRANSFER_ERROR] stage=ISOLATION
```

El diagnóstico de lectura (`memcpy`, accesos `volatile` de 8/16/32 bits y barrido CRC completo) identificó discrepancias repetidas de **un solo bit**: `XOR = 0x40` (bit 6 del byte bajo de muestras PCM16). También se observaron lecturas variables de una misma dirección. No se detectó solapamiento entre el anillo de audio y los 12 snapshots, y la SDRAM estaba configurada como no cacheable.

### Corrección aplicada

Se realizó un A/B cambiando **una sola línea** en `portenta_sdram.c`:

```c
// Anterior (RPIPE0)
s_hsdram1.Init.ReadPipeDelay = FMC_SDRAM_RPIPE_DELAY_0;

// Checkpoint actual (RPIPE1)
s_hsdram1.Init.ReadPipeDelay = FMC_SDRAM_RPIPE_DELAY_1;
```

**No se modificaron** CAS, ReadBurst, frecuencia/reloj FMC, refresco, MPU, SAI/DMA, recorder, MFCC, TFLite, TOA ni protocolo de exportación. El resultado respalda una **limitación del margen temporal de lectura FMC** con RPIPE0; no demuestra por sí solo un defecto físico del chip SDRAM.

### Validación consolidada

En **cuatro sesiones LIVE consecutivas** con RPIPE1, incluyendo NORTE y cambios de elevación de la fuente:

| Comprobación | Resultado |
|---|---:|
| Sesiones completadas | **4/4** |
| Snapshots PCM16 exportados | **48/48** |
| CRC32 global firmware–PC | **48/48 correctos** |
| Barridos con `mismatch`, `v16_unstable`, `xor40` u `other` distintos de cero | **0** |
| Snapshots con `src_diff` o `ram_mismatch` | **0** |
| Errores `SPCM_TRANSFER_ERROR` | **0** |
| Errores de captura reportados | **0** |

Cada snapshot contiene **4 × 4.410 muestras PCM16 = 35.280 bytes**; las 48 transferencias abarcan **1.693.440 bytes** de PCM. Todas las sesiones llegaron a:

```text
[SPCM_BATCH_END] count=12 planned=12
```

**Conclusión:** RPIPE1 eliminó el fallo reproducible de inconsistencias SPCM en esta campaña y queda **aceptado como checkpoint funcional**, sujeto a validaciones prolongadas futuras si cambian temperatura, carga o configuración. Conservar RPIPE0 únicamente como referencia/reversión.

## 4. Configuración que debe conservarse

```text
Firmware base         v2.1.7 estable (26ba5c6)
MCU / placa           STM32H747XI / Portenta H7 (CM7)
Audio                 44.100 Hz, cuatro canales sincronizados
Captura               SAI2 + DMA D2 + anillo SDRAM, LIVE + STOP
SDRAM                 8 MiB @ 0x60000000, MPU no cacheable
FMC ReadPipeDelay     FMC_SDRAM_RPIPE_DELAY_1  [CORRECCIÓN]
TOA                   W100, seis TDOA, multi-peak + closure
Tracker / radar       F1.1 2D de referencia; TRACK3D-R2 experimental
SPCM                  12 snapshots de 100 ms, planar PCM16LE
UART PCM              1.000.000 baudios, SPB2/ACK/CRC32
```

No retirar las comprobaciones de CRC ni dar por resuelta la precisión angular por haber estabilizado SDRAM. Antes de una optimización nueva, cambiar una sola variable y comparar contra este checkpoint.

## 5. Checkpoint Git y continuidad

**Próximo trabajo:** estudiar offline los `TOA_PEAKS` y las ventanas PCM ya verificadas, especialmente casos `INVALID` y elevaciones variables. Mejorar robustez del solver 3D y del tracker sin alterar inicialmente el controlador FMC ni la arquitectura de captura.
