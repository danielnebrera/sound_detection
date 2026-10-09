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

## 39. Próximo experimento: P5-H

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

