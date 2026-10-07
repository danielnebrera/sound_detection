# SoundDetection

Proyecto principal de detección de sonido para **Arduino Portenta H7 / STM32H747XI**, basado en la versión estable **v2.1.7**.

Esta rama fue creada a partir del estado estable histórico del proyecto y renombrada para mantener una estructura más clara y coherente con el repositorio `sound_detection`.

---

## 1. Objetivo

`SoundDetection` implementa la captura sincronizada de audio de **4 micrófonos digitales ICS-43434** usando los dos bloques de `SAI2` del STM32H747XI.

La arquitectura utiliza:

- `SAI2A` como receptor maestro.
- `SAI2B` como receptor esclavo sincronizado.
- Un único `BCLK` compartido.
- Un único `WS / LRCL` compartido.
- Dos líneas de datos:
  - `PI6 / SAI2_SD_A`
  - `PG10 / SAI2_SD_B`
- Dos micrófonos por línea DOUT.
- Separación de los dos micrófonos de cada pareja mediante `SEL`.
- Captura mediante DMA.
- Almacenamiento temporal en SDRAM.
- Transferencia de la sesión al PC mediante UART.
- Generación posterior de JSON/WAV mediante los scripts Python del proyecto.

---

## 2. Esquema de conexión

El diagrama utilizado por esta versión es:

![Esquema SoundDetection](<Schema version BasicSetUpMics2Pairs.png>)

> El nombre histórico del archivo del esquema se conserva, pero representa la arquitectura utilizada por `SoundDetection`.

---

## 3. Arquitectura de audio

```text
                           STM32H747XI
                        Arduino Portenta H7

                         SAI2A - MASTER_RX
                               |
                 +-------------+-------------+
                 |                           |
              PI5 BCLK                     PI7 WS/LRCL
                 |                           |
          +------+-----+               +-----+------+
          |            |               |            |
      Pareja A      Pareja B       clocks compartidos
          |            |
          |            +-----------------> PG10 / SAI2_SD_B
          +------------------------------> PI6  / SAI2_SD_A
```

Cada pareja comparte:

```text
BCLK
WS / LRCL
DOUT
3.3 V
GND
```

Los dos micrófonos de una misma línea DOUT utilizan valores opuestos de `SEL`.

La relación validada es:

```text
SEL = GND -> SLOT0
SEL = VDD -> SLOT1
```

---

## 4. Pines principales

| Señal | STM32H747XI | Función |
|---|---|---|
| BCLK / SCK | `PI5` | `SAI2_SCK_A` |
| WS / LRCL / FS | `PI7` | `SAI2_FS_A` |
| DOUT pareja A | `PI6` | `SAI2_SD_A` |
| DOUT pareja B | `PG10` | `SAI2_SD_B` |
| Alimentación mics | `3.3 V` | Alimentación ICS-43434 |
| Tierra | `GND` | Tierra común |

Importante:

```text
BCLK = SCK
WS = LRCL = FS
DOUT = SD
```

`BCLK` y `WS/LRCL` son señales diferentes y no deben intercambiarse.

---

## 5. Configuración SAI

### SAI2A

```text
Instance        = SAI2_Block_A
Mode            = MASTER_RX
Synchronization = ASYNCHRONOUS
Protocol        = I2S standard
Data size       = 24 bit
Slots           = 2
Sample rate     = 44.1 kHz
DMA             = DMA1_Stream0
DMA request     = SAI2_A
TriState        = SAI_OUTPUT_NOTRELEASED
```

`SAI2A` genera los clocks compartidos del sistema:

```text
BCLK  -> PI5
WS    -> PI7
```

y recibe la primera pareja de micrófonos por:

```text
PI6 / SAI2_SD_A
```

### SAI2B

```text
Instance        = SAI2_Block_B
Mode            = SLAVE_RX
Synchronization = SYNCHRONOUS
Protocol        = I2S standard
Data size       = 24 bit
Slots           = 2
DMA             = DMA1_Stream1
DMA request     = SAI2_B
TriState        = SAI_OUTPUT_RELEASED
```

