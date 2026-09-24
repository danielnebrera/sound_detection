# BasicSetUpMicsPairs

## Objetivo

`BasicSetUpMicsPairs` es una evolución directa del proyecto `BasicSetUpMics`.

El objetivo de esta etapa es validar que **dos micrófonos digitales ICS-43434 puedan compartir una misma línea de datos I2S**, utilizando:

- BCLK compartido
- LRCL / WS compartido
- DOUT compartido
- un micrófono con `SEL = GND`
- un micrófono con `SEL = VDD`

y comprobar que el STM32H747XI puede recibir simultáneamente ambos micrófonos mediante:

```text
SAI2 Block A
PI6 / SAI2_SD_A
```

separando correctamente:

```text
SLOT0
SLOT1
```

Esta etapa parte del banco previamente validado `BasicSetUpMics`, donde se demostró individualmente que los micrófonos funcionales:

- generan audio correctamente;
- transmiten por DOUT;
- pueden utilizar SLOT0 o SLOT1;
- cambian correctamente de slot mediante el pin SEL.

El propósito de `BasicSetUpMicsPairs` es comprobar ahora la operación simultánea de **dos micrófonos sobre una única línea DOUT**.

---

# Arquitectura del proyecto

El proyecto continúa utilizando únicamente:

```text
SAI2 Block A
```

No se utiliza todavía:

```text
SAI2 Block B
PG10 / SAI2_SD_B
```

La arquitectura actual es:

```text
MIC A ----+
          |
          +---- DOUT compartido ----> PI6 / SAI2_SD_A
          |
MIC B ----+

MIC A BCLK --------------------------> PI5 / SAI2_SCK_A
MIC B BCLK --------------------------> PI5 / SAI2_SCK_A

MIC A LRCL --------------------------> PI7 / SAI2_FS_A
MIC B LRCL --------------------------> PI7 / SAI2_FS_A
```

Los dos micrófonos utilizan estados SEL opuestos.

Ejemplo:

```text
MIC A
SEL = GND
-> SLOT0

MIC B
SEL = VDD
-> SLOT1
```

---

# Esquema del montaje

El esquema físico utilizado para esta etapa se encuentra en:

![Esquema BasicSetUpMicsPairs](./Schema%20version%20BasicSetUpMicsPairs.png)

La conexión validada es:

```text
STM32 / Portenta                  MIC A                    MIC B
---------------------------------------------------------------------

PI5 / SAI2_SCK_A  ----------->   BCLK                     BCLK

PI7 / SAI2_FS_A   ----------->   LRCL / WS                LRCL / WS

PI6 / SAI2_SD_A   <-----------   DOUT --------+
                                               |
                                  DOUT --------+

3.3 V             ----------->   3V                       3V

GND               ----------->   GND                      GND
```

Los dos DOUT se unen físicamente y terminan en:

```text
PI6 / SAI2_SD_A
```

---

# Equivalencias importantes

En el ICS-43434:

```text
BCLK = SCK

LRCL = WS = FS

DOUT = SD
```

Por lo tanto:

```text
BCLK != LRCL
```

Estas señales no deben intercambiarse.

Durante la etapa anterior se descubrió que una conexión incorrecta entre BCLK y LRCL provocaba que los micrófonos no generaran una salida DOUT válida.

La conexión correcta y actualmente validada es:

```text
PI5 -> BCLK

PI7 -> LRCL / WS

PI6 <- DOUT
```

---

# Selección de slot mediante SEL

Las pruebas individuales realizadas previamente demostraron:

```text
SEL = GND
-> SLOT0
```

y:

```text
SEL = VDD
-> SLOT1
```

En `BasicSetUpMicsPairs` se aprovecha esta característica para conectar dos micrófonos a la misma línea DOUT.

Ejemplo utilizado durante las pruebas principales:

```text
MIC5
SEL = GND
-> SLOT0

MIC6
SEL = VDD
-> SLOT1
```

También se realizó la configuración inversa:

```text
MIC5
SEL = VDD
-> SLOT1

MIC6
SEL = GND
-> SLOT0
```

Ambas configuraciones funcionaron correctamente.

---

# Configuración SAI

La configuración utilizada permanece basada en el banco validado anteriormente.

