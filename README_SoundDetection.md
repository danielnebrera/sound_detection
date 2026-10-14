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

## 19. Estado de optimización

La campaña de rendimiento mantiene la metodología de cambiar una sola variable por prueba y conservar como referencia la última versión validada.

Estado actual:

- [x] Perfilado P1 de preprocesamiento, MFCC y TFLite.
- [x] `C = -O3`.
- [x] `C++ = -O2`.
- [x] `C++ = -O3` global descartado por overflow de FLASH.
- [x] FFT temporal movida desde `RAM_D2` hacia `DTCM` en P3.
- [x] Resolver TFLM mínimo de 8 operadores.
- [x] Perfilado interno por nodo TFLite mediante P4-D.
- [x] Optimización P4-E1 de punteros/strides.
- [x] Especialización geométrica 3x3 en P4-E2.
- [x] Desenrollado explícito de profundidad 16 en P4-E3.
- [x] P4-E4 evaluado y descartado.
- [x] Corrección MPU de FLASH validada como CACHE-0.
- [x] I-Cache evaluada sin beneficio medible.
- [x] D-Cache evaluada y descartada por regresión reproducible.
- [x] P4-E5 validado con 64 inferencias.
- [x] P4-D retirado de la versión de producción.
- [ ] P5: optimización de MFCC.

---

## 20. Arquitectura validada actual

`SoundDetection` continúa siendo la base limpia de captura y detección de audio sobre STM32H747XI.

Arquitectura de audio:

```text
2 micrófonos -> PI6  / SAI2_SD_A
2 micrófonos -> PG10 / SAI2_SD_B

BCLK compartido -> PI5
WS compartido   -> PI7
```

Configuración SAI:

```text
SAI2A = MASTER_RX / ASYNCHRONOUS
SAI2B = SLAVE_RX  / SYNCHRONOUS
```

Captura:

```text
sample rate       = 44.1 kHz
canales           = 4
DMA               = habilitado
SDRAM             = 8 MiB
UART              = 1,000,000 baudios
transferencia     = CRC32 + ACK/NACK
queue_high        = 1
errors            = 0
```

La captura DMA utiliza buffers ping-pong en D2 y el firmware copia posteriormente los datos al ring de SDRAM. La SDRAM no se utiliza como destino directo del DMA.

---

## 21. Optimización avanzada TFLite: P4

Después de P3, TFLite volvió a ser el principal cuello de botella. La campaña P4 se centró en reducir el coste de inferencia sin cambiar:

```text
modelo
pesos
matemática del detector
MFCC
gate
thresholds
orden de acumulación de las convoluciones aceptadas
```

### P4-B - Resolver mínimo

El modelo utiliza exactamente:

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

Se validó `MicroMutableOpResolver<8>`.

Resultado principal:

```text
text antes de P4-B : 1,210,896 bytes
text con P4-B       :   897,440 bytes
reducción           :   313,456 bytes
```

P4-B se conserva principalmente como optimización de footprint.

### P4-C - `conv.cc` selectivo en `-O3`

Se evaluó `conv.cc` en `-O3` manteniendo `C++ = -O2` global.

No produjo una mejora medible y fue descartado.

### P4-D - Perfilado interno por nodo

P4-D añadió temporalmente el observer de TFLite Micro para acumular tiempos por nodo en RAM y emitirlos sólo al final de la captura.

La sesión 1091 mostró que los dos `Conv2D` representaban aproximadamente el 94.5 % del tiempo TFLite:

```text
Conv2D #0 avg : 34.045 ms
Conv2D #2 avg : 81.791 ms
```

Esto justificó concentrar P4-E en el kernel float de convolución.

P4-D se mantiene como herramienta de desarrollo y se elimina en producción.

---

## 22. P4-E1 - Punteros y strides directos

P4-E1 sustituyó indexación multidimensional repetitiva por aritmética directa de punteros y strides, manteniendo el orden de cálculo.

Sesión 1092:

```text
Conv2D #0 : 34.045 ms -> 23.033 ms
Conv2D #2 : 81.791 ms -> 37.702 ms
```

Tiempo combinado:

```text
115.836 ms -> 60.735 ms
```

P4-E1 fue aceptado.

---

## 23. P4-E2 y P4-E3 - Especialización geométrica

Se midió la geometría exacta de los dos `Conv2D`.

Conv0:

```text
input  = [1,100,20,1]
filter = [16,3,3,1]
output = [1,98,18,16]
stride = 1
padding = VALID
dilation = 1
groups = 1
```

Conv2:

```text
input  = [1,49,9,16]
filter = [32,3,3,16]
output = [1,47,7,32]
stride = 1
padding = VALID
dilation = 1
groups = 1
```

### P4-E2

Se añadió un fast-path para convolución `3x3`, `stride=1`, `VALID`, `dilation=1`, `groups=1`.

Sesión 1095:

```text
Conv0 avg = 13.225 ms
Conv2 avg = 33.936 ms
TFLite max ~53.978 ms
detect_us_max ~492.656 ms
```

P4-E2 fue aceptado.

### P4-E3

Para `filter_input_depth == 16`, se sustituyó el loop interior por 16 MAC explícitos manteniendo el mismo orden de acumulación.

Sesión 1096:

```text
Conv0 avg       = 13.561 ms
Conv2 avg       = 21.698 ms
TFLite max      ~42.089 ms
detect_us_max   = 444.192 ms
P1_WINDOW max   = 444.191 ms
queue_high      = 1
errors          = 0
```

Respecto a la sesión 1081:

```text
2,227,136 us -> 444,192 us
```

P4-E3 fue aceptado y pasó a ser la nueva base.

---

## 24. CACHE-0, I-Cache y D-Cache

Durante la revisión de memoria se detectó que la región MPU de FLASH no debía mantenerse como una región de 1 MiB con base `0x08040000`.

La configuración validada CACHE-0 utiliza:

```text
FLASH MPU base = 0x08000000
FLASH MPU size = 2 MiB
I-Cache        = OFF
D-Cache        = OFF
```

### CACHE-0

Sesión 1099:

```text
Conv0 avg      = 13.541 ms
Conv2 avg      = 20.653 ms
detect_us_max  = 439.528 ms
queue_high     = 1
errors         = 0
```

CACHE-0 fue aceptado.

### I-Cache

La sesión 1100 mostró diferencias dentro del ruido experimental.

Conclusión:

```text
I-Cache funcional
beneficio medible: no
```

### D-Cache

La sesión 1102 mostró una regresión reproducible, especialmente en Conv2:

```text
Conv2 avg:
20.791 ms -> 21.709 ms
```

aproximadamente:

```text
+4.42 %
```

Por ello la configuración de producción mantiene:

```text
I-Cache = OFF
D-Cache = OFF
```

---

## 25. P4-E4 - Prueba descartada

P4-E4 desenrolló más agresivamente la estructura `3x3`.

Sesión 1098:

```text
Conv0 avg = 15.289 ms
Conv2 avg = 19.109 ms
```

Aunque Conv2 mejoró, Conv0 sufrió una regresión importante y aumentó el tamaño de código.

P4-E4 fue descartado.

La base regresó a P4-E3 antes de continuar.

---

## 26. P4-E5 - Especialización exclusiva de Conv2

P4-E5 mantiene P4-E3 intacto como fallback y añade una ruta especializada sólo para:

```text
input  = [1,49,9,16]
filter = [32,3,3,16]
output = [1,47,7,32]
```

Objetivo:

```text
calcular una sola vez los 9 punteros del patch 3x3 por output pixel
reutilizarlos para los 32 canales de salida
mantener exactamente el orden de acumulación
no usar fast-math
no usar acumuladores paralelos
no cambiar precisión
```

La ruta especializada se mantuvo aislada para no penalizar Conv0.

### Validación con P4-D ON

Sesión 1109, con los cuatro canales activos y 64 inferencias:

```text
Conv0 avg = 13.202 ms
Conv2 avg = 19.028 ms
invokes   = 64
```

Comparación contra P4-E3 / sesión 1099:

```text
Conv0 : 13.541 ms -> 13.202 ms   (-2.50 %)
Conv2 : 20.653 ms -> 19.028 ms   (-7.87 %)
```

El tiempo combinado de ambos Conv2D pasó de:

```text
34.194 ms -> 32.230 ms
```

P4-E5 fue aceptado.

### Footprint con P4-D

```text
P4-E3 + P4-D:
text = 898472
data = 528
bss  = 444080

P4-E5 + P4-D:
text = 900400
data = 528
bss  = 444080
```

Coste de P4-E5:

```text
+1928 bytes de text
data sin cambio
bss sin cambio
```

---

## 27. Producción limpia P4-E5

Después de validar P4-E5 se retiró únicamente P4-D.

P1 permanece activo para poder medir:

```text
preprocess
MFCC
TFLite total
ventana completa
gate/calibración
```