`SAI2B` utiliza los clocks generados por `SAI2A` y recibe la segunda pareja por:

```text
PG10 / SAI2_SD_B
```

Durante el arranque de la captura se inicia primero `SAI2B` y posteriormente `SAI2A`.

---

## 6. Frecuencia y formato de audio

La configuración de captura utilizada es:

```text
Sample rate : 44,100 Hz
Resolution  : 24 bit I2S
Slots       : 2 por bloque SAI
Micrófonos  : 4
```

Cada segundo de audio contiene:

```text
44,100 muestras por micrófono
```

La señal recibida en 24 bits se convierte posteriormente al formato PCM utilizado por el pipeline del proyecto.

---

## 7. SDRAM

La Portenta H7 utiliza la SDRAM externa como memoria principal para las sesiones de audio.

Configuración validada:

```text
Capacidad : 8 MiB
Base      : 0x60000000
```

La SDRAM fue validada mediante pruebas de inicialización, prueba rápida y prueba completa antes de incorporarla al flujo de captura.

La arquitectura evita depender de UART durante la fase crítica de adquisición. El audio se captura primero y se transmite posteriormente.

---

## 8. Flujo general

```text
ICS-43434
    |
    v
SAI2A + SAI2B
    |
    v
DMA
    |
    v
SDRAM
    |
    v
procesamiento / detección
    |
    v
UART 1,000,000 baudios
    |
    v
grabar_sesion.py
    |
    +--> JSON
    +--> metadata
    +--> WAV / procesamiento posterior
```

---

## 9. Transferencia UART

La versión v2.1.7 utiliza una transferencia robusta por UART.

Características:

```text
Baudrate        = 1,000,000
Bloque inicial  = 8192 bytes
Fallback        = 4096 bytes
Fallback final  = 1024 bytes
Integridad      = CRC32
Confirmación    = ACK / NACK
```

El receptor de PC valida cada bloque antes de continuar.

---

## 10. Script principal de recepción

El receptor correspondiente a esta versión es:

```text
Python/grabar_sesion.py
```

Ejemplo:

```powershell
python grabar_sesion.py --port COM6 --baud 1000000 --session 1078 --start-order 1
```

En esta versión se utiliza:

```text
--start-order
```

y no `--order`.

---

## 11. Organización de canales

La arquitectura entrega cuatro canales de audio procedentes de los dos bloques SAI:

```text
SAI2A SLOT0
SAI2A SLOT1
SAI2B SLOT0
SAI2B SLOT1
```

En la configuración histórica de la versión v2.1.7 utilizada con Khamex, el mapeo lógico fue:

| Canal | Micrófono físico | Posición |
|---|---|---|
| CH0 | Mic2 | left |
| CH1 | Mic4 | top |
| CH2 | Mic3 | back |
| CH3 | Mic1 | right |

En metadata/UI:

```text
Mic1 = right
Mic2 = left
Mic3 = back
Mic4 = top
```

Si se reemplazan físicamente los micrófonos, debe mantenerse documentada la nueva correspondencia de canal y posición.

---

## 12. Estructura principal del proyecto

