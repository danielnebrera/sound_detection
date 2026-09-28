# BasicSetUpMics2Pairs - Fase 3.1 - Validacion aislada de SD_B / PG10

## 1. Objetivo

Esta fase valida de forma aislada la segunda linea de datos del sistema de audio del STM32H747XI:

- `SAI2A` permanece activo como maestro y genera los clocks compartidos.
- `PI5 / SAI2_SCK_A` genera `BCLK`.
- `PI7 / SAI2_FS_A` genera `WS / LRCL`.
- `SAI2B` trabaja como receptor sincronizado.
- `PG10 / SAI2_SD_B` recibe el `DOUT` compartido por un par de microfonos ICS-43434.
- `PI6 / SAI2_SD_A` queda sin microfonos durante esta fase.

El objetivo es comprobar que SD_B puede recibir correctamente dos microfonos que comparten una sola linea DOUT y se separan mediante `SEL`:

- `SEL=GND -> SLOT0`
- `SEL=VDD -> SLOT1`

Esta fase se realiza antes de conectar simultaneamente los dos pares y los cuatro microfonos.

---

## 2. Arquitectura de la prueba

```text
                     SAI2A
                  MASTER_RX
                      |
             genera BCLK + LRCL
              PI5          PI7
               |            |
               +------------+--------------------+
                                                     
MIC A DOUT ----+                                      
               +----------> PG10 / SAI2_SD_B
MIC B DOUT ----+                    |
                                    v
                               SAI2 Block B
                                SLAVE_RX
                              SYNCHRONOUS
                                    |
                              DMA1_Stream1
                                    |
                              SLOT0 + SLOT1
```

Durante esta fase:

```text
PI5  -> BCLK compartido
PI7  -> WS / LRCL compartido
PG10 -> DOUT compartido del par bajo prueba
PI6  -> sin microfonos conectados
```

---

## 3. Configuracion SAI y DMA

### SAI2A

```text
Instance       = SAI2_Block_A
Mode           = MASTER_RX
Synchronization= ASYNCHRONOUS
Protocol       = I2S standard
Data size      = 24 bit
Slots          = 2
Sample rate    = 44.1 kHz
DMA            = DMA1_Stream0
DMA request    = SAI2_A
```

SAI2A se mantiene activo para generar `BCLK` y `WS/LRCL`. Los datos recibidos por A no se utilizan para crear los WAV de esta fase.

### SAI2B

```text
Instance       = SAI2_Block_B
Mode           = SLAVE_RX
Synchronization= SYNCHRONOUS
Protocol       = I2S standard
Data size      = 24 bit
Slots          = 2
Sample rate    = 44.1 kHz
Data input     = PG10 / SAI2_SD_B
DMA            = DMA1_Stream1
DMA request    = SAI2_B
```

Los callbacks de audio procesan exclusivamente los eventos de `SAI2_Block_B`.

---

## 4. Diagnostico utilizado

La captura conserva la misma estrategia usada previamente en `BasicSetUpMicsPairs`:

- 3 s de pre-roll con clocks activos.
- Captura exacta de 1 s.
- 44,100 muestras por slot.
- DMA circular.
- Conversion PCM24 -> PCM16.
- Transferencia por UART a 1,000,000 baudios.
- Generacion posterior de JSON y WAV.

Durante cada prueba se comprueban tambien:

```text
PI5_trans  -> actividad BCLK
PG10_trans -> actividad DOUT / SD_B
PI7_trans  -> actividad WS / LRCL
raw_nz0    -> muestras RAW no nulas SLOT0
raw_nz1    -> muestras RAW no nulas SLOT1
pcm_nz0    -> muestras PCM no nulas SLOT0
pcm_nz1    -> muestras PCM no nulas SLOT1
```

La configuracion correcta de PG10 observada durante las pruebas fue:

```text
[PINCFG] PG10 mode=2 pupd=0 af=10
```

---

# 5. Resultados

## 5.1 MIC5 + MIC6

### Sesion 1052

Configuracion:

```text
MIC5 SEL=VDD -> SLOT1
MIC6 SEL=GND -> SLOT0
DOUT comun   -> PG10 / SD_B
```

Diagnostico STM32:

```text
PG10_trans = 77664
PI7_trans  = 8801
raw_nz0    = 44099
raw_nz1    = 44099
pcm_nz0    = 44048
pcm_nz1    = 44038
```

Metricas:

| Canal | Microfono | RMS | dBFS | Peak |
|---|---|---:|---:|---:|
| SLOT0 | MIC6 | 0.008881 | -41.03 | 0.04135 |
| SLOT1 | MIC5 | 0.007579 | -42.41 | 0.03265 |

Comparacion entre slots:

```text
samples        = 44100
exact_equal    = 84
equal_pct      = 0.1905 %
correlation    = 0.7702
rms_difference = 0.005712
```

Resultado:

**PASS.** Ambos WAV contienen audio util y los dos slots son distintos.

---

### Sesion 1053 - prueba invalida por contacto SEL

Configuracion pretendida:

```text
MIC5 SEL=GND -> SLOT0
MIC6 SEL=VDD -> SLOT1
```

Resultado anormal:

```text
SLOT0 RMS = 0.56425 -> -4.97 dBFS
SLOT1 RMS = 0.51473 -> -5.77 dBFS
peak SLOT0 = 1.0
peak SLOT1 = 1.0
PG10_trans = 192458
correlation = -0.0040
rms_difference = 0.7651
```

Ambos WAV presentaron interferencia/corrupcion fuerte.

Despues de recolocar fisicamente los contactos SEL, sin cambiar firmware ni arquitectura, la siguiente captura funciono correctamente. Por ello la sesion 1053 se considera **invalida como prueba funcional de SD_B** y se conserva como evidencia de un problema fisico temporal de contacto en SEL.

---

### Sesion 1054

Configuracion:

```text
MIC5 SEL=GND -> SLOT0
MIC6 SEL=VDD -> SLOT1
DOUT comun   -> PG10 / SD_B
```

Diagnostico STM32:

```text
PG10_trans = 80258
PI7_trans  = 8801
raw_nz0    = 44100
raw_nz1    = 44100
pcm_nz0    = 44052
pcm_nz1    = 44064
```

Metricas:

| Canal | Microfono | RMS | dBFS | Peak |
|---|---|---:|---:|---:|
| SLOT0 | MIC5 | 0.012862 | -37.81 | 0.06412 |
| SLOT1 | MIC6 | 0.013210 | -37.58 | 0.06573 |

Comparacion entre slots:

```text
samples        = 44100
exact_equal    = 54
equal_pct      = 0.1224 %
correlation    = 0.7373
rms_difference = 0.009454
```

Resultado:

**PASS.** MIC5 y MIC6 funcionan correctamente en SD_B en ambas configuraciones SEL.

---

## 5.2 MIC2 + MIC4

### Sesion 1055

Configuracion:

```text
MIC2 SEL=VDD -> SLOT1
MIC4 SEL=GND -> SLOT0
DOUT comun   -> PG10 / SD_B
```

Diagnostico STM32:

```text
PG10_trans = 78367
PI7_trans  = 8801
raw_nz0    = 44100
raw_nz1    = 44099
pcm_nz0    = 44071
pcm_nz1    = 44023
```

Metricas:

| Canal | Microfono | RMS | dBFS | Peak |
|---|---|---:|---:|---:|
| SLOT0 | MIC4 | 0.013527 | -37.38 | 0.06339 |
| SLOT1 | MIC2 | 0.005707 | -44.87 | 0.02335 |

Comparacion entre slots:

```text
samples        = 44100
exact_equal    = 43
equal_pct      = 0.0975 %
correlation    = 0.5821
rms_difference = 0.011211
```

Resultado:

**PASS.** Ambos slots contienen audio util e independiente.

---

### Sesion 1056

Configuracion:

```text
MIC2 SEL=GND -> SLOT0
MIC4 SEL=VDD -> SLOT1
DOUT comun   -> PG10 / SD_B
```

Diagnostico STM32:

```text
PG10_trans = 78914
PI7_trans  = 8801
raw_nz0    = 44100
raw_nz1    = 44098
pcm_nz0    = 43982
pcm_nz1    = 44049
```

Metricas:

| Canal | Microfono | RMS | dBFS | Peak |
|---|---|---:|---:|---:|
| SLOT0 | MIC2 | 0.004895 | -46.20 | 0.02179 |
| SLOT1 | MIC4 | 0.010781 | -39.35 | 0.04300 |

Comparacion entre slots:

```text
samples        = 44100
exact_equal    = 63
equal_pct      = 0.1429 %
correlation    = 0.6183
rms_difference = 0.008657
```

Resultado:

**PASS.** La diferencia de nivel se desplaza de SLOT0 a SLOT1 junto con el microfono fisico al invertir SEL.

En esta fase MIC4 presenta aproximadamente entre 6.9 dB y 7.5 dB de diferencia respecto a MIC2. El nivel relativo concreto depende del montaje y de la fuente acustica, pero el comportamiento sigue al microfono fisico y no al slot.

---

## 5.3 MIC3 + MIC7 - pareja problematica

Esta pareja volvio a presentar problemas, reproduciendo el comportamiento irregular observado en fases anteriores.

### Prueba conjunta inicial

Se realizaron tres intentos con:

```text
MIC3 SEL=GND
MIC7 SEL=VDD
DOUT comun -> PG10 / SD_B
```

Resultado observado:

- un canal presentaba interferencia;
- el otro no entregaba audio util;
- repetir la prueba no normalizo el comportamiento.

Las conexiones SEL fueron revisadas y resoldadas nuevamente.

---

### MIC7 aislado

Se desconecto el DOUT de MIC3 y se dejo MIC7 trabajando de forma aislada sobre PG10.

Resultado:

**MIC7 funciono correctamente y produjo audio limpio.**

Esto demuestra que MIC7 puede trabajar de forma individual usando la misma infraestructura:

```text
PG10 -> SAI2B -> DMA1_Stream1 -> PCM -> WAV
```