La sesión 1110 validó la versión de producción:

```text
active              = 0x0F
active_count        = 16 en CH0..CH3
completed           = 19
detected            = 19
record_chunks       = 16
queue_high          = 1
copy_us_max         = 172 us
pairs               = 1638
pair_skew           = 1
errors              = 0
JSON de audio       = 64
estado              = OK
```

Build de producción:

```text
text = 899464
data = 512
bss  = 443656
```

Comparación limpia:

```text
sesión 1103 = P4-E3 + CACHE-0 + P4-D OFF
sesión 1110 = P4-E5 + CACHE-0 + P4-D OFF
```

TFLite máximo por canal:

```text
             P4-E3       P4-E5
CH0          40.659 ms    38.922 ms
CH1          40.481 ms    38.737 ms
CH2          40.685 ms    38.732 ms
CH3          40.479 ms    38.919 ms
```

Promedio de máximos:

```text
40.576 ms -> 38.828 ms
mejora = 4.31 %
```

Ventana completa:

```text
detect_us_max:
439.372 ms -> 431.347 ms

P1_WINDOW max:
439.371 ms -> 431.346 ms
```

Mejora de la ventana completa:

```text
~1.83 %
```

Respecto a la referencia original de la sesión 1081:

```text
2,227,136 us -> 431,347 us
reducción    = 80.63 %
speedup      = 5.16x
```

---

## 28. Incidente físico SEL durante la validación

Durante las sesiones 1104-1108 aparecieron temporalmente sólo dos canales activos:

```text
active = 0x06
```

CH0 y CH3 mostraban niveles post-DSP cercanos a `0 dBFS`, mientras CH1 y CH2 permanecían alrededor de `-40 dBFS`.

Se comprobó que no era una regresión de:

```text
P4-E5
P4-D
CACHE-0
MFCC
TFLite
```

La causa fue física: los pines `SEL` no estaban haciendo buen contacto con el adaptador de voltaje.

Los `SEL` se mantienen sin soldar de forma deliberada para facilitar pruebas de intercambio entre `GND` y `VCC`.

Después de corregir el contacto, la captura regresó inmediatamente a:

```text
active = 0x0F
active_count = 16 en los cuatro canales
```

Este incidente no se utiliza como benchmark de rendimiento.

---

## 29. Base de producción actual

La base estable después de P4 queda definida como:

```text
Firmware             = v2.1.7 estable
MCU                  = STM32H747XI / Cortex-M7
Sample rate          = 44.1 kHz
Canales              = 4
Captura              = SAI2 + DMA + SDRAM
C                    = -O3
C++                  = -O2
FFT temporal         = DTCM
TFLM resolver        = MicroMutableOpResolver<8>
Conv2D               = P4-E5
MPU FLASH            = 0x08000000 / 2 MiB
I-Cache              = OFF
D-Cache              = OFF
P4-D                 = OFF en producción
P1                   = ON
queue_high           = 1
errors               = 0
```

Build de referencia:

```text
text = 899464
data = 512
bss  = 443656
```

Benchmark de referencia:

```text
sesión              = 1110
detect_us_max       = 431347 us
P1_WINDOW max       = 431346 us
TFLite max promedio = 38.828 ms
```

Esta versión debe conservarse como checkpoint antes de comenzar P5.

---

## 30. Próxima campaña: P5 - MFCC

Después de P4-E5, TFLite dejó de ser el mayor bloque individual del pipeline.

En la sesión 1110:

```text
Preprocess max ~26-27 ms
MFCC max       ~42.5 ms
TFLite max     ~38.9 ms
```

El nuevo objetivo de P5 será reducir el coste de MFCC sin alterar su salida matemática.

Reglas para P5:

```text
una variable por experimento
P4-E5 permanece congelado
CACHE-0 permanece congelado
I-Cache y D-Cache permanecen OFF
C = -O3
C++ = -O2
sin cambios de modelo
sin cambios de gate/thresholds
sin UART durante la fase crítica
comparar siempre contra la sesión 1110
```

Primer objetivo de P5:

```text
descomponer MFCC internamente
identificar qué etapa domina los ~42 ms
optimizar únicamente la etapa medida como cuello de botella
```

La versión de producción P4-E5 queda como punto de retorno seguro durante toda la campaña P5.

---

## 31. P5-A - Perfilado interno del MFCC

La campaña P5 comenzó instrumentando internamente el cálculo MFCC sin alterar su matemática.

Se midieron por separado las etapas:

```text
WINDOW
FFT
POWER
MEL
LOG
DCT
```

Las métricas se acumulan durante la captura y se emiten únicamente al finalizar, evitando UART dentro de la fase crítica.

La sesión 1111 mostró:

```text
WINDOW avg = 56 us/frame
FFT avg    = 241 us/frame
POWER avg  = 26 us/frame
MEL avg    = 45 us/frame
LOG avg    = 27 us/frame
DCT avg    = 25 us/frame

MFCC compute avg = 42.432 ms
MFCC compute max = 42.554 ms
```

La FFT representó aproximadamente el 56.8 % del tiempo interno medido por frame y quedó identificada como el principal cuello de botella dentro del MFCC.

Build P5-A:

```text
text = 900424
data = 512
bss  = 443832
```

P5-A se mantiene como profiler de diagnóstico durante las pruebas P5.

---

## 32. P5-B - Perfilado interno de RFFT/CFFT

Se añadió un segundo profiler para descomponer la FFT de CMSIS-DSP sin modificar su matemática.

La ruta utilizada por `N_FFT = 2048` es:

```text
RFFT 2048
  -> CFFT interna 1024
  -> radix-8 + radix-2
  -> bit reversal
  -> etapa RFFT
```

Se midieron:

```text
RFFT_TOTAL
CFFT_TOTAL
RFFT_STAGE
CFFT_KERNEL
BITREV
RADIX2_PREP
RADIX8_COL1
RADIX8_COL2
```

La sesión 1112 obtuvo:

```text
RFFT_TOTAL   = 242 us/frame
CFFT_TOTAL   = 182 us/frame
RFFT_STAGE   =  60 us/frame
CFFT_KERNEL  = 156 us/frame
BITREV       =  25 us/frame
RADIX2_PREP  =  22 us/frame
RADIX8_COL1  =  68 us/frame
RADIX8_COL2  =  65 us/frame
```

Los dos bloques radix-8 consumían conjuntamente:

```text
68 + 65 = 133 us/frame
```

equivalentes aproximadamente al:

```text
85.3 % del CFFT kernel
55.0 % del RFFT total
```

Esto identificó el radix-8 como el principal objetivo interno de la FFT.

Build P5-B:

```text
text = 901208
data = 512
bss  = 444024
```

P5-B se mantiene activo como profiler durante las siguientes pruebas.

---

## 33. P5-C - Twiddle CFFT desde FLASH hacia DTCM

P5-C movió únicamente la tabla de twiddles utilizada por la CFFT interna de 1024 puntos desde FLASH hacia DTCM.

Se añadió:

```text
2048 float32 = 8192 bytes
```

en `.DTCM_dsp`.

No se modificó:

```text
algoritmo FFT
orden matemático
twiddle values
N_FFT
MFCC
TFLite
gate
caches
compiler flags
```

Build:

```text
P5-B:
text = 901208
data = 512
bss  = 444024

P5-C:
text = 901232
data = 512
bss  = 452216
```

El incremento de `bss` fue exactamente:

```text
+8192 bytes
```

La sesión 1113 mostró:

```text
MFCC avg      42.598 ms -> 41.738 ms
FFT avg          242 us ->    235 us
CFFT total       182 us ->    175 us
CFFT kernel      156 us ->    150 us
RADIX2_PREP       22 us ->     20 us
RADIX8_COL1       68 us ->     64 us
RADIX8_COL2       65 us ->     64 us
detect_us_max 434360 us -> 431771 us
```

La mejora fue coherente con una reducción del coste de acceso a los twiddles de la CFFT.

Decisión:

```text
P5-C = ACEPTADO
```

---

## 34. P5-D - Twiddle RFFT desde FLASH hacia DTCM

P5-D mantuvo P5-C y movió además la tabla de twiddles de la etapa RFFT 2048 desde FLASH hacia DTCM.

Coste adicional:

```text
2048 float32 = 8192 bytes
```

Build:

```text
P5-C:
text = 901232
data = 512
bss  = 452216

P5-D:
text = 901248
data = 512
bss  = 460408
```

El incremento de `bss` fue nuevamente exactamente:

```text
+8192 bytes
```

La sesión 1114 mostró una firma causal muy clara:

```text
                       P5-C       P5-D
MFCC avg              41.738 ms   40.572 ms
FFT / RFFT_TOTAL         235 us      227 us
CFFT_TOTAL               175 us      174 us
RFFT_STAGE                59 us       51 us
CFFT_KERNEL              150 us      150 us
BITREV                     24 us       24 us
RADIX2_PREP                20 us       20 us
RADIX8_COL1                64 us       64 us
RADIX8_COL2                64 us       64 us
detect_us_max          431771 us   426016 us
```

