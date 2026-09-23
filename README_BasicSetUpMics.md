# BasicSetupMics

## Objetivo

`BasicSetupMics` es una rama/proyecto de diagnóstico mínimo creada para validar la captura I2S de los micrófonos **ICS-43434** con el **STM32H747XI (Arduino Portenta H7)** sin alterar la base estable del proyecto principal.

El objetivo de esta versión es reducir el sistema a una sola línea de recepción SAI y un solo micrófono físico para comprobar de forma independiente:

- generación de BCLK;
- generación de LRCL / WS;
- actividad eléctrica en DOUT;
- selección de slot mediante SEL;
- recepción SAI;
- transferencia mediante DMA;
- conversión PCM;
- transferencia UART;
- generación de JSON;
- conversión posterior a WAV.

La versión de diagnóstico validada actualmente es:

```text
BasicSetupMics P0.2c
Single Mic
SAI2A
PI6 GPIO-IDR + DMA Diagnostic
```

---

## Base del proyecto

Este proyecto de diagnóstico fue creado como una copia independiente de la arquitectura utilizada durante el desarrollo de STM32 + Khamex.

La finalidad es poder modificar y simplificar el firmware sin alterar la versión estable del proyecto principal.

La versión completa original continúa siendo la referencia para la aplicación final de cuatro micrófonos.

---

## Arquitectura actual

En `BasicSetupMics` se utiliza únicamente:

```text
SAI2 Block A
```

como receptor maestro I2S.

### Esquema del montaje validado

![Esquema BasicSetupMics](./Schema_BasicSetupMics.png)

### Conexiones

```text
STM32 / Portenta                ICS-43434
------------------------------------------------
PI5  / SAI2_SCK_A       --->   BCLK / SCK

PI7  / SAI2_FS_A        --->   LRCL / WS / FS

PI6  / SAI2_SD_A        <---   DOUT / SD

3.3 V                   --->   3V

GND                     --->   GND
```

En esta versión:

```text
PG10 / SAI2 Block B = NO UTILIZADO
```

---

## Equivalencias importantes

Las señales I2S utilizadas por el ICS-43434 se corresponden de la siguiente manera:

```text
BCLK = SCK

LRCL = WS = FS

DOUT = SD
```

Por lo tanto:

```text
BCLK != LRCL
```

BCLK y LRCL son dos señales distintas y deben conectarse a pines diferentes del micrófono.

---

## Error de cableado descubierto

Durante las primeras pruebas de `BasicSetupMics`, varios micrófonos parecían no generar datos.

Los síntomas observados eran:

```text
PI6_high  = 0
PI6_trans = 0

raw_nz0 = 0
raw_nz1 = 0

pcm_nz0 = 0
pcm_nz1 = 0
```

También se observaba mediante osciloscopio que DOUT permanecía prácticamente inactivo.

Después de revisar individualmente:

```text
VDD
BCLK
LRCL
DOUT
GPIO PI6
SAI
DMA
```

se encontró que las señales:

```text
BCLK / SCK
LRCL / WS / FS
```

habían sido intercambiadas físicamente durante las pruebas.

La conexión incorrecta era equivalente a:

```text
PI5 / BCLK -> LRCL del mic

PI7 / WS   -> BCLK del mic
```

La conexión correcta es:

```text
PI5 -> BCLK

PI7 -> LRCL

PI6 <- DOUT
```

Una vez corregido el cableado, los micrófonos comenzaron inmediatamente a generar:

```text
actividad digital en PI6
datos RAW
datos PCM
JSON con audio
WAV audible
```

---

## Configuración de SEL

El ICS-43434 utiliza el pin `SEL` para determinar en qué slot del frame I2S transmite sus datos.

En el banco `BasicSetupMics` se verificó experimentalmente:

```text
SEL = GND
    ↓
SLOT0 activo
SLOT1 vacío
```

y:

```text
SEL = VDD
    ↓
SLOT0 vacío
SLOT1 activo
```

Por tanto:

```text
SEL = GND -> SLOT0

SEL = VDD -> SLOT1
```

Este comportamiento fue comprobado con múltiples micrófonos.

---

## Configuración SAI