```text
SoundDetection/
|
|-- CM7/
|   |-- Core/
|   |-- Middlewares/
|   |-- X-CUBE-AI/
|   `-- ...
|
|-- CM4/
|   `-- ...
|
|-- Common/
|-- Drivers/
|-- Python/
|
|-- SoundDetection.ioc
|-- README_SoundDetection.md
|-- Schema version BasicSetUpMics2Pairs.png
`-- ...
```

Los nombres de proyecto utilizados en STM32CubeIDE son:

```text
SoundDetection
SoundDetection_CM7
SoundDetection_CM4
```

---

## 13. TensorFlow Lite Micro / X-CUBE-AI

El proyecto incluye la infraestructura de TensorFlow Lite Micro utilizada por el pipeline de detección.

El subproyecto `SoundDetection_CM7` contiene las rutas necesarias hacia:

```text
CM7/Middlewares/tensorflow
CM7/Middlewares/tensorflow/tensorflow/lite
CM7/Middlewares/tensorflow/tensorflow/lite/micro
CM7/X-CUBE-AI/App
```

Durante la creación del nuevo worktree `SoundDetection`, las rutas absolutas históricas de TensorFlow fueron actualizadas para apuntar a:

```text
C:/Users/jaene/Desktop/ModeloLocalizacion/SoundDetection/...
```

---

## 14. Correcciones mínimas realizadas al reconstruir v2.1.7

La rama `SoundDetection` parte del commit estable:

```text
26ba5c6 - Daniela 2.1.7 'estable'
```

Al recompilar desde cero se detectaron dos declaraciones faltantes en el snapshot histórico del código fuente.

En `CM7/Core/Inc/sai.h` se añadieron:

```c
extern DMA_HandleTypeDef hdma_sai2_a;
extern DMA_HandleTypeDef hdma_sai2_b;
```

En `CM7/Core/Src/main.c` se añadió el prototipo:

```c
static void uart_send(const char *text);
```

Estas modificaciones no alteran la lógica de captura, configuración SAI, DMA, audio ni protocolo UART. Únicamente permiten recompilar correctamente el código fuente almacenado en el commit.

---

## 15. Validación del montaje

Durante la validación más reciente se comprobó nuevamente la versión v2.1.7 y la arquitectura experimental `BasicSetUpMics2Pairs`.

Inicialmente ambas presentaron ausencia de audio debido a un problema físico de cableado.

Se identificaron dos incidencias en los empalmes:

```text
- un empalme no estaba estañado correctamente;
- BCLK se había soltado físicamente del empalme.
```

Una vez corregidas estas conexiones:

```text
v2.1.7              -> PASS
BasicSetUpMics2Pairs -> PASS
```

Por lo tanto, ambas arquitecturas quedaron funcionalmente validadas con el cableado corregido.

---

## 16. Recomendaciones de cableado

Antes de diagnosticar firmware se recomienda comprobar:

```text
1. Continuidad BCLK desde PI5 hasta todos los micrófonos.
2. Continuidad WS/LRCL desde PI7.
3. Continuidad DOUT pareja A -> PI6.
4. Continuidad DOUT pareja B -> PG10.
5. 3.3 V en todos los micrófonos.
6. GND común.
7. SEL=GND o SEL=VDD según el slot esperado.
8. Soldadura correcta de todos los empalmes.
```

Si BCLK o WS se pierde, los ICS-43434 no entregarán una trama I2S válida aunque alimentación, SEL y DOUT tengan continuidad estática.

---

## 17. Estado del proyecto

```text
Proyecto             : SoundDetection
Base                  : v2.1.7 estable
MCU                   : STM32H747XI
Board                 : Arduino Portenta H7
Core principal        : Cortex-M7
Micrófonos            : 4 x ICS-43434
Sample rate           : 44.1 kHz
SAI                   : SAI2A + SAI2B
DMA                   : habilitado
SDRAM                 : 8 MiB
UART                  : 1,000,000 baudios
Transferencia         : CRC32 + ACK/NACK
Captura 4 canales     : validada
Build desde cero      : validado
```

---

## 18. Optimización de rendimiento TFLite

Se añadió un perfilador `P1` para medir por separado el tiempo de:

```text
preprocesamiento
MFCC
TFLite
```

El perfilado permitió identificar a TFLite como el principal cuello de botella inicial.

La configuración de compilación validada actualmente es:

```text
C   = -O3
C++ = -O2
```

Durante esta optimización se mantuvieron sin cambios:

```text
modelo
MFCC matemáticamente
gate
thresholds
lógica del profiler P1
```

También se probó `C++ = -O3`, pero se descartó porque el firmware excedía la región FLASH disponible.

Con `C++ = -O2`, la sesión 1086 confirmó una mejora importante:

```text
TFLite antes (-Os) : ~408.8 ms por inferencia
TFLite ahora (-O2) : ~122.9 ms por inferencia
Mejora              : ~3.3x más rápido
```

El tiempo máximo de detección de una ventana completa de 4 canales pasó aproximadamente de:

```text
~2.23 s -> ~1.08 s
```

Posteriormente se realizó la prueba P3, moviendo únicamente los buffers temporales de FFT desde `RAM_D2` hacia `DTCM`, sin modificar la matemática del MFCC ni la lógica del detector.

La sesión 1088 validó el resultado:

```text
MFCC antes (RAM_D2) : ~120.7 ms
MFCC ahora (DTCM)   : ~42.9 ms
Mejora              : ~2.8x más rápido