La etapa directamente afectada fue:

```text
RFFT_STAGE:
59 us -> 51 us
reducción ~13.6 %
```

mientras el núcleo CFFT permaneció estable.

Decisión:

```text
P5-D = ACEPTADO
```

P5-D quedó como checkpoint para los experimentos posteriores del radix-8.

---

## 35. P5-E - Radix-8 ejecutado desde ITCM

P5-E probó si el fetch de instrucciones desde FLASH era un cuello de botella para `arm_radix8_butterfly_f32()`.

Únicamente esa función se colocó en ITCM.

El `.map` confirmó:

```text
.ITCM_text VMA       = 0x00000000
arm_radix8_butterfly = 0x00000000
función size         = 0x51C = 1308 bytes
load address FLASH   = 0x0811BBB8
```

Build:

```text
text = 901328
data = 512
bss  = 460408
```

La sesión 1115 mostró:

```text
                       P5-D       P5-E
RADIX8_COL1              64 us       64 us
RADIX8_COL2              64 us       64 us
CFFT_KERNEL             150 us      150 us
CFFT_TOTAL              174 us      175 us
RFFT_TOTAL              227 us      224 us
MFCC avg             40.572 ms   40.336 ms
detect_us_max         426016 us   425362 us
```

Las dos etapas radix-8 no mejoraron.

Por lo tanto, la pequeña variación global no se atribuye al cambio de ubicación del código.

Conclusión:

```text
instruction fetch desde FLASH no es un cuello de botella significativo
para el radix-8 en esta configuración
```

Decisión:

```text
P5-E = RECHAZADO
```

---

## 36. P5-F - Fast-path 512/modifier=2 con parámetros constantes

P5-F creó un fast-path específico para la geometría utilizada por la CFFT interna:

```text
fftLen            = 512
twidCoefModifier  = 2
```

La implementación CMSIS original se mantuvo como fallback.

No se modificó:

```text
orden matemático
twiddles
sumas/restas
multiplicaciones
C81
caches
compiler flags
```

Build:

```text
text = 902600
data = 512
bss  = 460408
```

La sesión 1116 obtuvo:

```text
                       P5-D       P5-F
RADIX8_COL1              64 us       64 us
RADIX8_COL2              64 us       64 us
RADIX2_PREP               20 us       20 us
CFFT_KERNEL              150 us      149 us
CFFT_TOTAL               174 us      174 us
RFFT_TOTAL               227 us      224 us
MFCC avg             40.572 ms   40.294 ms
detect_us_max         426016 us   426080 us
```

La especialización de parámetros por sí sola no produjo una reducción medible del radix-8.

Decisión:

```text
P5-F = RECHAZADO
```

---

## 37. P5-G - Especialización estructural del radix-8

P5-G mantuvo como base P5-D y especializó estructuralmente la ruta exacta utilizada por el proyecto:

```text
RFFT 2048
  -> CFFT 1024
  -> radix8by2
  -> dos columnas de 512 puntos
  -> twidCoefModifier = 2
```

Se conserva la función CMSIS genérica como fallback.

La nueva ruta elimina trabajo de control e indexado que era necesario para la implementación genérica:

```text
cálculo dinámico de n1/n2
actualización genérica de stages
cadena dinámica ia1..ia7
actualización dinámica de twidCoefModifier
parte del cálculo repetido de índices de twiddles
loops genéricos innecesarios para la geometría fija
```

No se modificó:

```text
orden matemático
valores de twiddle
sumas/restas/multiplicaciones
C81
formato float32
bit reversal
RFFT stage
MFCC
TFLite
gate/thresholds
caches
compiler flags
```

Build P5-G:

```text
text = 903720
data = 512
bss  = 460408
```

Comparado con P5-D:

```text
text : +2472 bytes
data : sin cambio
bss  : sin cambio
```

La sesión 1117 obtuvo:

```text
                       P5-D / 1114    P5-G / 1117
RADIX8_COL1                 64 us          56 us
RADIX8_COL2                 64 us          56 us
RADIX2_PREP                  20 us          20 us
BITREV                       24 us          24 us
CFFT_KERNEL                 150 us         133 us
CFFT_TOTAL                  174 us         158 us
RFFT_STAGE                   51 us          51 us
RFFT_TOTAL / FFT            227 us         210 us
MFCC avg                  40.572 ms      38.876 ms
detect_us_max            426016 us      419228 us
```

Mejoras:

```text
RADIX8_COL1       -12.50 %
RADIX8_COL2       -12.50 %
CFFT_KERNEL       -11.33 %
CFFT_TOTAL         -9.20 %
RFFT_TOTAL         -7.49 %
MFCC avg           -4.18 %
detect_us_max      -1.59 %
```

El radix-8 conjunto pasó de:

```text
128 us/frame -> 112 us/frame
```

lo que representa:

```text
16 us/frame de ahorro
~1.6 ms por MFCC de 100 frames
```

La reducción observada en MFCC fue:

```text
40.572 ms -> 38.876 ms
ahorro = 1.696 ms
```

La magnitud y la localización de la mejora son coherentes con el cambio realizado.

La sesión 1117 ejecutó:

```text
MFCC computes = 62
FFT frames    = 6200
CH0 active    = 15
CH1 active    = 15
CH2 active    = 16
CH3 active    = 16
queue_high    = 1
copy_us_max   = 172 us
errors        = 0
pairs         = 1638
pair_skew     = 1
chunks        = 16
JSON audio    = 64
estado        = OK
```

La diferencia en número de ventanas activas no invalida la comparación de promedios de las etapas internas, ya que se acumularon miles de ejecuciones y la reducción se produjo exactamente en los bloques modificados.

Decisión:

```text
P5-G = ACEPTADO
```

---

## 38. Checkpoint P5-G

P5-G se establece como nuevo checkpoint de respaldo de la campaña de optimización.

Base acumulada:

```text
v2.1.7 estable
P3    - buffers FFT en DTCM
P4-B  - resolver TFLM mínimo
P4-E5 - Conv2D especializado
CACHE-0 / MPU FLASH corregido
P5-C  - twiddle CFFT en DTCM
P5-D  - twiddle RFFT en DTCM
P5-G  - radix-8 especializado para CFFT1024 / columnas512
```

Configuración de compilación:

```text
C       = -O3
C++     = -O2
I-Cache = OFF
D-Cache = OFF
P4-D    = OFF
P5-A    = ON durante diagnóstico
P5-B    = ON durante diagnóstico
```

Build del checkpoint P5-G de diagnóstico:

```text
text = 903720
data = 512
bss  = 460408
```

Benchmark asociado:

```text
sesión           = 1117
RFFT_TOTAL avg   = 210 us/frame
CFFT_KERNEL avg  = 133 us/frame
RADIX8_COL1 avg  = 56 us/frame
RADIX8_COL2 avg  = 56 us/frame
MFCC avg         = 38.876 ms
detect_us_max    = 419228 us
queue_high       = 1
errors           = 0
```

Experimentos descartados durante P5:

```text
P5-E = código radix-8 en ITCM -> sin mejora localizada
P5-F = constantes 512/mod2    -> sin mejora localizada
```

La versión P5-G debe conservarse como punto de retorno seguro antes de continuar con P5-H.

---

## 39. P5-H — plan previo del experimento

Después de P5-G el principal coste interno continúa siendo el radix-8:

```text
CFFT_KERNEL  = 133 us/frame
RADIX8_COL1  =  56 us/frame
RADIX8_COL2  =  56 us/frame
```

Los dos radix-8 representan aproximadamente:

```text
112 / 133 = 84.2 % del CFFT kernel
```

El siguiente experimento debe estudiar el direccionamiento interno del fast-path P5-G, especialmente operaciones repetidas de la forma:

```c
pSrc[2 * i]
pSrc[2 * i + 1]
```

Objetivo P5-H:

```text
reducir aritmética de direccionamiento y trabajo load/store
manteniendo exactamente la misma matemática
```

Reglas:

```text
una sola variable por prueba
P5-G permanece como checkpoint
CMSIS genérico permanece como fallback
sin fast-math
sin reassociation
sin cambios de caches
sin cambios de compiler flags
sin cambios de modelo, MFCC externo, gate o thresholds
P5-A/P5-B permanecen activos para medir el A/B
```

La métrica primaria de aceptación seguirá siendo:

```text
RADIX8_COL1
RADIX8_COL2
```

y cualquier mejora deberá propagarse de forma coherente hacia:

```text
CFFT_KERNEL
CFFT_TOTAL
RFFT_TOTAL
MFCC
detect_us_max
```

---

## 40. P5-H — direccionamiento optimizado del radix-8