```text
Peripheral      : SAI2 Block A
Mode            : MASTER_RX
Synchronization : ASYNCHRONOUS
Protocol        : I2S Standard
Data Size       : 24 bit
Slots           : 2
Sample Rate     : 44.1 kHz
```

La inicialización utiliza:

```c
HAL_SAI_InitProtocol(
    &hsai_BlockA2,
    SAI_I2S_STANDARD,
    SAI_PROTOCOL_DATASIZE_24BIT,
    2U
);
```

Los dos slots son recibidos de forma intercalada mediante la misma entrada:

```text
PI6 / SAI2_SD_A
```

Conceptualmente, DMA recibe:

```text
word 0 -> SLOT0
word 1 -> SLOT1
word 2 -> SLOT0
word 3 -> SLOT1
...
```

---

# Clock I2S

La frecuencia de muestreo es:

```text
Fs = 44.1 kHz
```

Cada frame contiene:

```text
2 slots
```

con:

```text
32 BCLK por slot
```

por tanto:

```text
64 BCLK por frame
```

La frecuencia aproximada de BCLK es:

```text
44100 × 64
=
2.8224 MHz
```

Los valores observados durante las pruebas son coherentes con:

```text
LRCL / WS ≈ 44.1 kHz

BCLK      ≈ 2.82 MHz
```

---

# DMA

SAI2A continúa utilizando DMA circular.

Configuración:

```text
Direction        : Peripheral to Memory
Peripheral Inc   : Disabled
Memory Inc       : Enabled
Peripheral Align : Word
Memory Align     : Word
Mode             : Circular
Priority         : Very High
```

El buffer se mantiene en RAM D2:

```c
__attribute__((section(".RAM_D2_bss"), aligned(32)))
```

Durante las pruebas se confirmó que:

```text
cpu_addr == dma_m0ar
```

por lo que DMA escribe exactamente en el buffer posteriormente procesado por CPU.

---

# Firmware utilizado

La primera etapa de `BasicSetUpMicsPairs` utiliza prácticamente el mismo firmware validado en `BasicSetUpMics`.

Versión de diagnóstico:

```text
BasicSetupMics P0.2c
SAI2A
PI6 GPIO-IDR
DMA diagnostic
```

Aunque algunos mensajes UART todavía contienen textos heredados como:

```text
single-mic
```

o:

```text
Micros fisicos : 1
```

estos textos no representan una limitación funcional.

El firmware recibe actualmente:

```text
SLOT0
+
SLOT1
```

simultáneamente.

Las pruebas realizadas en este proyecto demostraron que ambos slots pueden contener audio proveniente de dos micrófonos físicos distintos conectados sobre el mismo DOUT.

---

# Cómo compilar el proyecto

## 1. Abrir STM32CubeIDE

Abrir el workspace utilizado durante el desarrollo.

Dentro del workspace se encuentran distintas versiones del proyecto.

Se recomienda mantener cerrados los proyectos de referencia:

```text
v2.1.7 original

BasicSetUpMics
```

y mantener abierto únicamente:

```text
BasicSetUpMicsPairs
BasicSetUpMicsPairs_CM7
BasicSetUpMicsPairs_CM4
```

El desarrollo actual utiliza únicamente CM7.

---

## 2. Seleccionar el proyecto CM7

El proyecto que debe compilarse es:

```text
BasicSetUpMicsPairs_CM7
```

No es necesario modificar CM4 durante esta etapa.

---

## 3. Realizar Build

En STM32CubeIDE:

```text
Right Click sobre BasicSetUpMicsPairs_CM7
-> Build Project
```

o utilizar el botón de Build del IDE.

El enlace debe utilizar el linker script perteneciente al propio proyecto:

```text
BasicSetUpMicsPairs\CM7\STM32H747XIHX_FLASH.ld
```

Una compilación correcta genera:

```text
BasicSetUpMicsPairs_CM7.elf
BasicSetUpMicsPairs_CM7.map
BasicSetUpMicsPairs_CM7.list
```

---

# Warnings conocidos durante Build

STM32CubeIDE actualmente muestra mensajes relacionados con `newlib-nano`:

```text
_close
_fstat
_getpid
_isatty
_kill
_lseek
_read
_write
```

Ejemplo:

```text
warning: _write is not implemented and will always fail
```