Ventana 4 canales:
~1.085 s -> ~0.769 s
```

TFLite se mantuvo prácticamente igual (~122.9 ms), confirmando que la mejora provino del cambio de ubicación de los buffers FFT.

La detección, calibración, gate, probabilidades, EMA y alertas continuaron funcionando correctamente.

---

## 19. Próximos pasos

- [x] Implementar perfilado P1 para separar preprocesamiento, MFCC y TFLite.
- [x] Identificar TFLite como principal cuello de botella.
- [x] Probar `C++ = -O3` y descartar por overflow de FLASH.
- [x] Validar `C++ = -O2` manteniendo `C = -O3`.
- [x] Añadir telemetría P1-GATE para validar calibración y activación del gate.
- [x] Validar detección completa con 4 canales activos en la sesión 1086.
- [x] Mover los buffers temporales de FFT desde `RAM_D2` hacia `DTCM`.
- [x] Medir nuevamente MFCC y el tiempo total de detección después del cambio.
- [x] Mantener modelo, gate, thresholds y lógica matemática sin cambios durante la prueba P3.
- [ ] Identificar el siguiente cuello de botella manteniendo una sola variable por prueba.

---

## 20. Conclusión

`SoundDetection` constituye la base limpia del proyecto principal de captura y detección de audio sobre STM32H747XI.

La arquitectura de cuatro micrófonos queda formada por:

```text
2 micrófonos -> PI6  / SAI2_SD_A
2 micrófonos -> PG10 / SAI2_SD_B

BCLK compartido -> PI5
WS compartido   -> PI7
```

con:

```text
SAI2A = MASTER_RX / ASYNCHRONOUS
SAI2B = SLAVE_RX  / SYNCHRONOUS
```

La versión v2.1.7 fue nuevamente validada después de corregir el cableado físico y actualmente incorpora el perfilado de rendimiento y la optimización `C++ = -O2` como base para las siguientes etapas del proyecto.

---

## 21. Optimización avanzada TFLite: P4-B, P4-D y P4-E1

Después de la optimización de MFCC en P3, TFLite volvió a ser el principal cuello de botella del pipeline. A partir de este punto se mantuvo la metodología de cambiar una sola variable por prueba.

### P4-B - Resolver mínimo de operadores

Se reemplazó el resolver general por un `MicroMutableOpResolver<8>` con únicamente los operadores realmente utilizados por el modelo:

```text
Conv2D
MaxPool2D
Shape
StridedSlice
Pack
Reshape
FullyConnected
Logistic
```

La sesión 1089 validó el funcionamiento completo del modelo con los 8 operadores. El tiempo de inferencia se mantuvo prácticamente igual, por lo que P4-B se considera una optimización de footprint, no de velocidad.

Resultado principal:

```text
text antes de P4-B : 1,210,896 bytes
text con P4-B       :   897,440 bytes
reducción           :   313,456 bytes (~306 KiB)
```

La detección completa siguió funcionando con:

```text
queue_high = 1
errors     = 0
```

### P4-C - `conv.cc` selectivo en `-O3`

Se probó mantener `C++ = -O2` global y compilar únicamente `cmsis_nn/conv.cc` con `-O3`.

La sesión 1090 mostró que el tiempo TFLite permaneció aproximadamente en:

```text
~122.8 ms
```

Por lo tanto, P4-C fue descartado como optimización de rendimiento. El cambio de nivel de optimización del translation unit por sí solo no produjo una mejora medible.

### P4-D - Perfilado interno por nodo TFLite

Se activó temporalmente el observer/profiler interno de TFLite Micro para medir cada nodo sin imprimir por UART durante la inferencia. Las métricas se acumularon en RAM y se emitieron únicamente al terminar la captura.

La sesión 1091 midió 64 inferencias y 11 nodos:

```text
Nodo 0  CONV_2D          avg ~34.045 ms
Nodo 1  MAX_POOL_2D      avg ~ 2.756 ms
Nodo 2  CONV_2D          avg ~81.791 ms
Nodo 3  MAX_POOL_2D      avg ~ 0.842 ms
Nodo 4  SHAPE            avg ~ 0.000 ms
Nodo 5  STRIDED_SLICE    avg ~ 0.004 ms
Nodo 6  PACK             avg ~ 0.001 ms
Nodo 7  RESHAPE          avg ~ 0.050 ms
Nodo 8  FULLY_CONNECTED  avg ~ 3.026 ms
Nodo 9  FULLY_CONNECTED  avg ~ 0.003 ms
Nodo 10 LOGISTIC         avg ~ 0.001 ms
```

Los dos `Conv2D` consumían juntos aproximadamente:

```text
115.836 ms
```

sobre unos:

```text
122.5 ms
```

medidos dentro de TFLite, es decir, aproximadamente el 94.5 % del tiempo total de inferencia.

Este resultado confirmó que la siguiente optimización debía centrarse directamente en la implementación float de `Conv2D`.

### P4-E1 - Optimización del `reference_ops::Conv()` float

El camino `float32` utilizado por el modelo ejecuta la convolución de referencia de TensorFlow Lite Micro. La implementación original recalculaba offsets multidimensionales para cada multiplicación dentro del bucle más interno.

P4-E1 mantuvo sin cambios:

```text
modelo
pesos
orden de acumulación
stride
padding
dilation
activación
MFCC
gate
thresholds
```

El cambio consistió únicamente en sustituir la indexación repetitiva mediante `Offset()` por aritmética directa de punteros y strides precalculados, manteniendo el mismo orden de cálculo:

```text
filter_y
  -> filter_x
     -> in_channel