P5-H parte directamente del checkpoint P5-G y mantiene una sola variable experimental:

```text
optimizar el direccionamiento/load-store dentro del fast-path radix-8
sin modificar la matemática del butterfly
```

En P5-G el acceso a los datos complejos todavía reconstruía repetidamente direcciones del tipo:

```c
pSrc[2 * i]
pSrc[2 * i + 1]
```

P5-H sustituye ese patrón dentro del fast-path especializado por punteros directos a los ocho complejos del butterfly:

```text
q1
q2
q3
q4
q5
q6
q7
q8
```

y avanza esos punteros directamente entre iteraciones.

Se conservaron:

```text
orden de sumas y restas
orden de multiplicaciones
C81
twiddles
stages FFT
bit reversal
fallback CMSIS genérico
N_FFT = 2048
CFFT interna = 1024
fast-path radix-8 = 512 / modifier 2
```

No se introdujeron:

```text
fast-math
SIMD
reassociation
FMA explícito
cambios de cache
cambios de compiler flags
cambios de modelo
cambios de MFCC externo
cambios de gate o thresholds
```

### Build diagnóstico P5-H

```text
P5-G:
text = 903720
data = 512
bss  = 460408

P5-H:
text = 903704
data = 512
bss  = 460408
```

Diferencia:

```text
text = -16 bytes
data = 0
bss  = 0
```

Build:

```text
0 errors
0 warnings
```

### Sesión 1118

La sesión 1118 mantuvo el sistema estable:

```text
record_chunks = 16
JSON audio    = 64
queue_high    = 1
copy_us_max   = 172 us
pairs         = 1638
pair_skew     = 1
errors        = 0
estado        = OK
```

La actividad acústica de esa sesión no fue uniforme entre canales:

```text
CH0 active_count = 0
CH1 active_count = 16
CH2 active_count = 16
CH3 active_count = 5

MFCC computes = 37
FFT frames    = 3700
```

Por esa razón:

```text
detect_us_max = 339374 us
```

no debe compararse directamente con una sesión de cuatro canales activos durante todas las ventanas.

La decisión de P5-H se tomó sobre las métricas internas acumuladas en 3700 ejecuciones:

```text
                       P5-G / 1117    P5-H / 1118
RADIX8_COL1                 56 us          53 us
RADIX8_COL2                 56 us          53 us
RADIX2_PREP                  20 us          20 us
BITREV                       24 us          24 us
CFFT_KERNEL                 133 us         127 us
CFFT_TOTAL                  158 us         152 us
RFFT_TOTAL / FFT            210 us         204 us
MFCC avg                  38.876 ms      38.325 ms
```

Mejoras P5-G -> P5-H:

```text
RADIX8_COL1       -5.36 %
RADIX8_COL2       -5.36 %
CFFT_KERNEL       -4.51 %
CFFT_TOTAL        -3.80 %
RFFT_TOTAL        -2.86 %
MFCC avg          -1.42 %
```

El criterio previo de aceptación se había fijado aproximadamente en:

```text
56 us -> 52/53 us
```

para ambas columnas.

Resultado:

```text
P5-H = ACEPTADO
```

La reducción aparece exactamente en las dos columnas modificadas y se propaga de forma coherente hacia CFFT, RFFT y MFCC.

P5-H se establece como la última optimización experimental de P5.

---

## 41. Cierre de P5 y Production Clean

Después de aceptar P5-H se retiró la instrumentación utilizada exclusivamente para P5-A/P5-B.

Se eliminaron:

```text
[P5_MFCC_STAGE]
[P5_MFCC_PROFILE]
[P5_FFT_STAGE]
[P5_FFT_PROFILE]

MfccStm32Profile
mfcc_stm32_profile_reset()
mfcc_stm32_profile_get()

P5FftProfile
p5_fft_profile_reset()
p5_fft_profile_get()
p5_fft_profile_add()

cronómetros DWT usados exclusivamente por P5-A/P5-B
```

Se conservó la telemetría P1 necesaria para la validación final:

```text
[P1_CH0]
[P1_CH1]
[P1_CH2]
[P1_CH3]
[P1_CAL_CHx]
[P1_GATE_CHx]
[P1_WINDOW]
```

También se conservaron todas las optimizaciones reales:

```text
P3       - buffers FFT en DTCM
P4-B     - resolver TFLM mínimo
P4-E5    - Conv2D especializado
CACHE-0  - MPU FLASH corregido / caches OFF
P5-C     - twiddle CFFT en DTCM
P5-D     - twiddle RFFT en DTCM
P5-G     - especialización estructural radix-8
P5-H     - direccionamiento optimizado radix-8
```

### Build Production Clean

```text
text = 901968
data = 512
bss  = 460040
dec  = 1362520
```

Resultado:

```text
0 errors
0 warnings
```

Comparado con P5-H diagnóstico:

```text
                 P5-H diag     Production Clean
text               903704          901968
data                  512             512
bss                460408          460040
```

Reducción debida a la limpieza:

```text
text = -1736 bytes
data = 0
bss  = -368 bytes
```

La reducción corresponde a instrumentación eliminada; no se retiraron las optimizaciones aceptadas.

---

## 42. Sesión 1119 — benchmark final Production Clean

La sesión 1119 se utiliza como benchmark final porque los cuatro canales estuvieron activos durante las 16 ventanas grabadas:

```text
CH0 active_count = 16 / 16
CH1 active_count = 16 / 16
CH2 active_count = 16 / 16
CH3 active_count = 16 / 16
active mask      = 0x0F
```

Resultados globales:

```text
frames         = 837900
completed      = 19
detected       = 19
record_chunks  = 16
queue_high     = 1
copy_us_max    = 172 us
detect_us_max  = 415847 us
pairs          = 1638
pair_skew      = 1
errors         = 0
```

P1 final:

```text
CH0:
pre_max_us     = 26986
mfcc_max_us    = 38266
tflite_max_us  = 38784
total_max_us   = 103986

CH1:
pre_max_us     = 26784
mfcc_max_us    = 38264
tflite_max_us  = 38769
total_max_us   = 103802

CH2:
pre_max_us     = 26510
mfcc_max_us    = 38433
tflite_max_us  = 38767
total_max_us   = 103539

CH3:
pre_max_us     = 27503
mfcc_max_us    = 38442
tflite_max_us  = 38770
total_max_us   = 104515
```

Ventana completa:

```text
[P1_WINDOW]
last_us = 407492
max_us  = 415846
windows = 19
```

Transferencia:

```text
chunks recibidos      = 16
chunks JSON completos = 16
JSON de audio         = 64
órdenes               = 1 -> 16
estado                = OK
```

Se observaron retries UART durante la transferencia posterior a la captura. No invalidan el benchmark de cómputo porque:

```text
la captura ya había terminado
los 16 chunks fueron recibidos
los 64 JSON fueron generados
el estado final fue OK
```

La ausencia de logs P5 en esta sesión confirma que el Production Clean se ejecutó sin la instrumentación interna de P5-A/P5-B.

---

## 43. Resultado acumulado de la campaña de optimización

Benchmark original de referencia:

```text
sesión 1081
detect_us_max = 2227136 us
queue_high    = 8
```

Benchmark final:

```text
sesión 1119
detect_us_max = 415847 us
queue_high    = 1
```

Mejora acumulada:

```text
2227136 us -> 415847 us

reducción = 81.33 %
speedup   = 5.36x
```

El procesamiento máximo por ventana queda por debajo de un segundo:

```text
415847 us = 0.415847 s
```

Margen respecto al siguiente segundo de audio:

```text
1000000 - 415847 = 584153 us
```

equivalente a aproximadamente:

```text
584 ms de margen
```

La cola dejó de crecer:

```text
queue_high:
8 -> 1
```

y la copia siguió siendo pequeña frente al coste de inferencia:

```text
copy_us_max = 172 us
```

Conclusión de la campaña:

```text
la limitación original estaba en el consumidor de detección,
principalmente MFCC/TFLite y sus kernels internos,
no en la copia DMA -> SDRAM.
```

Estado final aceptado:

```text
v2.1.7 estable
+ P3
+ P4-B
+ P4-E5
+ CACHE-0 / MPU-FIX
+ P5-C
+ P5-D
+ P5-G
+ P5-H
+ Production Clean
```

Configuración congelada:

```text
C       = -O3
C++     = -O2
I-Cache = OFF
D-Cache = OFF
P4-D    = OFF
P5-A/B  = OFF en producción
```

La versión correspondiente a la sesión 1119 se considera el nuevo punto de retorno seguro antes de comenzar localización acústica.

---

## 44. TOA-0 — validación temporal antes de TDOA/localización

TOA-0 inicia una etapa nueva del proyecto.

El objetivo inmediato todavía no es estimar dirección ni posición. Primero se debe demostrar que las cuatro señales RAW pueden compararse temporalmente con una referencia de muestra consistente.