La configuración utilizada en SAI2 Block A mantiene la estructura utilizada en la arquitectura principal:

```text
Peripheral      : SAI2 Block A
Mode            : MASTER_RX
Synchronization : ASYNCHRONOUS
Protocol        : I2S Standard
Data Size       : 24 bit
Slots           : 2
Sample Rate     : 44.1 kHz
```

El frame mantiene dos slots porque el ICS-43434 selecciona mediante `SEL` cuál de ellos utiliza.

---

## Clock I2S

La frecuencia de muestreo utilizada es:

```text
Fs = 44.1 kHz
```

Cada frame contiene:

```text
2 slots
```

de:

```text
32 clocks por slot
```

por lo que:

```text
64 BCLK por frame
```

La frecuencia esperada de BCLK es aproximadamente:

```text
44,100 Hz × 64
≈ 2.8224 MHz
```

Las mediciones realizadas con osciloscopio son coherentes con:

```text
WS / LRCL ≈ 44.1 kHz

BCLK      ≈ 2.82 MHz
```

---

## DMA

SAI2A utiliza DMA para transferir las muestras recibidas a memoria.

La configuración utilizada es:

```text
Direction        : Peripheral to Memory
Peripheral Inc   : Disabled
Memory Inc       : Enabled
Peripheral Align : Word
Memory Align     : Word
Mode             : Circular
Priority         : Very High
```

El buffer DMA está ubicado en RAM D2:

```c
__attribute__((section(".RAM_D2_bss"), aligned(32)))
```

Ejemplo:

```c
static uint32_t s_dma_raw[DMA_WORDS_TOTAL];
```

---

## Diagnóstico P0.2c

La versión P0.2c incorpora diagnósticos adicionales para comprobar la ruta física y digital antes del procesamiento PCM.

### PINCFG

Se imprime la configuración activa de PI6:

```text
[PINCFG]
```

Ejemplo:

```text
PI6 mode=2
pupd=0
af=10
```

Esto confirma:

```text
PI6 = Alternate Function

AF10 = SAI2

No Pull-Up

No Pull-Down
```

---

## Diagnóstico del buffer DMA

También se imprime:

```text
[DMA_BUF]
```

incluyendo:

```text
cpu_addr
bytes
dcache
dma_m0ar
dma_ndtr
```

Esto permite comprobar que la CPU y DMA están trabajando sobre la misma región de memoria.

Durante las pruebas se verificó:

```text
cpu_addr == dma_m0ar
```

confirmando que DMA escribe exactamente sobre el buffer leído posteriormente por la CPU.

---

## GPIO-IDR

P0.2c incorpora una medición directa de:

```c
GPIOI->IDR
```

durante aproximadamente:

```text
100 ms
```

mientras SAI permanece activo.

Los pines permanecen configurados como AF10.

Se contabilizan:

```text
PI5_high
PI5_low
PI5_trans

PI6_high
PI6_low
PI6_trans

PI7_high
PI7_low
PI7_trans
```

Esto permite observar directamente qué niveles digitales está detectando el STM32.

Las señales corresponden a:

```text
PI5 -> BCLK

PI6 -> DOUT

PI7 -> LRCL / WS
```

---

## Prueba de control con PI6 flotante

Durante el diagnóstico se desconectó físicamente DOUT del STM32 dejando PI6 flotante.

En esa situación se observó:

```text
PI6_high > 0

PI6_low > 0

PI6_trans > 0
```

y SAI/DMA capturó valores RAW no cero.

Esto permitió comprobar que la ruta:

```text
PI6
  ↓
SAI2A
  ↓
DMA
  ↓
RAM
  ↓
PCM
```

era capaz de recibir actividad digital.

---

## Captura de audio

El flujo de P0.2c es:

```text
STM32 inicia
      ↓
UART_INIT_OK
      ↓
[BASIC_READY]
      ↓
Python envía R
      ↓
SAI + DMA comienzan
      ↓
3 s de preroll con clocks activos
      ↓
diagnóstico GPIO-IDR
      ↓
reinicio de contadores
      ↓
captura exacta de 1 segundo
      ↓
44,100 frames
      ↓
transferencia SLOT0
      ↓
transferencia SLOT1
      ↓
[BASIC_DONE]
```

