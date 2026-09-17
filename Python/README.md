# Detector de drones — versión Windows

Port a Python del detector que corre en la Portenta H7 (`CM7/Core/Src/drone_detection.c`
+ `mfcc_stm32.c` + X-CUBE-AI). Usa el **mismo modelo** que el firmware: el `.tflite`
extraído del array C de `CM7/X-CUBE-AI/App/network.c`.

Sirve para probar el detector con el micrófono del portátil, con WAVs, y para
averiguar qué preprocesado espera realmente el modelo.

## Instalación

```
pip install -r requirements.txt
```

`scipy` es opcional: solo mejora la calidad del reamostrado cuando un WAV no está
a 44.1 kHz. Sin él se usa interpolación lineal (y se avisa por pantalla).

Si hace falta regenerar el modelo desde el firmware:

```
python tools/extract_tflite.py        # network.c -> drone_mfcc_model.tflite
```

## Arranque en 3 pasos

Desde una consola en esta carpeta (`Python/`):

```
cd C:\Users\danie\OneDrive\Documentos\khamex\sound_detection\Python

python drone_detector_cli.py probe                 # 1. ¿qué micro uso?
python drone_detector_cli.py calibrate --device 12  # 2. ¿qué ganancia?
python drone_detector_cli.py mic --device 12 --gain 1.3   # 3. detectar
```

Cada paso te imprime el comando del siguiente ya montado, con el dispositivo y
la ganancia puestos. Para la versión con gráficas, mismo paso 3 pero con
`drone_detector_gui.py`. Se sale con `Ctrl+C` (o cerrando la ventana).

Repite el paso 2 si cambias de sitio o de micrófono: la ganancia depende del
ruido de fondo que haya.

## Uso

```
python drone_detector_cli.py list-devices          # ver entradas de audio
python drone_detector_cli.py probe                 # medir cuál entrega más señal
python drone_detector_cli.py calibrate             # medir tu micro y sugerir --gain
python drone_detector_cli.py mic --gain 34.6       # detección en vivo (consola)
python drone_detector_gui.py  --gain 34.6          # detección en vivo (gráficas)
python drone_detector_cli.py wav audio.wav         # procesar un fichero
python drone_detector_cli.py record --label dron   # grabar dataset etiquetado
python drone_detector_cli.py compare audio.wav --gains 1 10 30
```

La salida imita la que la Portenta imprime por UART:

```
Mic1:  -12dB p=0.14 | EMA:0.09  [RASTREANDO 1/3]
Mic1:  -11dB p=0.87 | EMA:0.60  >>> ALERTA ROJA: DRON DETECTADO (60%)
```

## Latencia y coste

Medido en el portátil, con el análisis corriendo en su propio hilo:

| | antes | ahora |
|---|---|---|
| buffer del dispositivo | 171 ms (bloque 4096) | **43 ms** (bloque 1024) |
| espera a llenar la ventana | 1000 ms | **500 ms** (ventana deslizante) |
| análisis | 60 ms | 47 ms |
| espera al refresco | hasta 1000 ms | hasta **500 ms** |
| cola de audio acumulada | hasta **9 bloques** | 0–1 |

La ventana de análisis **sigue siendo de 1 segundo** (el modelo y H la necesitan),
pero se desliza cada `--hop` segundos en vez de avanzar entera. Con `--hop 0.5`
las ventanas se solapan al 50 %: el mismo contexto, el doble de refrescos.

Dónde estaba el problema real: el audio nunca fue el cuello de botella (47 ms de
cada 1000), y el `update()` de la ventana tampoco (5 ms). El coste es
`canvas.draw()`, unos 165 ms, y se estaba llamando **4 veces por segundo cuando
los datos solo cambian una** (o dos). El refresco va ahora al ritmo de `--hop`.

Ese draw está repartido entre los cinco paneles (35 ms el espectrograma, 20-25 ms
cada uno de los demás), no hay un culpable único: es el motor de matplotlib
repintando ejes, textos y barra de color. Si quieres menos CPU, sube `--refresh`.

### Que no se haga bola