La arquitectura ya utiliza:

```text
SAI2A = MASTER_RX / ASYNCHRONOUS
SAI2B = SLAVE_RX  / SYNCHRONOUS
```

con BCLK y WS/LRCL compartidos entre ambas parejas de micrófonos.

Esto es favorable para TOA, pero no sustituye la validación de alineación de muestras en todo el pipeline:

```text
I2S/SAI
-> DMA A/B
-> emparejamiento de bloques
-> deinterleaving de slots
-> SDRAM
-> chunks de 44100 muestras
-> JSON/WAV
```

El valor:

```text
pair_skew = 1
```

demuestra que el emparejamiento de bloques de captura permanece controlado, pero por sí solo no prueba que CH0[n], CH1[n], CH2[n] y CH3[n] representen exactamente el mismo instante acústico.

### Objetivos de TOA-0

```text
1. Verificar continuidad temporal de cada canal RAW.
2. Verificar que no existe desplazamiento artificial fijo introducido por DMA,
   pairing, slots o deinterleaving.
3. Medir retardos relativos entre canales sobre la misma ventana RAW.
4. Separar retardo físico acústico de cualquier retardo de adquisición.
5. Definir una convención única de signo para los retardos.
6. Validar el análisis en PC antes de llevar TDOA/localización al STM32.
```

### Regla de trabajo

Durante TOA-0 no se modificará el pipeline de detección Production Clean salvo que una prueba demuestre una necesidad concreta.

La primera fase se realizará preferentemente sobre audio RAW ya capturado:

```text
4 canales
misma sesión
misma order
44100 muestras por canal
sin DSP MFCC
sin tanh/HPF del detector
sin selección de micrófono principal
```

### TOA-0A — primera prueba propuesta

Tomar una única orden de una sesión con los cuatro canales válidos y cargar las cuatro señales RAW correspondientes a esa misma orden.

Para cada par se calculará:

```text
correlación cruzada normalizada
lag entero de máximo
lag en muestras
lag en microsegundos
coeficiente de correlación
```

Pares mínimos:

```text
CH0 <-> CH1
CH0 <-> CH2
CH0 <-> CH3
CH1 <-> CH2
CH1 <-> CH3
CH2 <-> CH3
```

A 44.1 kHz:

```text
1 muestra = 1 / 44100 s
          = 22.6757 us
```

Todavía no se fija una ventana máxima de TDOA basada en geometría física. Ese límite se establecerá cuando estén confirmadas y documentadas las posiciones/distancias reales del montaje actual.

### Criterio de salida de TOA-0

TOA-0 se considerará completado cuando podamos afirmar, con datos RAW:

```text
- qué significa lag positivo y negativo;
- que los cuatro canales conservan una relación temporal estable;
- que no existe un offset artificial desconocido entre SAI2A y SAI2B;
- cuál es la resolución temporal real con correlación a muestra entera;
- qué pares de micrófonos producen correlación utilizable;
- qué rango físico de búsqueda debe utilizar TOA/TDOA.
```

Solo después se continuará con:

```text
TOA-1  -> estimación robusta de TDOA
TOA-2  -> geometría del arreglo
TOA-3  -> dirección/localización
TOA-4  -> integración en STM32/Khamex
```

---

## 45. TOA-0A — correlación RAW de la sesión 1119, orden 2

Para la primera validación temporal se utilizó:

```text
session_id   = 1119
order_number = 2
sample_rate  = 44100 Hz
```

Canales analizados:

```text
CH0 = left
CH1 = top
CH2 = back
CH3 = right
```

Cada archivo contiene exactamente:

```text
44100 muestras
```

No se encontraron NaN ni infinitos.

La señal RAW no presenta clipping. Los picos absolutos observados fueron:

```text
left  = 0.057892
top   = 0.058228
back  = 0.042175
right = 0.087494
```

RMS RAW:

```text
left  = 0.0137123
top   = 0.0115057
back  = 0.0098561
right = 0.0160364
```

La correlación de TOA-0A se realizó sobre las muestras RAW, eliminando únicamente la media DC antes de correlacionar.

No se aplicó:

```text
HPF del detector
tanh
MFCC
band-pass
TFLite
selección de micrófono principal
```

### Convención de signo

Para un par:

```text
A -> B
```

se define:

```text
lag = tiempo(B) - tiempo(A)
```

Por tanto:

```text
lag > 0  -> B llega después que A
lag < 0  -> B llega antes que A
lag = 0  -> mismo índice de llegada a resolución de una muestra
```

A 44.1 kHz:

```text
1 muestra = 22.6757 us
```

### Correlación cruzada RAW — segundo completo

```text
Par             lag       lag_us       corr
left -> top     +12      +272.109      0.5660
left -> back    +11      +249.433      0.5084
left -> right    -3       -68.027      0.6730
top  -> back     -1       -22.676      0.6361
top  -> right   -15      -340.136      0.6110
back -> right   -14      -317.460      0.6084
```

En los seis pares el máximo absoluto relevante corresponde a correlación positiva; no se observó una inversión de polaridad dominante.

### Vector relativo de llegada

Tomando `left = 0` como referencia, los seis pares son compatibles exactamente con:

```text
right = -3 muestras
left  =  0 muestras
back  = +11 muestras
top   = +12 muestras
```

Orden temporal relativo:

```text
right
  |
  +3 samples
  v
left
  |
  +11 samples
  v
back
  |
  +1 sample
  v
top
```

La consistencia de ciclos es exacta en muestra entera.

Ejemplos:

```text
(left -> top) + (top -> back)
= 12 + (-1)
= 11
= left -> back
```

y:

```text
(left -> top) + (top -> right)
= 12 + (-15)
= -3
= left -> right
```

Esto demuestra consistencia interna de los retardos medidos.

### Estabilidad dentro del segundo

La orden 2 también se dividió en diez ventanas consecutivas de 100 ms.

Resultados:

```text
left -> top:
12 muestras en 9/10 ventanas
13 muestras en 1/10 ventanas

left -> back:
11 muestras en 10/10 ventanas

left -> right:
-3 muestras en 10/10 ventanas

top -> back:
-1 muestra en 10/10 ventanas

top -> right:
-15 muestras en 10/10 ventanas

back -> right:
-14 muestras en 10/10 ventanas
```

Por tanto, 59 de las 60 estimaciones par/ventana coinciden exactamente con el lag del segundo completo. La única variación fue de una muestra en `left -> top`.

Esto es una evidencia fuerte de que no existe deriva temporal apreciable ni pérdida progresiva de muestras durante este segundo.

### Qué demuestra TOA-0A

Los datos permiten afirmar:

```text
- los cuatro canales tienen la misma longitud temporal;
- existe correlación utilizable entre los seis pares;
- los retardos RAW son pequeños y estables;
- los seis retardos son algebraicamente consistentes;
- no aparece una inversión de polaridad dominante;
- no se observa drift a lo largo de la orden 2.
```

### Qué todavía NO demuestra

Una única fuente acústica, una única posición y una única orden no permiten separar de forma definitiva:

```text
retardo acústico físico
vs.
offset fijo de adquisición entre SAI2A y SAI2B
```

Un offset fijo de adquisición podría quedar sumado al retardo acústico y mantenerse estable.

Por ello todavía no se declara:

```text
offset SAI2A/SAI2B = 0
```

### Estado TOA-0A

```text
TOA-0A = PASS
```

La captura es suficientemente coherente para continuar con validación temporal.

### Siguiente etapa: TOA-0B

TOA-0B debe verificar estabilidad entre distintas órdenes y posteriormente separar retardo físico de un posible offset fijo de adquisición.

La siguiente comprobación recomendada es repetir el mismo análisis sobre varias órdenes completas de una misma sesión con la fuente inmóvil.

Después, con la geometría física del arreglo confirmada, se realizará una prueba controlada que permita determinar si existe un offset fijo entre los dos bloques SAI.

---

## 46. TOA-0B — estabilidad temporal entre órdenes 2–6 de la sesión 1119

TOA-0B repite exactamente el método definido en TOA-0A, ahora sobre cinco segundos consecutivos de la misma sesión:

```text
session_id = 1119
orders     = 2, 3, 4, 5, 6
sample_rate = 44100 Hz
```

Para cada orden se utilizaron los cuatro canales RAW:

```text
left
top
back
right
```

Cada señal contiene exactamente:

```text
44100 muestras
```

No se encontraron NaN ni infinitos.

El análisis se mantiene sobre señal RAW eliminando únicamente la media DC antes de la correlación. No se aplicó HPF, tanh, MFCC, TFLite ni selección de micrófono principal.

La convención sigue siendo:

```text
lag = tiempo(B) - tiempo(A)

lag > 0 -> B llega después de A
lag < 0 -> B llega antes de A
```