Estos mensajes ya estaban presentes en `BasicSetUpMics`.

Aunque CubeIDE pueda mostrar:

```text
Build Failed. 8 errors, 8 warnings
```

el firmware sí genera correctamente:

```text
BasicSetUpMicsPairs_CM7.elf
```

y posteriormente ejecuta:

```text
arm-none-eabi-size
arm-none-eabi-objdump
```

Por tanto, durante esta fase no se están tratando estos mensajes como un fallo funcional del firmware.

No se recomienda modificar el proyecto únicamente para eliminar estos warnings mientras el ELF se genere correctamente.

---

# Cómo flashear

Después de compilar:

1. Conectar la Portenta H7 al PC.
2. Verificar que `BasicSetUpMicsPairs_CM7` es el proyecto activo.
3. Utilizar la configuración habitual de STM32CubeIDE para programar CM7.
4. Flashear el archivo:

```text
BasicSetUpMicsPairs_CM7.elf
```

5. Reiniciar la placa si es necesario.
6. Mantener la Portenta conectada por USB para la comunicación UART.

La UART utilizada por los scripts trabaja a:

```text
1,000,000 baud
```

En las pruebas realizadas:

```text
COM6
```

fue el puerto utilizado.

El número de COM puede cambiar según el equipo.

---

# Secuencia correcta de ejecución

## Paso 1 - Apagar el montaje antes de modificar cables

Antes de modificar:

```text
SEL
DOUT
BCLK
LRCL
3V
GND
```

apagar la alimentación del montaje.

---

## Paso 2 - Revisar conexión del par

Ejemplo validado:

```text
MIC5
SEL = GND

MIC6
SEL = VDD
```

Ambos comparten:

```text
BCLK
LRCL
DOUT
3.3V
GND
```

La línea DOUT común termina en:

```text
PI6
```

---

## Paso 3 - Encender y conectar la Portenta

Después de revisar físicamente el montaje:

1. alimentar los micrófonos;
2. conectar la Portenta;
3. verificar que aparece el puerto UART;
4. ejecutar el script de captura.

---

# Script de captura

El script utilizado es:

```text
grabar_sesion_basico.py
```

Ejemplo:

```powershell
python grabar_sesion_basico.py --port COM6 --session 1034 --order 1
```

El script:

1. abre UART;
2. espera `[BASIC_READY]`;
3. envía `R`;
4. espera la captura;
5. recibe SLOT0;
6. recibe SLOT1;
7. genera un JSON para cada slot;
8. genera metadata de la sesión.

---

# Preroll

Después de recibir `R`, el STM32 mantiene aproximadamente:

```text
3000 ms
```

de clocks activos antes de almacenar el segundo útil.

Por esta razón, cuando se utiliza un dron o tono continuo para las pruebas:

1. iniciar el sonido antes de lanzar la captura;
2. mantenerlo durante al menos 5 o 6 segundos;
3. no utilizar únicamente una palmada corta.

La captura almacenada tiene una duración exacta aproximada de:

```text
1 segundo
```

y contiene:

```text
44100 muestras por slot
```

---

# Salida esperada del STM32

Durante una captura correcta con dos micrófonos se espera:

```text
[GPIO_IDR]
```

con actividad sobre PI6.

Y posteriormente:

```text
[CAPTURE_DONE]
```

con:

```text
raw_nz0 ≈ 44100
raw_nz1 ≈ 44100

pcm_nz0 ≈ 44100
pcm_nz1 ≈ 44100
```

Esto indica que ambos slots contienen datos.

---

# Archivos JSON generados

Ejemplo para la sesión 1034:

```text
json_test/sound/sound_s1034_ord1_slot0.json

json_test/sound/sound_s1034_ord1_slot1.json
```

Cada archivo contiene:

```text
44100 muestras
```

normalizadas.

---

# Medición de RMS y dBFS

Para analizar simultáneamente ambos slots se puede utilizar:

```powershell
python -c "import json,math; files=[r'json_test\sound\sound_s1034_ord1_slot0.json',r'json_test\sound\sound_s1034_ord1_slot1.json']; [(lambda p,x: print(p,'samples=',len(x),'min=',min(x),'max=',max(x),'peak=',max(abs(v) for v in x),'rms=',(r:=math.sqrt(sum(v*v for v in x)/len(x))),'rms_dbfs=',20*math.log10(r) if r>0 else '-inf'))(p,json.load(open(p))) for p in files]"
```