El buffer de captura tiene **tope duro** (`--max-lag`, 1 s por defecto) y la cola
del callback está **acotada**. Cuando el análisis se retrasa:

- el callback descarta el bloque más antiguo en vez de dejar crecer la cola
- si caben varias ventanas pendientes, se analiza **solo la más reciente** y las
  intermedias se saltan

Sin esto el buffer crecía sin límite y cada `concatenate` costaba más, con lo que
se retrasaba todavía más: espiral de muerte que acababa colgando la ventana.

Verificado con un consumidor artificialmente lento (1.2 s por ventana, 2.4× más
lento que tiempo real): el intervalo se queda **plano en 1.20 s**, la cola topada,
y va descartando. En marcha normal, 45 s seguidos con la cola en 0 y 0 ventanas
saltadas.

La ventana también actualiza los vértices del relleno en vez de borrar y recrear
la colección en cada refresco — eran miles de artistas creados y destruidos en
una sesión larga. El `update` bajó de 5 a 3 ms.

**Latencia real medida** reproduciendo un pitido y viendo cuándo aparece: el
nivel sube a los **1.16 s** y llega al pico a los 1.65 s. La ventana de análisis
es de 1 s, así que por debajo de eso no se puede bajar sin cambiar el modelo.

`scipy` deja de ser opcional en la práctica: el filtro paso alto pasa de 11.7 ms
a 0.2 ms por segundo de audio, y el reamostrado sube de calidad.

## La ventana

Cinco pestañas, y solo se repinta la que está visible:

| pestaña | qué muestra |
|---|---|
| **Detección** | P del modelo y H del peine, con sus medidores y umbrales, más la forma de onda |
| **Espectro** | espectro medio del segundo y espectrograma, con los picos marcados |
| **MFCC** | bandas mel y la matriz 100×20 exacta que recibe la red |
| **Armónicos** | residuo espectral (espectro menos su fondo) con los armónicos que sostienen H |
| **Workflow** | una caja por paso del algoritmo, con su valor en vivo y su minigráfica |

Coste por refresco: 43 ms la de Armónicos, 89 ms la de Workflow, 92 ms la de
Espectro. Antes se pintaban los cinco paneles juntos en cada refresco, 165 ms
siempre.

### La pestaña Workflow

**Una caja por paso del algoritmo**, con sus parámetros fijos y su valor en vivo:

```
                 1 CAPTURA  →  2 PASO ALTO
                        ┌───────┴───────┐
        rama del MODELO │               │ rama del PEINE ARMÓNICO
                3 AGC   │               │  11 STFT FINA (n_fft 8192)
                4 PUERTA│               │  12 FONDO ESPECTRAL
                5 SOFT CLIP             │  13 BUSCAR f0
                6 STFT   │              │  14 ESTABILIDAD
                7 BANCO MEL             │  15 H
                8 POWER→dB              │
                9 DCT-II │              │
               10 RED NEURONAL          │
                        └───────┬───────┘
                          16 EMA  →  17 ALERTA
```

Cada caja lleva además una **minigráfica** con los últimos 60 valores de ese
paso, del color del estado. Los valores que ya viven en un rango conocido (P, H,
EMA, estabilidad, nivel de alerta, f0) usan escala fija, para que la altura de la
traza signifique siempre lo mismo; el resto se escala con su propio mínimo y
máximo recientes.

La caja de la rama que decide la alerta va con el borde resaltado. Con
`--no-workflow` la pestaña no se añade.

## Adaptarse al ruido de la sala: `--agc`

```
python drone_detector_cli.py mic --device 12 --agc
python drone_detector_gui.py  --device 12 --agc
```

Con `--agc` la ganancia deja de ser fija: se estima el **suelo de ruido** del
entorno y se aplica la ganancia que lo lleva a un nivel de referencia. La puerta
deja de ser "−35 dBFS absolutos" y pasa a ser "**N dB por encima del fondo de
esta sala**" (`--gate-over-floor`, 6 dB por defecto). Ventajas:

- No hace falta `calibrate`, ni recalibrar al cambiar de sitio.
- Lo que el modelo ve deja de depender del volumen absoluto.