A 44.1 kHz:

```text
1 muestra = 22.6757 us
```

### Resultado por segundo completo

Los seis pares producen exactamente el mismo lag entero en las cinco órdenes:

```text
Par             ord2  ord3  ord4  ord5  ord6

left -> top      +12   +12   +12   +12   +12
left -> back     +11   +11   +11   +11   +11
left -> right     -3    -3    -3    -3    -3
top  -> back      -1    -1    -1    -1    -1
top  -> right    -15   -15   -15   -15   -15
back -> right    -14   -14   -14   -14   -14
```

Equivalentes temporales:

```text
left -> top    +12 = +272.109 us
left -> back   +11 = +249.433 us
left -> right   -3 =  -68.027 us
top  -> back    -1 =  -22.676 us
top  -> right  -15 = -340.136 us
back -> right  -14 = -317.460 us
```

Por tanto:

```text
30 / 30 estimaciones par-segundo
coinciden exactamente con el mismo lag entero.
```

La solución relativa usando `left = 0` se mantiene durante los cinco segundos:

```text
right = -3 muestras
left  =  0 muestras
back  = +11 muestras
top   = +12 muestras
```

y los ciclos cierran exactamente:

```text
left -> top + top -> back
12 + (-1) = 11 = left -> back

left -> top + top -> right
12 + (-15) = -3 = left -> right

left -> back + back -> right
11 + (-14) = -3 = left -> right
```

### Correlación normalizada del segundo completo

Rango observado entre órdenes 2–6:

```text
left -> top    0.5439 .. 0.6106
left -> back   0.5085 .. 0.5999
left -> right  0.6377 .. 0.6895
top  -> back   0.6339 .. 0.6970
top  -> right  0.6033 .. 0.6540
back -> right  0.6086 .. 0.6455
```

Todos los pares mantienen correlación positiva utilizable.

### Validación interna con ventanas de 100 ms

Cada uno de los cinco segundos se dividió en diez ventanas:

```text
5 órdenes x 10 ventanas x 6 pares = 300 estimaciones
```

Distribución agregada:

```text
left -> top:
+12 = 48 / 50
+13 =  2 / 50

left -> back:
+11 = 47 / 50
+10 =  3 / 50

left -> right:
 -3 = 48 / 50
 -4 =  2 / 50

top -> back:
 -1 = 47 / 50
 -2 =  3 / 50

top -> right:
-15 = 48 / 50
-16 =  2 / 50

back -> right:
-14 = 50 / 50
```

Resumen:

```text
288 / 300 = 96.0 % exactamente en el lag modal
300 / 300 = 100 % dentro de +/-1 muestra
```

La máxima desviación observada es:

```text
1 muestra = 22.6757 us
```

No aparece ninguna ventana con un salto mayor.

### Resultado TOA-0B

```text
TOA-0B = PASS
```

La sesión 1119 demuestra, entre órdenes 2–6:

```text
- estabilidad entre segundos consecutivos;
- estabilidad dentro de cada segundo;
- ausencia de drift progresivo observable;
- ausencia de pérdida o inserción de muestras observable;
- relación temporal estable entre los cuatro canales;
- seis pares compatibles con un único vector relativo de llegada.
```

La evidencia es considerablemente más fuerte que TOA-0A porque ahora el mismo resultado se conserva durante cinco segundos consecutivos de la misma sesión.

### Limitación que permanece abierta

TOA-0B demuestra estabilidad relativa, pero todavía no separa de forma definitiva:

```text
retardo acústico real
vs.
offset fijo de adquisición
```

Un offset constante entre grupos de adquisición podría permanecer estable durante todos los segundos y quedar sumado al retardo físico.

Por tanto todavía no se declara:

```text
offset fijo SAI2A/SAI2B = 0
```

El siguiente objetivo de TOA-0 debe ser una prueba controlada que permita aislar ese término fijo antes de avanzar a TDOA geométrico.

---

## 47. TOA-0C — validación espacial controlada en cuatro posiciones

Después de TOA-0A y TOA-0B se realizó una campaña controlada con la fuente acústica fija en cuatro posiciones cardinales alrededor del arreglo. El objetivo fue comprobar que los TDOA cambian de signo y magnitud de acuerdo con la dirección física de la fuente, manteniendo el mismo método de correlación RAW usado anteriormente.

### 47.1 Geometría física usada

El arreglo medido es un rectángulo de 9 cm x 12 cm:

```text
MIC7 / left                 MIC2 / right
(-4.5 cm, +6 cm)            (+4.5 cm, +6 cm)

        9 cm
     <-------->

        |                        |
        |                        | 12 cm
        |                        |

MIC5 / top                  MIC6 / back
(-4.5 cm, -6 cm)            (+4.5 cm, -6 cm)
```

La diagonal es:

```text
sqrt(9^2 + 12^2) = 15 cm
```

Correspondencia física/software utilizada en esta campaña:

```text
left  = MIC7 = esquina superior izquierda
right = MIC2 = esquina superior derecha
top   = MIC5 = esquina inferior izquierda
back  = MIC6 = esquina inferior derecha
```

Importante: los nombres software `top` y `back` son etiquetas históricas del proyecto y no describen la posición geométrica actual del micrófono.

A 44.1 kHz:

```text
1 muestra = 22.6757 us
```

Con velocidad del sonido aproximada de 343 m/s, los retardos máximos geométricos aproximados son:

```text
9 cm  -> 11.57 muestras
12 cm -> 15.43 muestras
15 cm -> 19.29 muestras
```

### 47.2 Protocolo

Se utilizaron cuatro sesiones independientes con el teléfono inmóvil durante toda la sesión:

```text
1120 -> ARRIBA
1121 -> DERECHA
1122 -> ABAJO
1123 -> IZQUIERDA
```

En cada sesión se analizaron:

```text
orders 2, 3, 4, 5 y 6
```

Cada canal contiene 44100 muestras por orden.

Método:

```text
- señal RAW;
- eliminación únicamente de la media DC;
- correlación cruzada normalizada;
- estimación de lag entero;
- comprobación adicional en ventanas de 100 ms.
```

Convención:

```text
lag(A -> B) = tiempo(B) - tiempo(A)

lag > 0 -> B recibe después que A
lag < 0 -> B recibe antes que A
```

### 47.3 Session 1123 — fuente a la IZQUIERDA

Los cinco segundos completos entregaron exactamente los mismos lags enteros:

| Par | Ord2 | Ord3 | Ord4 | Ord5 | Ord6 |
|---|---:|---:|---:|---:|---:|
| left -> top | 0 | 0 | 0 | 0 | 0 |
| left -> back | +8 | +8 | +8 | +8 | +8 |
| left -> right | +8 | +8 | +8 | +8 | +8 |
| top -> back | +9 | +9 | +9 | +9 | +9 |
| top -> right | +9 | +9 | +9 | +9 | +9 |
| back -> right | 0 | 0 | 0 | 0 | 0 |

Por tanto:

```text
30 / 30 comparaciones de segundo completo
mantienen exactamente el mismo lag.
```

Rangos de correlación normalizada observados durante las cinco órdenes:

```text
left -> top    0.4859 .. 0.5932
left -> back   0.4907 .. 0.5421
left -> right  0.5926 .. 0.6842
top  -> back   0.6421 .. 0.7074
top  -> right  0.3994 .. 0.5105
back -> right  0.4719 .. 0.5256
```

El ajuste conjunto de los seis pares, usando `left = 0`, da:

```text
left  =  0.00 muestras
top   = -0.50 muestras
back  = +8.25 muestras
right = +8.25 muestras
```

El error residual máximo del ajuste es solamente:

```text
0.5 muestra
```

La lectura física es clara:

```text
MIC7 / left
MIC5 / top
      reciben primero

MIC2 / right
MIC6 / back
      reciben aproximadamente 8–9 muestras después
```

Esto coincide con una fuente situada a la izquierda del arreglo.

### 47.4 Session 1123 — estabilidad en ventanas de 100 ms

Se realizaron:

```text
5 órdenes x 10 ventanas x 6 pares = 300 estimaciones
```

Distribución:

```text
left -> top:
  0 = 48 / 50
 -1 =  2 / 50

left -> back:
 +8 = 49 / 50
 +9 =  1 / 50

left -> right:
 +8 = 48 / 50
 +9 =  2 / 50

top -> back:
 +9 = 41 / 50
 +8 =  9 / 50

top -> right:
 +9 = 46 / 50
 +8 =  1 / 50
 otros picos aislados = 3 / 50

back -> right:
  0 = 42 / 50
 otros picos = 8 / 50
```

Resumen global:

```text
274 / 300 = 91.3 % exactamente en el lag modal
289 / 300 = 96.3 % dentro de +/-1 muestra
```

Los outliers de ventana corta se concentran principalmente en pares cuyo retardo real está cerca de cero o cuya correlación es menor. El resultado de segundo completo permanece estable en 30/30 mediciones.