```

La sesión 1092 validó una mejora importante:

```text
Conv2D #0 promedio : 34.045 ms -> 23.033 ms
Conv2D #2 promedio : 81.791 ms -> 37.702 ms
```

Resultado conjunto de ambos Conv2D:

```text
115.836 ms -> 60.735 ms
```

lo que representa aproximadamente una reducción del 47.6 % en el tiempo combinado de convolución.

El tiempo máximo de TFLite por canal pasó aproximadamente de:

```text
122.8 ms -> 67.6 ms
```

El tiempo máximo de detección de una ventana completa de 4 canales pasó de:

```text
767.7 ms -> 546.6 ms
```

manteniendo:

```text
queue_high = 1
errors     = 0
pair_skew  = 1
64 JSON de audio
16 órdenes completas
4 canales activos
```

Comparado con la referencia anterior a estas optimizaciones:

```text
sesión 1081 : detect_us_max ~2.227 s
sesión 1092 : detect_us_max ~0.547 s
```

La reducción acumulada de latencia máxima es aproximadamente del 75.5 %, equivalente a unas 4.1 veces más velocidad que la referencia inicial.

P4-E1 queda validado como parte de la nueva base del proyecto.

### Base validada actual

```text
C                  = -O3
C++                = -O2
FFT temporal       = DTCM
TFLM resolver      = MicroMutableOpResolver<8>
Conv2D float       = reference_ops::Conv() optimizado con punteros/strides
queue_high         = 1
errors             = 0
caches             = sin cambios / no habilitadas
```

---

## 22. Próximo paso: geometría de los dos Conv2D

Antes de especializar más la implementación de convolución en P4-E2, se medirá y documentará la geometría exacta de ambos nodos `Conv2D`:

```text
input H x W x C
filter H x W x C
output H x W x C
stride
padding
dilation
groups
```

El objetivo es determinar si ambos nodos pueden beneficiarse de una ruta especializada sin cambiar la matemática del modelo.

La metodología se mantiene:

```text
una sola variable por prueba
medición antes de optimizar
sin cambios en modelo ni lógica de detección
sin UART durante la fase crítica de captura
```