Esto imprime:

```text
samples
min
max
peak
rms
rms_dbfs
```

para cada slot.

---

# Comparación entre SLOT0 y SLOT1

Para comprobar que ambos slots no contienen una copia digital del mismo audio se utilizó:

```powershell
python -c "import json,math; a=json.load(open(r'json_test\sound\sound_s1034_ord1_slot0.json')); b=json.load(open(r'json_test\sound\sound_s1034_ord1_slot1.json')); n=len(a); ma=sum(a)/n; mb=sum(b)/n; num=sum((x-ma)*(y-mb) for x,y in zip(a,b)); da=math.sqrt(sum((x-ma)**2 for x in a)); db=math.sqrt(sum((y-mb)**2 for y in b)); corr=num/(da*db) if da and db else 0; equal=sum(x==y for x,y in zip(a,b)); diff=math.sqrt(sum((x-y)**2 for x,y in zip(a,b))/n); print('samples=',n); print('exact_equal=',equal); print('equal_pct=',100*equal/n); print('correlation=',corr); print('rms_difference=',diff)"
```

Los campos utilizados son:

```text
exact_equal
equal_pct
correlation
rms_difference
```

Si ambos canales fueran una copia digital directa se esperaría aproximadamente:

```text
equal_pct ≈ 100 %
correlation ≈ 1
rms_difference ≈ 0
```

Esto no ocurrió durante las pruebas.

---

# Conversión JSON a WAV

Después de generar los JSON se utiliza:

```text
json_to_wav_basico.py
```

Ejemplo:

```powershell
python json_to_wav_basico.py --session 1034
```

El script genera:

```text
slot0 WAV
slot1 WAV
```

permitiendo escuchar ambos canales de forma independiente.

La salida se almacena en:

```text
wavs/session_1034/
```

Ambos WAV fueron comprobados auditivamente durante las pruebas de funcionamiento.

---

# Captura Hantek LRCL + DOUT

La medición de referencia utilizada para documentar el bus compartido es:

![LRCL y DOUT compartido](./CH1LRCL-CH2DOUT.png)

Configuración utilizada:

```text
CH1 -> LRCL / WS común

CH2 -> DOUT común

Time/Div = 5 us/div

Sample Rate = 16 MHz

CH1 = 1.00 V/div

CH2 = 1.00 V/div

DC coupling

Probe físico = 10X

Software = x10

Trigger Source = CH1

Trigger Slope = +

Trigger Level ≈ 1.6 V
```

---

# Interpretación de LRCL

La señal amarilla:

```text
CH1
```

corresponde a:

```text
LRCL / WS
```

Se observa una señal periódica aproximadamente cuadrada.

El STM32 registró:

```text
PI7_trans ≈ 8801
```

durante:

```text
100 ms
```

Esto corresponde aproximadamente a:

```text
8801 / 2 / 0.1
≈ 44005 Hz
```

lo cual es coherente con:

```text
44.1 kHz
```

La señal LRCL queda por tanto validada.

---

# Interpretación de DOUT

La señal verde:

```text
CH2
```

corresponde a:

```text
DOUT compartido
```

La captura muestra actividad digital durante ambas mitades del frame delimitado por LRCL.

Esto es consistente con:

```text
SLOT0 -> un micrófono

SLOT1 -> segundo micrófono
```

ambos transmitiendo sobre la misma línea física DOUT.

El STM32 confirma este comportamiento al obtener simultáneamente:

```text
raw_nz0 ≈ 44100

raw_nz1 ≈ 44100
```

---

# Interferencia observada en DOUT

La forma de onda DOUT no aparece completamente limpia.

Se observa:

```text
ruido
rugosidad
ringing / interferencia
```

sobre las transiciones digitales.

Esta interferencia ya había sido observada parcialmente durante etapas anteriores del proyecto.

Actualmente no se considera completamente resuelta.

---

# Posibles causas de la interferencia

Entre las posibles causas se mantienen abiertas:

```text
cableado provisional

bifurcaciones físicas

impedancia de la línea

retornos de tierra

proximidad entre conductores

ringing producido por flancos rápidos

ausencia de optimización del layout

montaje mediante jumpers / cables temporales
```

Los cables utilizados en `BasicSetUpMicsPairs` fueron deliberadamente cortos para reducir:

```text
longitud de línea

acoplamiento

ruido

interferencia
```

pero la forma de onda aún presenta imperfecciones visibles.

---

# Importante sobre la interferencia

La interferencia observada no impidió obtener:

```text
dos slots activos

audio audible en ambos slots

RAW válido en ambos slots

PCM válido en ambos slots

JSON independiente

WAV independiente
```

Por tanto, actualmente se clasifica como:

```text
OBSERVACIÓN / PENDIENTE DE OPTIMIZACIÓN
```

y no como:

```text
FAIL
```

de la arquitectura de pares.

---

# Resultados experimentales

## Sesión 1032

Configuración:

```text
MIC5
SEL = VDD
-> SLOT1

MIC6
SEL = GND
-> SLOT0
```

Resultados:

```text
SLOT0

samples = 44100
RMS = 0.010494090498838982
dBFS = -39.58110389713721
```

```text
SLOT1

samples = 44100
RMS = 0.010630005579301414
dBFS = -39.46933015062686
```

Comparación:

```text
exact_equal = 37

equal_pct = 0.08390022675736962 %

correlation = 0.1702787298499451

rms_difference = 0.01360635608748369
```

Resultado:

```text
PASS
```

---

# Sesión 1033

Se invirtió únicamente SEL.

Configuración:

```text
MIC5
SEL = GND
-> SLOT0

MIC6
SEL = VDD
-> SLOT1
```

Resultados:

```text
SLOT0

RMS = 0.011327264128756898
dBFS = -38.917499450149855
```

```text
SLOT1

RMS = 0.010507329258770178
dBFS = -39.57015316863736
```

Comparación:

```text
exact_equal = 45

equal_pct = 0.10204081632653061 %

correlation = 0.3550662760130734

rms_difference = 0.01241736995890805
```

Resultado:

```text
PASS
```

Esta prueba confirmó que:

```text
el slot sigue a SEL
```

y no a un micrófono físico específico.

---

# Sesión 1034 - referencia final

Configuración:

```text
MIC5
SEL = GND
-> SLOT0

MIC6
SEL = VDD
-> SLOT1
```

Resultado STM32:

```text
raw_nz0 = 44100
raw_nz1 = 44100

raw_or0 = 00FFFFFE
raw_or1 = 00FFFFFE

pcm_nz0 = 44066
pcm_nz1 = 44061
```

SLOT0:

```text
min  = -0.062713623046875

max  = 0.05572509765625

peak = 0.062713623046875

rms  = 0.01360993151184579

dBFS = -37.32288120512109
```

SLOT1:

```text
min  = -0.056304931640625

max  = 0.06158447265625

peak = 0.06158447265625

rms  = 0.012559465056942755

dBFS = -38.020577160647576
```

Comparación entre ambos canales:

```text
samples = 44100

exact_equal = 36

equal_pct = 0.08163265306122448 %

correlation = 0.38952116546252347

rms_difference = 0.014484682008131439
```

Resultado:

```text
PASS
```

---

# Interpretación de la correlación

Los dos micrófonos se encuentran físicamente próximos y reciben la misma fuente acústica.

Por esta razón es normal observar cierta correlación entre ambos canales.

Sin embargo:

```text
correlation != 1
```

y únicamente:

```text
36 muestras de 44100
```

fueron exactamente iguales durante la sesión 1034.

Esto representa aproximadamente:

```text
0.0816 %
```

de las muestras.

Por tanto:

```text
SLOT0 y SLOT1 NO son copias digitales uno del otro.
```

Cada slot contiene una señal independiente.

---

# Prueba inicial anómala

Durante una prueba anterior se observó temporalmente:

```text
PI6_trans = 368194

raw_nz0 = 0

raw_nz1 = 0
```

Este comportamiento no volvió a repetirse en las capturas posteriores.

Las sesiones 1032, 1033 y 1034 mostraron:

```text
PI6_trans ≈ 80000
```

junto con datos válidos en ambos slots.

Actualmente esta primera captura se considera una condición transitoria del montaje físico o del contacto eléctrico durante la preparación inicial del par.