No es un compresor de audio: la ganancia es **una constante por ventana de 1 s**
y se mueve despacio. Un compresor con ataque/release rápidos aplastaría la
envolvente temporal, que es justo donde está la firma de un dron.

El seguidor del suelo baja al instante y sube solo 0.7 dB/s, y además **se
congela** mientras la ventana sobresale más de 6 dB. Sin eso, un dron sostenido
se convertiría en el nuevo "ambiente" al cabo de medio minuto y dejaría de
detectarse. Verificado: 25 s de evento a +20 dB y el suelo no se mueve.

### De dónde viene el volumen en la decisión

Medido con la misma señal a distintos volúmenes:

| amplitud | sin AGC: p | con AGC: p |
|---|---|---|
| 0.003 | 0.00 | 0.00 |
| 0.010 | **1.00** | 0.00 |
| 0.100 | **1.00** | 0.00 |

Sin AGC la `p` salta de 0 a 1 solo por subir el volumen. Y no es que el modelo
mire el nivel: es **la puerta**, que en silencio impide que se ejecute la red.
Por encima de la puerta, la red responde igual a cualquier volumen.

Sobre el MFCC: el coeficiente **c0 es literalmente el volumen** (la fila 0 de la
DCT es constante y suma 8.0; las filas 1–19 suman cero y sí son invariantes al
nivel). Con `power_to_db(ref=1.0)` ese c0 absoluto entra en la red. El preset
`librosa` (`ref=max`) lo normaliza por ventana y lo elimina.

## Empieza por calibrar

El firmware normaliza el ICS-43434 con `>>8 / 8388608` y aplica una cadena DSP
agresiva (HPF α=0.9 → ×15 → `tanh`) antes del gate de silencio de −35 dBFS. Un
micrófono de portátil entrega un nivel completamente distinto: con `--gain 1`
(el valor del firmware) el ambiente cae a unos −58 dBFS y **el gate lo filtra
todo**, así que el detector diría "SIN DRON" siempre.

`calibrate` graba unos segundos de ruido de fondo y busca la ganancia que deja
el ambiente justo por debajo del gate. Dos avisos:

- Calibra con ruido de fondo **real**, no en una sala muda. Si el nivel crudo
  sale por debajo de −70 dBFS el comando te lo advierte: normalmente significa
  que el micro está silenciado o que el array *Intel Smart Sound* está aplicando
  supresión de ruido (se desactiva en Configuración → Sistema → Sonido → Micrófono).
- El `tanh` satura, así que la ganancia no es solo volumen: también cambia cuánta
  distorsión armónica entra en el MFCC.

## Elegir por dónde entra el audio

El mismo micrófono entrega niveles muy distintos según el host API, porque
MME, DirectSound y WASAPI pasan por el motor de audio de Windows —que aplica
supresión de ruido— y WDM-KS habla con el driver directamente. Medido en un
portátil con array *Intel Smart Sound*, en una sala en silencio:

| host API | ambiente |
|---|---|
| MME | −96.7 dBFS |
| DirectSound | −86.6 |
| WASAPI | −86.4 |
| **WDM-KS** | **−70.8** |

26 dB de diferencia con el mismo hardware. `probe` mide todas las entradas y
recomienda la mejor. Detalles a tener en cuenta:

- Las **mejoras de audio** de Windows se apagan en Configuración → Sistema →
  Sonido → (clic en el micro) → *Mejoras de audio* → Desactivado; y el
  *Aislamiento de voz*, si aparece. En el panel clásico (`Win+R` → `mmsys.cpl`)
  están además *Niveles* y la pestaña *Mejoras*. Ojo: si el recorte lo hace el
  DSP del propio Intel Smart Sound y no un APO de Windows, ese interruptor no
  cambia nada — ahí la salida es WDM-KS.
- Los pines **WDM-KS son exclusivos**: solo un programa a la vez. Si uno
  responde *Invalid device*, lo tiene otro proceso; prueba el siguiente.
- Algunos endpoints WDM-KS son salidas expuestas como entrada y devuelven
  valores absurdos; `probe` los marca y los descarta.