### 47.5 Tabla final de la campaña cardinal

Se usa la mediana de las órdenes 2–6 como valor representativo de cada sesión.

| Sesión | Posición | L->T | L->B | L->R | T->B | T->R | B->R | Resultado |
|---|---|---:|---:|---:|---:|---:|---:|---|
| 1120 | ARRIBA | +15 | +16 | +2 | +1 | -14 | -15 | PASS |
| 1121 | DERECHA | -2 | -11 | -10 | -8 | -8 | +1 | PASS |
| 1122 | ABAJO | -16 | -16 | -2 | -1 | +14 | +15 | PASS |
| 1123 | IZQUIERDA | 0 | +8 | +8 | +9 | +9 | 0 | PASS |

El patrón espacial es el esperado:

```text
ARRIBA:
fila superior recibe primero;
los retardos verticales son grandes y positivos.

DERECHA:
MIC2/right y MIC6/back reciben primero;
los retardos horizontales son negativos.

ABAJO:
fila inferior recibe primero;
los retardos verticales cambian de signo respecto a ARRIBA.

IZQUIERDA:
MIC7/left y MIC5/top reciben primero;
los retardos horizontales cambian de signo respecto a DERECHA.
```

### 47.6 Indicadores de eje

Para resumir la dirección se definen dos indicadores simples:

```text
H = (lag(left->right) + lag(top->back)) / 2

V = (lag(left->top) - lag(back->right)) / 2
```

Interpretación:

```text
H > 0 -> fuente hacia IZQUIERDA
H < 0 -> fuente hacia DERECHA

V > 0 -> fuente hacia ARRIBA
V < 0 -> fuente hacia ABAJO
```

Resultados:

| Sesión | Posición real | H [samples] | V [samples] | Dirección indicada |
|---|---|---:|---:|---|
| 1120 | ARRIBA | +1.5 | +15.0 | ARRIBA |
| 1121 | DERECHA | -9.0 | -1.5 | DERECHA |
| 1122 | ABAJO | -1.5 | -15.5 | ABAJO |
| 1123 | IZQUIERDA | +8.5 | 0.0 | IZQUIERDA |

Las cuatro sesiones quedan correctamente separadas únicamente por sus TDOA.

### 47.7 Comparación de posiciones opuestas

#### ARRIBA vs ABAJO

```text
1120 ARRIBA:
V = +15.0 samples

1122 ABAJO:
V = -15.5 samples
```

Centro del par:

```text
(+15.0 + -15.5) / 2 = -0.25 muestra
```

#### DERECHA vs IZQUIERDA

```text
1121 DERECHA:
H = -9.0 samples

1123 IZQUIERDA:
H = +8.5 samples
```

Centro del par:

```text
(-9.0 + +8.5) / 2 = -0.25 muestra
```

En ambos ejes, el centro de las posiciones opuestas queda aproximadamente a:

```text
-0.25 muestra
≈ -5.67 us
```

Esto no demuestra matemáticamente que cada ruta de adquisición tenga offset exactamente cero, porque las posiciones físicas del teléfono, reflexiones y cuantización a muestra entera siguen formando parte de la medida.

Sin embargo, sí permite afirmar que en esta campaña:

```text
- no aparece un offset fijo grande;
- no aparece un sesgo que impida invertir correctamente la dirección;
- las posiciones opuestas cambian el signo del TDOA como se espera;
- el centro de ambos pares cardinales queda muy cerca de cero.
```

### 47.8 Resultado TOA-0C

```text
TOA-0C = PASS
```

La campaña de sesiones 1120–1123 demuestra experimentalmente que:

```text
- los cuatro canales conservan sincronía relativa estable;
- los TDOA son repetibles cuando la fuente permanece fija;
- los TDOA cambian cuando cambia la posición física de la fuente;
- ARRIBA, DERECHA, ABAJO e IZQUIERDA producen firmas diferentes;
- las posiciones opuestas presentan inversión de signo coherente;
- los retardos medidos son compatibles con las dimensiones físicas del arreglo dentro de la resolución de una muestra y de las limitaciones acústicas del entorno.
```

---

## 48. Cierre de TOA-0

Estado final:

```text
TOA-0A = PASS
Correlación RAW de cuatro canales y consistencia interna.

TOA-0B = PASS
Estabilidad temporal entre segundos consecutivos sin drift observable.

TOA-0C = PASS
Respuesta espacial correcta en cuatro posiciones cardinales conocidas.
```

Conclusión de TOA-0:

```text
La captura de cuatro canales es suficientemente estable y coherente
para comenzar la etapa de TDOA/localización.

No se observa evidencia de drift progresivo ni de un offset fijo grande
capaz de dominar la información espacial.
```

La siguiente etapa recomendada es TOA-1:

```text
- estimación submuestra;
- comparación de correlación clásica y GCC-PHAT;
- selección de ventanas útiles;
- límites geométricos por cada par;
- conversión de TDOA a diferencia de distancia;
- preparación del solucionador de dirección/localización.
```

---

## 49. TOA-1 — validación submuestra y angular en PC

Antes de portar la localización al STM32 se validó el cálculo de TDOA submuestra y azimuth sobre las sesiones controladas ya grabadas.

### 49.1 TOA-1A — interpolación submuestra

Se mantuvo la correlación normalizada sobre audio RAW y se añadió interpolación parabólica alrededor del pico entero:

```text
lag_sub = k + delta
```

A partir de los TDOA se definieron:

```text
H = (lag(left->right) + lag(top->back)) / 2
V = (lag(left->top) - lag(back->right)) / 2
```

El azimuth se expresa en:

```text
0°   = ARRIBA
90°  = DERECHA
180° = ABAJO
270° = IZQUIERDA
```

Resultados cardinales:

| Sesión | Posición nominal | Azimuth estimado |
|---:|---|---:|
| 1120 | 0° | 353.81° |
| 1121 | 90° | 96.82° |
| 1122 | 180° | 171.89° |
| 1123 | 270° | 268.97° |

Resultado:

```text
TOA-1A = PASS
```

La estimación submuestra fue estable entre órdenes consecutivas y las cuatro posiciones cardinales quedaron correctamente identificadas.

### 49.2 TOA-1B — cuatro posiciones intermedias

Se añadieron cuatro posiciones nominales intermedias:

| Sesión | Posición nominal | Azimuth estimado | Cuadrante |
|---:|---:|---:|---|
| 1124 | 45° | 42.73° | correcto |
| 1125 | 135° | 153.35° | correcto |
| 1126 | 225° | 209.20° | correcto |
| 1127 | 315° | 339.14° | correcto |

Las cuatro sesiones presentaron una repetibilidad muy alta entre órdenes 2–6.

La diferencia respecto al ángulo nominal no se considera todavía error angular absoluto, porque la fuente fue colocada manualmente y no existe una referencia mecánica independiente del ángulo real.

La orientación física actual también queda documentada:

```text
los cuatro micrófonos miran hacia 0°
```

Conclusión:

```text
TOA-1B dirección/cuadrante = PASS
TOA-1B repetibilidad       = PASS
TOA-1B exactitud absoluta  = NOT EVALUATED
```

El sistema demuestra respuesta angular continua alrededor de los 360° sin saltos de cuadrante.

---

## 50. TOA-1C — localización online en STM32

TOA-1C portó al CM7 el cálculo de TDOA submuestra, `H`, `V`, azimuth y dirección, utilizando directamente el `RecorderChunkView` PCM16 que ya existe en el pipeline productor-consumidor.

La integración se realiza después de `drone_detection_process_window()` y antes de liberar el slot mediante `audio_recorder_complete_processing()`.

Durante esta etapa:

```text
captura = 44.1 kHz / 4 canales
sesión = 3 s calibración + 16 órdenes de 1 s
MFCC/TFLite = activos
JSON = desactivado
transferencia de audio = desactivada
salida TOA = UART
```

El módulo `toa_localizer` no reserva buffers estáticos grandes. En el `.map` de TOA-1C:

```text
toa_localizer.o .data = 0
toa_localizer.o .bss  = 0
```

La localización trabaja directamente sobre los PCM16 existentes.

### 50.1 TOA-1C.1 — ventana completa de 1 segundo

La primera implementación online utilizó las 44,100 muestras del bloque de 1 segundo para cada correlación.

Prueba controlada en posición `UP_RIGHT`:

```text
azimuth medio ≈ 30.56°
std azimuth   ≈ 0.29°
H medio       ≈ -4.713 samples
V medio       ≈ +10.641 samples
```

La estimación angular fue muy estable, pero el coste temporal fue demasiado alto:

```text
det_us  ≈ 0.41 s
toa_us  ≈ 2.70–3.14 s
proc_us ≈ 3.11–3.56 s
queue_high = 11
errors     = 0
```