---

### MIC3 reconectado junto a MIC7

Configuracion:

```text
MIC3 SEL=VDD
MIC7 SEL=GND
DOUT de ambos unidos -> PG10
```

Al reconectar DOUT de MIC3:

- MIC7 dejo de escucharse correctamente;
- aparecio audio deformado/interferencia;
- el dron podia distinguirse, pero la senal no era util.

El comportamiento es compatible con una perturbacion de la linea DOUT al incorporar MIC3, aunque esta observacion por si sola no identifica el mecanismo electrico interno exacto.

---

### MIC3 aislado - SEL=VDD

Se desconecto el DOUT de MIC7 y se dejo solamente MIC3 conectado a PG10.

```text
MIC3 SEL=VDD
MIC3 DOUT -> PG10
MIC7 DOUT desconectado
```

Resultado:

**MIC3 no entrego audio util.**

---

### MIC3 aislado - SEL=GND

Se mantuvo MIC3 aislado y se cambio unicamente SEL:

```text
MIC3 SEL=GND
MIC3 DOUT -> PG10
MIC7 DOUT desconectado
```

Resultado:

**MIC3 tampoco entrego audio util.**

Se revisaron continuidad, voltajes, conexiones y soldaduras varias veces sin encontrar un fallo externo evidente.

### Estado actual de MIC3

En el estado actual del hardware:

```text
MIC3 SEL=VDD -> no entrega audio util de forma aislada
MIC3 SEL=GND -> no entrega audio util de forma aislada
MIC3 en pareja con MIC7 -> perturba/corrompe la captura
```

El origen fisico exacto no se considera demostrado. Sin embargo, dado que:

- MIC7 funciona correctamente aislado;
- MIC5/MIC6 funcionan correctamente en PG10;
- MIC2/MIC4 funcionan correctamente en PG10;
- el problema reaparece especificamente al utilizar MIC3;
- continuidad, voltaje y soldaduras fueron revisados repetidamente;

MIC3 se considera **no fiable para continuar la integracion de cuatro microfonos en su estado actual** y debe quedar pendiente de reemplazo o inspeccion posterior.

No se atribuye este fallo a SAI2B o PG10.

---

# 6. Resumen de validacion de SD_B

| Par / prueba | Configuracion | Resultado |
|---|---|---|
| MIC5 + MIC6 | MIC5=VDD / MIC6=GND | PASS |
| MIC5 + MIC6 | MIC5=GND / MIC6=VDD | PASS |
| MIC2 + MIC4 | MIC2=VDD / MIC4=GND | PASS |
| MIC2 + MIC4 | MIC2=GND / MIC4=VDD | PASS |
| MIC7 aislado | DOUT -> PG10 | PASS |
| MIC3 + MIC7 | compartiendo DOUT | FAIL / comportamiento irregular |
| MIC3 aislado | SEL=VDD | FAIL / sin audio util |
| MIC3 aislado | SEL=GND | FAIL / sin audio util |

La sesion 1053 no se contabiliza como fallo de SD_B porque el comportamiento desaparecio despues de corregir el contacto fisico de SEL y la misma configuracion funciono correctamente en la sesion 1054.

---

# 7. Conclusion de Fase 3.1

La ruta secundaria de audio queda validada:

```text
PG10 / SAI2_SD_B
        -> SAI2 Block B
        -> SLAVE_RX sincronizado con SAI2A
        -> DMA1_Stream1
        -> SLOT0 + SLOT1
        -> PCM
        -> UART
        -> JSON / WAV
```

Los resultados de MIC5/MIC6 y MIC2/MIC4 muestran que:

1. PG10 recibe correctamente la linea DOUT compartida.
2. SAI2B separa correctamente SLOT0 y SLOT1.
3. `SEL=GND -> SLOT0` y `SEL=VDD -> SLOT1` se mantienen en SD_B.
4. Los dos slots contienen 44,100 muestras por segundo.
5. Los canales no son copias entre si.
6. La inversion de SEL mueve cada microfono al slot esperado.
7. SAI2A puede permanecer como maestro de BCLK/LRCL mientras SAI2B recibe el audio por PG10.

Por tanto, **Fase 3.1 - validacion aislada de SD_B / PG10: PASS**.

El problema observado con MIC3 se considera independiente de la validacion de SD_B y queda registrado como incidencia de hardware pendiente.

---

# 8. Siguiente fase

La siguiente etapa sera:

## Fase 3.2 - BasicSetUpMics2Pairs / 4 microfonos simultaneos

Arquitectura objetivo:

```text
PAIR A
2 mics -> DOUT compartido -> PI6 / SAI2_SD_A

PAIR B
2 mics -> DOUT compartido -> PG10 / SAI2_SD_B

Todos comparten:
BCLK -> PI5
WS   -> PI7
3V3
GND
```

Antes de realizar esta fase debe seleccionarse un conjunto de cuatro microfonos fisicamente fiables. MIC3 no debe utilizarse como elemento de referencia hasta resolver o sustituir su fallo actual.