- `--device` acepta índice o parte del nombre. **Usa el nombre**: los índices
  cambian al conectar o desconectar audio.
- Si el dispositivo solo admite 48 kHz, la captura se reamostrea a 44.1 sola.
  Instala `scipy` para que ese reamostrado sea de calidad.

## Los tres pipelines de `--mode`

| modo | parámetros | de dónde sale |
|---|---|---|
| `clean` (def) | n_fft 2048, 64 mel, 20 MFCC, `power_to_db(ref=1.0)`, bins 0–6 a cero | lo que `mfcc_stm32.h` declara como "idéntico al ESP32" |
| `header` | n_fft 1024, 40 mel, 13 MFCC rellenados a 20 | dimensiones reales de `mfcc_matrices_44k.h` |
| `librosa` | como `clean` pero `power_to_db(ref=np.max)` y sin descartar bins | convenio por defecto de librosa |

El modelo espera `[1, 100, 20, 1]` y es **muy sensible a la escala**: pasar de
`ref=1.0` a `ref=max` mueve el primer coeficiente MFCC unos 200 dB y cambia por
completo la salida. Como el script de entrenamiento no está en el repo, no hay
forma de confirmar cuál es el correcto sin audio real; para eso está `compare`,
que pasa el mismo WAV por los tres a la vez.

## H: parecido con la firma de un dron (`harmonic.py`)

Métrica **independiente del modelo y del volumen**, pensada para responder a
"¿se parece esta señal a la de un dron?" en vez de "¿suena fuerte?".

Un multirrotor gira a RPM casi constantes: cada rotor produce un tono a la
frecuencia de paso de pala (RPM/60 × nº de palas, 100–300 Hz típicos) con muchos
armónicos, sostenido en el tiempo. Eso es un **peine espectral estable**.

Cómo se mide, y por qué no depende del nivel:

1. STFT del segundo con `n_fft=8192` (5.4 Hz por bin; con los 21.5 Hz del MFCC un
   peine de 120 Hz no se resuelve).
2. Fondo espectral por percentil en bloques, y se resta. El residuo dice cuánto
   sobresale cada frecuencia **sobre su propio entorno**. Multiplicar la señal por
   una constante desplaza el espectro en bloque y esa diferencia no cambia: la
   invariancia es por construcción, no por calibración.
3. Se busca el f0 cuyos múltiplos caen sobre los picos del residuo (mediana sobre
   los armónicos, para tolerar que falte alguno; ante empate se prefiere el f0 más
   bajo, que es el fundamental y no su octava).
4. Estabilidad: si cada frame del segundo ve el mismo f0. **Aquí es donde la voz
   se cae**: también es un peine, pero con f0 inestable.

`H = f(cuánto sobresale) × estabilidad × f(cuántos armónicos)`, y 0 si hay menos
de 4 armónicos. Se calcula sobre la salida del **paso alto**, nunca sobre `y`: la
cadena del firmware acaba en `tanh()`, un recortador que *genera* armónicos y
daría un parecido a dron falso con cualquier sonido fuerte.

### Medido

| señal | H | f0 | armónicos | estabilidad |
|---|---|---|---|---|
| dron 140 Hz | **1.00** | 140.0 Hz | 25 | 100 % |
| dron 140 Hz + ruido de palas | **1.00** | 140.0 Hz | 26 | 100 % |
| dron 220 Hz | **1.00** | 219.5 Hz | 22 | 100 % |
| voz (f0 110 Hz con vibrato) | 0.00 | — | 12 | 0 % |
| portazo / ruido fuerte | 0.09 | — | 20 | 11 % |
| tono puro 1 kHz | 0.00 | — | 0 | 0 % |
| ruido blanco | 0.09 | — | 15 | 11 % |

Invariancia comprobada sobre 60 dB de rango: el mismo dron a amplitud 0.001 y a
0.8 da `H = 1.000` y el mismo f0.

Sensibilidad (dron enterrado en ruido blanco): H ≥ 0.89 hasta −6 dB de SNR, 0.56
a −10 dB, y por debajo de −15 dB se pierde (empieza a confundir f0 con su octava).