Conclusión:

```text
TOA-1C.1 cálculo/localización = PASS
TOA-1C.1 RAM                  = PASS
TOA-1C.1 real-time 1 Hz       = FAIL
```

El problema no estaba en captura, DMA ni SDRAM. El cuello de botella era el número de operaciones de correlación sobre 44,100 muestras.

### 50.2 TOA-1C.2 — ventana central de 100 ms

Se mantuvo el mismo estimador y se redujo únicamente la cantidad de muestras procesadas por TOA.

Configuración:

```text
bloque disponible       = 44100 samples
ventana TOA             = 4410 samples
inicio                   = sample 19845
fin                      = sample 24254
posición dentro del s    = 450–550 ms
duración                 = 100 ms
```

No se crea un buffer nuevo. Los cuatro punteros de canal se desplazan hasta el inicio de la ventana central.

La implementación STM32 usada en estas pruebas conserva:

```text
correlación normalizada por lag
interpolación parabólica submuestra
TOA_MAX_LAG_HORIZONTAL = 14
TOA_MAX_LAG_VERTICAL   = 18
TOA_MAX_LAG_DIAGONAL   = 22
```

Nota importante:

```text
la implementación online STM32 actual NO se declara todavía
paridad matemática estricta con la búsqueda offline ±100 del PC
```

La prueba inicial `UP_RIGHT` con 100 ms obtuvo:

```text
azimuth medio ≈ 32.59°
std azimuth   ≈ 0.32°
H medio       ≈ -5.025 samples
V medio       ≈ +10.483 samples

toa_us  ≈ 0.313 s
proc_us ≈ 0.726 s
queue_high = 1
errors     = 0
```

Comparado con la ventana de 1 segundo:

```text
toa_us:
~3.07 s -> ~0.313 s

mejora aproximada:
~9.8x
```

La reducción de ventana recuperó margen suficiente para procesamiento a 1 Hz sin ampliar la cola.

Conclusión:

```text
TOA-1C.2 RAM            = PASS
TOA-1C.2 real-time 1 Hz = PASS
```

### 50.3 Validación cardinal de TOA-1C.2

Se validó la ventana fija de 100 ms con la fuente colocada manualmente a aproximadamente 50 cm del centro del arreglo en:

```text
0°   UP
90°  RIGHT
180° DOWN
270° LEFT
```

Los cuatro micrófonos permanecieron físicamente orientados hacia `0°`.

#### 0° — UP

Las 16 órdenes se clasificaron como `UP`.

```text
azimuth medio = 354.52°
std azimuth   = 0.59°
rango         = 353.39°–355.61°

H = +1.075 ± 0.116 samples
V = +14.943 ± 0.063 samples

queue_high = 1
errors     = 0
```

Resultado:

```text
dirección      = PASS
repetibilidad  = PASS
real-time      = PASS
```

#### 90° — RIGHT

Apareció un cluster dominante y extremadamente repetible alrededor de `RIGHT`.

En las 10 órdenes pertenecientes al cluster coherente:

```text
azimuth medio = 102.45°
std azimuth   = 0.44°

H = -8.842 ± 0.092 samples
V = -2.603 ± 0.081 samples
```

Sin embargo, varias ventanas seleccionaron picos alternativos de correlación y produjeron azimuths incorrectos.

Entre las anomalías observadas después de estar los cuatro canales activos se encuentran:

```text
seq4  -> 24.72°
seq11 -> 46.17°
seq15 -> 50.96°
seq16 -> 74.03°
```

Las correlaciones individuales no siempre fueron bajas durante estos fallos.

Resultado:

```text
dirección dominante correcta         = PASS
repetibilidad del cluster coherente  = PASS
robustez de selección de pico        = PENDIENTE
real-time                             = PASS
```

#### 180° — DOWN

Las 16 órdenes se clasificaron como `DOWN`.

```text
azimuth medio = 167.24°
std azimuth   = 0.38°
rango         = 166.63°–167.85°

H = -2.602 ± 0.077 samples
V = -15.316 ± 0.049 samples

queue_high = 1
errors     = 0
```

Resultado:

```text
dirección      = PASS
repetibilidad  = PASS
real-time      = PASS
```

#### 270° — LEFT

Se obtuvo un cluster coherente `LEFT` en 14 de 16 órdenes.

Para ese cluster:

```text
azimuth medio = 272.79°
std azimuth   = 0.92°

H = +8.611 ± 0.096 samples
V = +0.560 ± 0.185 samples
```

Dos ventanas mostraron soluciones alternativas:

```text
seq1  -> 145.68°
seq12 -> 293.89°
```

En `seq12`, por ejemplo:

```text
LB = +9.44
BR = -9.69
LR = +8.77
```

La identidad esperada:

```text
LR ≈ LB + BR
```

queda fuertemente violada:

```text
LB + BR ≈ -0.25
LR       ≈ +8.77
```

Resultado:

```text
dirección dominante correcta         = PASS
repetibilidad del cluster coherente  = PASS
robustez de selección de pico        = PENDIENTE
real-time                             = PASS
```

### 50.4 Conclusión de la campaña cardinal 100 ms

La ventana de 100 ms no presenta dispersión angular alta cuando selecciona el pico correcto.

Los resultados muestran dos comportamientos:

```text
pico correcto
    ->
H/V altamente repetibles
    ->
azimuth estable

pico alternativo en uno o más pares
    ->
TDOA internamente incompatibles
    ->
salto de azimuth
```

Por tanto, el problema restante no se considera un problema de RAM ni de presupuesto temporal.

Estado:

```text
captura                     = PASS
RAM                         = PASS
procesamiento < 1 s         = PASS
queue_high = 1              = PASS
errors = 0                  = PASS
repetibilidad coherente     = PASS
selección robusta de pico   = PENDIENTE
```

---

## 51. TOA-1C.3 — checkpoint de coherencia geométrica

Las pruebas cardinales demostraron que una correlación alta no es suficiente para declarar una localización válida.

La versión actual marca `valid=0` únicamente cuando el máximo de algún par cae en el borde del rango de búsqueda.

Esto no detecta los casos donde un par selecciona un pico acústico alternativo dentro del rango.

### 51.1 Relaciones de cierre entre los seis TDOA

Con la convención:

```text
lag(A->B) = time(B) - time(A)
```

los seis pares deben cumplir aproximadamente:

```text
LB = LT + TB
LR = LT + TR
LR = LB + BR
TR = TB + BR
```

Se define como criterio candidato:

```text
e1 = |LB - LT - TB|
e2 = |LR - LT - TR|
e3 = |LR - LB - BR|
e4 = |TR - TB - BR|

closure_error = max(e1, e2, e3, e4)
```

Una ventana con `closure_error` elevado no debe producir una dirección operacional aunque las correlaciones individuales sean altas.

### 51.2 Decisión de diseño

Para el pipeline final se prefiere:

```text
localización incoherente
    ->
resultado TOA no válido
```

en lugar de:

```text
localización incoherente
    ->
publicar un azimuth incorrecto
```

El umbral numérico de `closure_error` no se fija todavía arbitrariamente. Debe derivarse de las distribuciones observadas en ventanas coherentes y anómalas.

### 51.3 Estado del checkpoint

Este checkpoint cierra la etapa experimental TOA-1C con las siguientes decisiones:

```text
ventana TOA              = 100 ms centrales
procesamiento 1 Hz       = viable
cola                     = estable
captura                   = estable
estimador submuestra      = operativo
azimuth/dirección         = operativo
closure geométrico        = definido como requisito
gating automático closure = pendiente de integración
```

Por tanto:

```text
TOA-1C.3 = CHECKPOINT
```

Importante: este nombre identifica el checkpoint de cierre de la campaña TOA-1C y la decisión de diseño de coherencia. El firmware medido en esta campaña todavía no implementa un umbral automático de `closure_error`.

Tampoco se declara todavía exactitud angular absoluta, ya que las posiciones de fuente se realizaron manualmente.

---

## 52. Siguiente etapa — TOA-1D

TOA-1D integrará la localización con el comportamiento operacional del detector.

Objetivos:

```text
detector de dron
    ->
TOA/TDOA 100 ms
    ->
validación de coherencia
    ->
azimuth + dirección + estado válido
    ->
salida operacional
```

La siguiente etapa debe mantener inicialmente:

```text
captura productor-consumidor validada
4 canales sincronizados
MFCC/TFLite actuales
ventana TOA de 100 ms
sin aumentar la cola como solución
sin buffers TOA grandes
```

Antes de pasar a captura indefinida se deberá validar que:

```text
proc_us < 1,000,000
queue_high permanece controlado
errors = 0
localizaciones incoherentes no se publican como válidas
```

El objetivo posterior será evolucionar desde la sesión finita de 16 órdenes hacia captura continua con inicio/STOP controlado y salida de dirección para el subsistema downstream.