No se utilizó como resultado válido de la arquitectura.

---

# Observación térmica

Durante algunas pruebas se observó que uno de los módulos parecía presentar una temperatura ligeramente superior al otro en la zona central correspondiente al encapsulado del micrófono.

No se observó una temperatura extrema ni un fallo posterior del dispositivo.

El micrófono había sido previamente validado de forma individual y continuó funcionando correctamente durante las pruebas posteriores de pares.

Esta observación queda registrada como:

```text
PENDIENTE DE MONITOREO
```

No se considera actualmente evidencia suficiente para diagnosticar un fallo del micrófono.

---

# Validación obtenida

Las pruebas realizadas permiten marcar como validados:

```text
2 micrófonos físicos                         PASS

BCLK compartido                              PASS

LRCL / WS compartido                         PASS

DOUT compartido                              PASS

SAI2A                                        PASS

PI6 como única entrada de datos              PASS

SEL = GND -> SLOT0                           PASS

SEL = VDD -> SLOT1                           PASS

SLOT0 + SLOT1 simultáneos                    PASS

Cambio de SEL entre micrófonos               PASS

DMA con dos slots simultáneos                PASS

PCM de ambos slots                           PASS

JSON independiente por slot                  PASS

WAV independiente por slot                   PASS

Audio audible en ambos canales               PASS

Datos no duplicados                          PASS

Captura Hantek LRCL + DOUT                   PASS
```

Permanece pendiente:

```text
optimización de integridad de señal / ringing / interferencia
```

---

# Conclusión

`BasicSetUpMicsPairs` demuestra correctamente que **dos micrófonos ICS-43434 pueden compartir una única línea de datos DOUT** siempre que utilicen estados SEL opuestos.

La arquitectura validada es:

```text
MIC A
SEL = GND
      |
      +---- SLOT0 ----+
                      |
                      +---- PI6 / SAI2A
                      |
MIC B                 |
SEL = VDD             |
      |               |
      +---- SLOT1 ----+
```

Los dos micrófonos comparten:

```text
BCLK
LRCL
DOUT
VDD
GND
```

y el STM32 separa correctamente sus datos en:

```text
SLOT0
SLOT1
```

Las pruebas con inversión de SEL confirmaron además que:

```text
el slot depende de SEL
```

y no de la identidad física del micrófono.

Los análisis numéricos confirmaron que:

```text
SLOT0 != SLOT1
```

y que ambos contienen señales acústicas diferentes.

Por tanto, el objetivo principal de esta etapa queda cumplido.

---

# Estado del proyecto

```text
PROJECT
BasicSetUpMicsPairs

MICROCONTROLLER
STM32H747XI / Portenta H7

ACTIVE CORE
CM7

SAI
SAI2 Block A

DATA LINE
PI6 / SAI2_SD_A

BCLK
PI5 / SAI2_SCK_A

LRCL
PI7 / SAI2_FS_A

SAMPLE RATE
44100 Hz

DATA SIZE
24 bit

SLOTS
2

PHYSICAL MICROPHONES
2

MIC A
SEL = GND
-> SLOT0

MIC B
SEL = VDD
-> SLOT1

DMA
Circular

UART
1000000 baud

OUTPUT
PCM16
JSON
WAV
```

Estado:

```text
BasicSetUpMicsPairs

2 MIC
1 DOUT
2 SLOTS

STATUS: VALIDATED
```

---

# Próxima etapa

Después de validar una pareja sobre:

```text
PI6 / SAI2A
```

la siguiente arquitectura a evaluar será:

```text
PAIR A
2 micrófonos
-> PI6 / SAI2A

PAIR B
2 micrófonos
-> PG10 / SAI2B
```

para alcanzar nuevamente:

```text
4 micrófonos físicos
```

manteniendo:

```text
BCLK compartido

LRCL compartido

2 micrófonos por línea DOUT
```

Antes de incorporar:

```text
SDRAM
captura continua
MFCC
TFLite
Khamex
```

se deberá validar primero eléctricamente y mediante WAV que:

```text
PI6 -> SLOT0 + SLOT1

PG10 -> SLOT0 + SLOT1
```

funcionen simultáneamente y mantengan cuatro señales independientes.

---