H se calcula y se muestra **siempre**, tenga la puerta abierta o cerrada: al ser
invariante al nivel no necesita la puerta para nada, y esconderla en los ratos
tranquilos hacía que el valor desapareciera de la pantalla durante segundos.

Lo que sí sigue dependiendo de la puerta es la EMA: una ventana con la puerta
cerrada no alimenta la alerta, solo la deja decaer. Si quieres que un dron muy
lejano —que apenas sobresale del ambiente— llegue a disparar, baja
`--gate-over-floor` de 6 a 3 dB.

Cuesta unos 40 ms por segundo de audio. Se desactiva con `--no-harmonic`.

## Evaluado contra el dataset etiquetado

`dataset_44k_combined_daataset_1s` (5120 dron + 5120 no dron, clips de 1 s a
44.1 kHz). Sobre una muestra aleatoria de 3000:

| métrica | AUC | precisión | recall | F1 |
|---|---|---|---|---|
| **P · pipeline `training`** | **1.000** | 1.000 | 1.000 | **1.000** |
| P · pipeline del firmware | 0.757 | 0.610 | 1.000 | 0.758 |
| H · peine armónico | 0.777 | 0.729 | 0.731 | 0.730 |

La separación de P es real, no un redondeo: el peor clip de dron saca 0.999382 y
el peor de no-dron 0.086072, un hueco de **0.91**. Cero errores con cualquier
umbral entre 0.1 y 0.9.

**El modelo siempre estuvo bien.** Lo que estaba roto era el pipeline: con el
del firmware el AUC cae de 1.000 a 0.757.

> Advertencia importante: es casi seguro que estos clips son los mismos con los
> que se entrenó (`modelo_tensor_audio_MFFCs.py` hace 5-fold sobre todo el
> conjunto, así que el modelo campeón vio el 80 % de ellos). Esto demuestra que
> **el pipeline ya es correcto**, no que el modelo generalice a drones o entornos
> nuevos. Para medir eso hace falta audio que no esté en el dataset.

La H se queda en 0.777: pierde 403 de cada 1500 drones y da 50 falsas alarmas
por encima de 0.6. Sigue siendo útil como diagnóstico —es invariante al volumen
y se puede explicar— pero ya no decide.

## Quién decide la alerta: `--decide`

Por defecto **la alerta la decide el modelo** (`model`). `--decide` lo cambia:

| valor | qué alimenta la EMA |
|---|---|
| `model` (def) | p de la red — AUC 1.000 sobre el dataset |
| `harmonic` | H — invariante al volumen, pero AUC 0.777 |
| `both` | `min(p, H)` — **no usar**: la H arrastra al mínimo y se pierden drones que el modelo detecta sin problema |

La EMA, la persistencia y los umbrales (0.25 / 0.60 / 0.85) siguen siendo los de
`drone_detection.c`; lo único que cambia es qué número entra en ellos.

Misma escena por los tres modos (4 s de ambiente, 4 de ruido fuerte, 2 de
ambiente, 4 de dron):

| modo | con ruido fuerte | con dron |
|---|---|---|
| `model` | **ALERTA ROJA** (falso positivo) | ROJA |
| `harmonic` | nada, EMA 0.09 | ROJA a los 2 s |
| `both` | nada | ROJA a los 3 s |

En la consola, el asterisco marca la puntuación que manda:
`Mic1:+27.2dB/fondo p=1.00 H=1.00*(142Hz)`. En la ventana, la etiqueta
"← decide la alerta" está bajo el medidor correspondiente.

## Por qué la P no funcionaba — resuelto

El script de entrenamiento está en
`khamex/sound_recognition/v44100Khz/modelo_tensor_audio_MFFCs.py`. Comparándolo
con lo que hacía este código (portado de `drone_detection.c` + `mfcc_stm32.c`)
aparecen **cuatro diferencias**, y las cuatro afectan al MFCC:

| paso | entrenamiento (`extract_mfcc`) | firmware Portenta / port inicial |
|---|---|---|
| cadena DSP | HPF α=0.9 → ×15 → `tanh` | **+ quitar la media + preénfasis 0.97** |
| bins bajos | nada | **bins 0..6 puestos a cero** |
| suelo de dB | `max − 80` (así aplica `top_db` librosa) | **−80 absoluto** |
| relleno STFT | ceros (librosa ≥0.10) | reflexión |

Las dos primeras las añadió `drone_detection.c` en la Portenta y nunca
estuvieron en el entrenamiento. La tercera es un malentendido de qué hace
`top_db` en `librosa.power_to_db`: no recorta en −80 absoluto, recorta 80 dB por
debajo del máximo de ese espectrograma.

El preset **`training`** reproduce el script exactamente: verificado contra
librosa real, diferencia **0.005 dB RMS** sobre valores de −263 a +152. Es el
pipeline por defecto.

### Antes y después

Logit de la última capa (antes de la sigmoide). Una red sana da ±1 a ±5:

| | antes | con `training` |
|---|---|---|
| rango de logits | **+28 a +90** | −8 a +11 |
| ruido blanco | P = **1.00** | P = 0.00 |
| ruido real de la sala | P = **1.00** | P = 0.00–0.05 |

### Y las matrices del firmware

`generar_matrices_mfcc.py` tiene la configuración vieja comentada
(n_fft 1024, 40 mel, 13 MFCC → `mfcc_matrices_44k.h`) y la nueva activa, que
escribe a **`mfcc_matrices_44k_v2.h`**. Ese `_v2` nunca se copió a la Portenta.
Además el generador construye la DCT traspuesta: `dct(basis_vector)` con un 1 en
la posición *i* devuelve la **columna** *i*, no la fila.

Para arreglar el firmware hay que: regenerar las matrices con la config v2,
corregir la transposición, quitar el `for (k=0;k<7;k++) power[k]=0`, quitar la
media y el preénfasis de `process_channel`, y cambiar el suelo de dB a relativo.

## Estado del modelo

El modelo actual (`drone_mfcc_model.tflite`, Conv16→Conv32→Dense64→Dense1+sigmoide)
responde a la **energía de banda ancha**, no a la estructura armónica. Escena
sintética con el AGC activo:

| qué suena | sobre el fondo | puerta | p |
|---|---|---|---|
| ambiente | 0.0 dB | cerrada | 0.00 |
| ruido fuerte (portazo) | 28.0 dB | abre | **1.00** → ALERTA ROJA |
| voz | 12.5 dB | abre | 0.00 |
| peine armónico tipo dron | 13.6 dB | abre | **0.00** |

Ojo con la lectura: ese "dron" de prueba es un peine armónico limpio y un dron
real lleva además mucho ruido de banda ancha de las palas, así que esto **no**
demuestra que falle con un dron real. Lo que demuestra es a qué responde. Grabar material con `record` (etiquetas `dron` y `sin_dron`) es el
primer paso para poder reentrenarlo.

## Nota sobre el firmware de la Portenta

`CM7/Core/Inc/mfcc_matrices_44k.h` **no encaja con el código que lo usa**:

- `k_mel_filter_bank[20520]` es un banco de 40 mel × 513 bins (n_fft = 1024),
  pero `mfcc_stm32.c:165` lo indexa como 64 × 1025 → necesitaría 65.600 floats
  y lee unos 45.000 fuera del array.
- `k_dct_matrix[520]` es 13 × 40 y además está guardada traspuesta; el código la
  indexa como 20 × 64.

Es decir, los MFCC que calcula la Portenta son basura, y por eso no reconoce
drones aunque el modelo y la captura de audio funcionen. El modo `header` de
estas herramientas reproduce esas dimensiones solo para comparar: **no** es la
referencia de lo correcto.

## Estructura

```
drone_detector/
    mfcc.py       MFCC (mel Slaney + DCT-II ortho) y la cadena DSP del firmware
    detector.py   TFLite + lógica de alerta (EMA, persistencia, gate)
    audio.py      captura por ventanas de 1 s, WAV, reamostrado, dispositivos
drone_detector_cli.py    consola
drone_detector_gui.py    gráficas en vivo (matplotlib)
tools/extract_tflite.py  network.c -> .tflite
```