---

## Preroll

Antes de almacenar el segundo útil de audio se mantienen aproximadamente:

```text
3000 ms
```

de clocks activos.

Por tanto, para realizar pruebas acústicas con un tono o dron continuo se recomienda iniciar el sonido antes de lanzar la captura y mantenerlo durante varios segundos.

---

## Formato de captura

Cada ejecución genera:

```text
SLOT0
44100 muestras

SLOT1
44100 muestras
```

con:

```text
Sample Rate = 44100 Hz

Duration = 1 segundo
```

---

## Conversión PCM

Las muestras recibidas desde SAI contienen datos de 24 bits.

La conversión utilizada a PCM16 es:

```c
static inline int16_t pcm24_to_pcm16(uint32_t value)
{
    int32_t sample = (int32_t)(value & 0x00FFFFFFU);

    if ((sample & 0x00800000L) != 0)
        sample |= (int32_t)0xFF000000U;

    return (int16_t)(sample >> 8);
}
```

Esto:

```text
extrae 24 bits
extiende el signo
desplaza 8 bits
genera PCM16
```

---

## Estadísticas RAW y PCM

Al finalizar cada captura se imprime:

```text
[CAPTURE_DONE]
```

junto con:

```text
frames
elapsed_ms
events

raw_nz0
raw_nz1

raw_or0
raw_or1

pcm_nz0
pcm_nz1
```

Para un micrófono conectado con:

```text
SEL = VDD
```

el comportamiento esperado es:

```text
raw_nz0 = 0

raw_nz1 ≈ 44100
```

Para:

```text
SEL = GND
```

se espera:

```text
raw_nz0 ≈ 44100

raw_nz1 = 0
```

---

## UART

La comunicación con el PC utiliza:

```text
USART1
1,000,000 baud
```

El protocolo básico es:

```text
[BASIC_READY]

R

[CAPTURE_STARTED]

[PINCFG]

[DMA_BUF]

[GPIO_IDR]

[CAPTURE_DONE]

[SLOT0_BIN]
<88200 bytes PCM16>
[SLOT0_END]

[SLOT1_BIN]
<88200 bytes PCM16>
[SLOT1_END]

[BASIC_DONE]
```

Cada slot contiene:

```text
44100 muestras × 2 bytes
=
88200 bytes
```

---

## Script `grabar_sesion_basico.py`

El receptor de PC utilizado durante estas pruebas es:

```text
grabar_sesion_basico.py
```

Sus responsabilidades son:

```text
Abrir COM

Trabajar a 1,000,000 baud

Esperar [BASIC_READY]

Enviar R

Recibir SLOT0

Recibir SLOT1

Convertir PCM16

Generar JSON

Generar metadata
```

Ejemplo:

```powershell
python grabar_sesion_basico.py --port COM6 --session 1028 --order 1
```

Los archivos generados se almacenan en:

```text
json_test/sound/
```

Ejemplo:

```text
sound_s1028_ord1_slot0.json

sound_s1028_ord1_slot1.json
```

---

## Script `json_to_wav_basico.py`

Para comprobar auditivamente las muestras se utiliza:

```text
json_to_wav_basico.py
```

Ejemplo:

```powershell
python json_to_wav_basico.py --session 1028
```

La salida se almacena en:

```text
wavs/session_1028/
```

generando:

```text
slot0.wav

slot1.wav
```

según los nombres asociados a la sesión.

---

## Medición dBFS

Durante las pruebas también se calculó el nivel RMS de cada captura.

La fórmula utilizada es:

```text
RMS = sqrt(sum(x²) / N)
```

y:

```text
dBFS = 20 × log10(RMS)
```

Esto permitió comparar el nivel acústico producido por los diferentes micrófonos bajo pruebas similares.

---

## Resultados de los micrófonos

Las pruebas completas de los micrófonos se encuentran documentadas en:

[**BasicSetUpMicsResults.xlsx**](./BasicSetUpMicsResults.xlsx)

El archivo contiene los resultados individuales obtenidos variando:

```text
Micrófono

SEL

Slot

dBFS

Montaje
```

### Resumen

Durante esta fase se probaron siete micrófonos.

Se comprobó que:

```text
MIC2 -> funcional

MIC3 -> funcional

MIC4 -> funcional

MIC5 -> funcional

MIC6 -> funcional

MIC7 -> funcional
```

Un micrófono presentó:

```text
I2S válido
SEL válido
slots válidos
```

pero un nivel acústico extremadamente bajo:

```text
aprox. -71 dBFS
```

por lo que queda marcado como:

```text
I2S PASS

Audio útil FAIL / sospechoso
```

---

## Micrófonos del montaje nuevo

Tres micrófonos pertenecientes al montaje nuevo habían sido probados inicialmente mientras BCLK y LRCL estaban intercambiados.

Por este motivo aquellas pruebas anteriores no se consideran válidas.

Después de corregir el cableado fueron probados nuevamente de forma individual.

Los tres entregaron:

```text
DOUT válido

SLOT0 válido con SEL=GND

SLOT1 válido con SEL=VDD

audio audible

niveles dBFS adecuados
```

Por tanto, los tres micrófonos nuevos quedan considerados funcionales.

---

## Funcionalidad retirada o deshabilitada temporalmente

`BasicSetupMics` no intenta ejecutar toda la arquitectura completa del proyecto.

Durante esta etapa se retiró del flujo de ejecución todo aquello que no era necesario para validar la recepción física de un micrófono.

No participa actualmente:

```text
SAI2 Block B

PG10 / SD_B

captura de cuatro micrófonos

mapeo LEFT / TOP / BACK / RIGHT

captura continua completa

SDRAM para sesiones largas

MFCC

TensorFlow Lite

modelo de detección

probabilidades

detecciones

pipeline Khamex

dispatcher

persistor

servidor

interfaz web
```

Estos componentes no se consideran eliminados del proyecto.

Simplemente se encuentran fuera del banco básico para evitar introducir variables adicionales durante el diagnóstico.

---

## Elementos conservados

Se mantuvieron los componentes esenciales relacionados directamente con la adquisición de audio:

```text
STM32H747XI

SAI2

SAI2 Block A

PI5 BCLK

PI7 LRCL

PI6 DOUT

I2S

24 bits

2 slots

44.1 kHz

DMA

RAM D2

PCM16

UART 1,000,000 baud

JSON

WAV
```

---

## Resultado final de BasicSetupMics

La arquitectura mínima queda validada con:

```text
Firmware
BasicSetupMics P0.2c

SAI
SAI2A

BCLK
PI5

LRCL / WS / FS
PI7

DOUT
PI6

Sample Rate
44100 Hz

Slots
2

SEL = GND
SLOT0

SEL = VDD
SLOT1
```

La cadena validada es:

```text
ICS-43434
    ↓
DOUT
    ↓
PI6
    ↓
SAI2A
    ↓
DMA
    ↓
RAM D2
    ↓
PCM24 -> PCM16
    ↓
UART
    ↓
Python
    ↓
JSON
    ↓
WAV
    ↓
Audio audible
```

---

## Checkpoint

Esta versión debe mantenerse como checkpoint funcional para:

```text
1 micrófono
+
1 línea SAI
+
2 slots
```

No se recomienda modificar esta versión durante el desarrollo de la siguiente etapa.

---

# Siguiente etapa: BasicSetupMicsPairs

El desarrollo de dos micrófonos simultáneos se realizará en una rama independiente:

```text
BasicSetupMicsPairs
```

El objetivo inicial será utilizar:

```text
2 × ICS-43434
```

compartiendo:

```text
BCLK

LRCL

DOUT
```

con:

```text
MIC A
SEL = GND
-> SLOT0

MIC B
SEL = VDD
-> SLOT1
```

La entrada seguirá siendo inicialmente:

```text
SAI2A
PI6
```

sin utilizar todavía:

```text
SAI2B
PG10
```

El objetivo será comprobar primero:

```text
SLOT0 + SLOT1 simultáneamente
```

antes de avanzar nuevamente hacia una arquitectura de cuatro micrófonos.

---

## Estado

```text
BasicSetupMics
Single Mic
SAI2A
44.1 kHz
24 bit
2 Slots
DMA
UART
JSON
WAV

STATUS: VALIDATED
```