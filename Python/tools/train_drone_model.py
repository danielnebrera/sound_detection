"""
Entrena un modelo especifico para UN dron concreto y lo exporta a .tflite.

Positivos : ventanas de 1 s recortadas de una grabacion de ESE dron.
Negativos : carpetas de clips de 1 s (ambiente, otros sonidos y, si se quiere,
            OTROS drones — que es lo que hace al modelo especifico de este).

El MFCC se calcula con el MISMO codigo que usa la deteccion en vivo
(drone_detector.mfcc, preset "training"), no con librosa: asi entrenamiento e
inferencia coinciden por construccion y no por coincidencia.

Necesita TensorFlow, que no tiene rueda para Python 3.14. Usa el entorno
Python/.venv-train (Python 3.13):

    .venv-train\\Scripts\\python.exe tools\\train_drone_model.py ^
        --positivo dataset/dron_crudo_44k.wav ^
        --negativos ../dataset_44k_combined_daataset_1s/nodrone ^
        --negativos ../dataset_44k_combined_daataset_1s/drone ^
        --salida drone_mfcc_model_especifico.tflite

Sobre la honestidad de las metricas
-----------------------------------
Si los positivos salen de una grabacion y los negativos de otro material, una
red puede separarlos por la *huella del canal* (micro, codec, sala) en vez de
por el sonido del dron, y dar 99 % que no significa nada. Contra eso:

  * --igualar-canal pasa los negativos por el mismo codec y reamostrado que la
    grabacion positiva, para que esa pista deje de estar disponible.
  * el reparto de los positivos es POR TIEMPO, no aleatorio: con ventanas
    solapadas, un reparto aleatorio pone casi la misma ventana en train y en
    validacion, y la metrica se infla sola.
  * al final se evalua contra material de control que el modelo deberia
    rechazar, y se imprime por separado.
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from drone_detector.mfcc import MFCCExtractor, PRESETS, preprocess_pcm  # noqa: E402
from drone_detector import audio as au                                  # noqa: E402

SR = 44100
PARAMS = PRESETS["training"]


# ── Recorte de la grabacion ───────────────────────────────────
def ventanas_de(x: np.ndarray, hop_s: float) -> list[np.ndarray]:
    """Trocea en ventanas de 1 s solapadas. hop 0.25 s -> 4 ventanas por segundo."""
    hop = max(1, int(round(hop_s * SR)))
    return [x[i:i + SR] for i in range(0, max(1, len(x) - SR + 1), hop)]


def _absorcion_aire(v: np.ndarray, atenuacion_db: float) -> np.ndarray:
    """
    El aire no atenua por igual todas las frecuencias: se come los agudos mucho
    antes que los graves. Por eso un dron lejano no es el mismo sonido mas bajo,
    es ademas mas sordo — y el MFCC lo nota.

    Se modela con un paso bajo de primer orden cuya frecuencia de corte baja al
    alejarse: sin atenuacion no filtra, y a -34 dB (lo mas lejos que generamos)
    corta sobre 1.5 kHz.
    """
    if atenuacion_db > -3.0:
        return v
    fc = float(np.interp(atenuacion_db, [-34.0, -3.0], [1500.0, 18000.0]))
    try:
        from scipy.signal import butter, lfilter
        b, a = butter(1, fc / (SR / 2.0), btype="low")
        return lfilter(b, a, v).astype(np.float32)
    except ImportError:
        alpha = float(np.exp(-2.0 * np.pi * fc / SR))
        fuera = np.empty_like(v)
        prev = 0.0
        for i, m in enumerate(v):
            prev = (1.0 - alpha) * m + alpha * prev
            fuera[i] = prev
        return fuera


def variantes_a_distancia(v: np.ndarray, rng: np.random.Generator, n: int,
                          fondos: list[np.ndarray], rango_db: tuple[float, float],
                          ) -> list[np.ndarray]:
    """
    Convierte una ventana grabada de cerca en n versiones "a distintas
    distancias". Tres efectos, que es lo que separa esto de bajar el volumen:

      1. Atenuacion geometrica  -> ganancia, repartida uniforme en dB.
      2. Absorcion del aire     -> los agudos se pierden antes que los graves.
      3. El fondo NO se atenua  -> se mezcla ambiente real a la SNR que toque.

    El tercero es el que de verdad ensena al modelo a trabajar lejos: a 200 m el
    dron no esta "bajito", esta enterrado en el ruido del sitio. Por eso el fondo
    sale de los propios negativos y no de ruido blanco.

    Lo que NO se toca es el tono ni la velocidad: la frecuencia de paso de pala
    es la firma de ESTE dron, y moverla seria entrenar con otro.
    """
    fuera = []
    for _ in range(n):
        y = np.roll(v, int(rng.integers(0, len(v))))

        at_db = float(rng.uniform(*rango_db))
        y = _absorcion_aire(y, at_db) * (10.0 ** (at_db / 20.0))

        if fondos:
            fondo = fondos[int(rng.integers(0, len(fondos)))]
            if len(fondo) < len(y):
                fondo = np.pad(fondo, (0, len(y) - len(fondo)))
            fondo = fondo[:len(y)]
            pot_s = float(np.mean(y.astype(np.float64) ** 2))
            pot_f = float(np.mean(fondo.astype(np.float64) ** 2))
            if pot_s > 0 and pot_f > 0:
                # Cuanto mas lejos, peor relacion senal/ruido.
                snr = float(np.interp(at_db, [rango_db[0], rango_db[1]], [-3.0, 25.0]))
                snr += float(rng.uniform(-4.0, 4.0))
                escala = np.sqrt(pot_s / (pot_f * 10.0 ** (snr / 10.0)))
                y = y + fondo * escala
        fuera.append(np.clip(y, -1.0, 1.0).astype(np.float32))
    return fuera


# ── Igualar el canal de grabacion ─────────────────────────────
def pasar_por_codec(bloques: list[np.ndarray], tmp: Path) -> list[np.ndarray]:
    """
    Pasa los clips por AAC 48 kHz y los devuelve a 44.1, que es el camino que
    recorrio la grabacion positiva (movil -> m4a -> reamostrado). Se hace en un
    unico fichero concatenado: miles de llamadas a ffmpeg tardarian una eternidad.
    """
    import imageio_ffmpeg
    ff = imageio_ffmpeg.get_ffmpeg_exe()

    largo = np.concatenate(bloques).astype(np.float32)
    crudo, m4a, vuelta = tmp / "n.wav", tmp / "n.m4a", tmp / "n2.wav"
    au.write_wav(str(crudo), largo, SR)

    for cmd in ([ff, "-y", "-i", str(crudo), "-ac", "1", "-ar", "48000",
                 "-c:a", "aac", "-b:a", "192k", str(m4a)],
                [ff, "-y", "-i", str(m4a), "-ac", "1", "-ar", str(SR),
                 "-c:a", "pcm_f32le", str(vuelta)]):
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        if r.returncode != 0:
            print("  [canal] ffmpeg fallo, sigo sin igualar el canal")
            print("  " + r.stderr.strip().splitlines()[-1])
            return bloques

    y = au.read_wav(str(vuelta), SR)
    # El codec mete un retardo de arranque; recortamos por el final, no importa.
    fuera = [y[i * SR:(i + 1) * SR] for i in range(len(bloques))]
    return [b if len(b) == SR else np.pad(b, (0, SR - len(b))) for b in fuera]


# ── Caracteristicas ───────────────────────────────────────────
def a_mfcc(ventanas: list[np.ndarray], etiqueta: str) -> np.ndarray:
    ext = MFCCExtractor(PARAMS)
    fuera = np.empty((len(ventanas), PARAMS.target_frames, PARAMS.pad_to),
                     dtype=np.float32)
    for i, v in enumerate(ventanas):
        if len(v) < SR:
            v = np.pad(v, (0, SR - len(v)))
        fuera[i] = ext.compute(preprocess_pcm(v[:SR], 1.0, dsp=PARAMS.dsp))
        if (i + 1) % 500 == 0:
            print(f"    {etiqueta}: {i+1}/{len(ventanas)}", flush=True)
    return fuera[..., np.newaxis]


def cargar_carpeta(carpeta: Path, tope: int, rng: random.Random) -> list[np.ndarray]:
    ficheros = sorted(carpeta.glob("*.wav"))
    if not ficheros:
        raise SystemExit(f"No hay .wav en {carpeta}")
    if len(ficheros) > tope:
        ficheros = rng.sample(ficheros, tope)
    print(f"  {carpeta.name}: {len(ficheros)} clips")
    return [au.read_wav(str(f), SR)[:SR] for f in ficheros]


# ── Modelo ────────────────────────────────────────────────────
def construir(forma):
    import tensorflow as tf
    k = tf.keras
    return k.Sequential([
        k.Input(shape=forma),
        k.layers.Conv2D(16, (3, 3), activation="relu"),
        k.layers.MaxPooling2D((2, 2)),
        k.layers.Conv2D(32, (3, 3), activation="relu"),
        k.layers.MaxPooling2D((2, 2)),
        k.layers.Flatten(),
        k.layers.Dense(64, activation="relu"),
        k.layers.Dropout(0.3),
        k.layers.Dense(1, activation="sigmoid"),
    ])


def a_tflite(modelo, destino: Path) -> int:
    """Keras 3 no siempre admite from_keras_model; se exporta a SavedModel si falla."""
    import tensorflow as tf
    try:
        datos = tf.lite.TFLiteConverter.from_keras_model(modelo).convert()
    except Exception as exc:
        print(f"  from_keras_model no sirvio ({type(exc).__name__}); via SavedModel")
        with tempfile.TemporaryDirectory() as d:
            modelo.export(d)
            datos = tf.lite.TFLiteConverter.from_saved_model(d).convert()

    # En OneDrive el destino puede estar bloqueado por el cliente de sincronizacion
    # justo al escribir. Se escribe al lado y se reemplaza, reintentando: perder el
    # modelo recien entrenado por un bloqueo de medio segundo seria absurdo.
    import os
    import time
    aparte = destino.with_suffix(destino.suffix + ".nuevo")
    aparte.write_bytes(datos)
    for intento in range(10):
        try:
            os.replace(aparte, destino)
            return len(datos)
        except OSError as exc:
            if intento == 9:
                print(f"  No se pudo reemplazar {destino.name} ({exc}).")
                print(f"  El modelo esta en {aparte.name}: renombralo a mano.")
                return len(datos)
            time.sleep(1.0)
    return len(datos)


# ── Informe ───────────────────────────────────────────────────
def barrido_umbral(y_real, p) -> tuple[float, float]:
    from sklearn.metrics import precision_recall_fscore_support
    print("\n  umbral | precision | recall |    F1")
    print("  " + "-" * 38)
    mejor = (0.5, 0.0)
    for u in np.arange(0.10, 0.95, 0.05):
        pr, rc, f1, _ = precision_recall_fscore_support(
            y_real, (p >= u).astype(int), average="binary", zero_division=0)
        print(f"    {u:.2f} |      {pr:.2f} |   {rc:.2f} |  {f1:.2f}")
        if f1 > mejor[1]:
            mejor = (float(u), float(f1))
    return mejor


def main() -> None:
    ap = argparse.ArgumentParser(description="Entrena un modelo para un dron concreto")
    ap.add_argument("--positivo", action="append", required=True, metavar="WAV",
                    help="WAV con una grabacion de ESE dron. Repetible: cada una se "
                         "reparte por separado en train/val, para que la validacion "
                         "cubra todas y no se quede con la cola de una sola")
    ap.add_argument("--negativos", action="append", default=[], metavar="CARPETA",
                    help="Carpeta de clips de 1 s para la clase 'no es mi dron'. "
                         "Repetible. Incluir OTROS drones es lo que hace que el "
                         "modelo sea especifico de este.")
    ap.add_argument("--negativos-mismo-canal", action="append", default=[],
                    metavar="CARPETA",
                    help="Negativos grabados por la MISMA via que los positivos "
                         "(mismo microfono y mismo codec). No se les aplica "
                         "--igualar-canal: volver a codificarlos les dejaria una "
                         "marca que la red podria usar como atajo en vez del sonido")
    ap.add_argument("--control", action="append", default=[], metavar="CARPETA",
                    help="Carpeta que NO se usa para entrenar y sobre la que se "
                         "informa al final. Apuntala a otros drones para ver si el "
                         "modelo es especifico del tuyo o solo dice 'dron'")
    ap.add_argument("--salida", default="drone_mfcc_model_especifico.tflite")
    ap.add_argument("--hop", type=float, default=0.25,
                    help="Avance entre ventanas del positivo, en s (def 0.25)")
    ap.add_argument("--aug", type=int, default=0,
                    help="Variantes sinteticas a distancia por ventana positiva. "
                         "def 0 = ninguna, se entrena solo con lo grabado de verdad. "
                         "Subelo solo si te faltan grabaciones reales a distancia")
    ap.add_argument("--nivel-pico", type=float, default=0.5,
                    help="Reescala la grabacion a este pico antes de trocear. La "
                         "grabacion de cerca suele venir recortada, y entrenar con "
                         "ella saturada ensena al modelo la distorsion, no el dron "
                         "(def 0.5; 0 = no tocar)")
    ap.add_argument("--dist-db", type=float, nargs=2, default=[-34.0, 0.0],
                    metavar=("MIN", "MAX"),
                    help="Rango de atenuacion por distancia de las variantes, en dB. "
                         "def -34 0: desde el dron encima hasta 50 veces mas lejos")
    ap.add_argument("--max-por-carpeta", type=int, default=2500,
                    help="Tope de clips por carpeta de negativos (def 2500)")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--val-fraccion", type=float, default=0.25,
                    help="Cola final de la grabacion reservada para validar (def 0.25)")
    ap.add_argument("--igualar-canal", action="store_true",
                    help="Pasa los negativos por el mismo codec que el positivo, "
                         "para que la red no pueda separar por la huella del canal")
    ap.add_argument("--semilla", type=int, default=42)
    args = ap.parse_args()

    rng_np = np.random.default_rng(args.semilla)
    rng_py = random.Random(args.semilla)

    # -- negativos primero: hacen de fondo para las variantes lejanas --
    print(f"\n[1/5] Negativos")
    if not args.negativos and not args.negativos_mismo_canal:
        raise SystemExit("Hace falta al menos un --negativos CARPETA")
    neg = []
    for c in args.negativos:
        neg += cargar_carpeta(Path(c), args.max_por_carpeta, rng_py)

    if args.igualar_canal and neg:
        print("  igualando el canal de grabacion (AAC 48k -> 44.1k)...")
        with tempfile.TemporaryDirectory() as d:
            neg = pasar_por_codec(neg, Path(d))

    for c in args.negativos_mismo_canal:
        propios = cargar_carpeta(Path(c), args.max_por_carpeta, rng_py)
        print(f"    ^ ya comparte canal con los positivos: no se recodifica")
        neg += propios
    rng_py.shuffle(neg)

    n_val = int(len(neg) * args.val_fraccion)
    neg_va, neg_tr = neg[:n_val], neg[n_val:]
    print(f"  {len(neg)} negativos -> {len(neg_tr)} train / {len(neg_va)} val")

    # -- positivos, repartidos POR TIEMPO dentro de cada grabacion --
    print(f"\n[2/5] Positivos")
    pos_tr_base, pos_va_base = [], []
    for ruta in args.positivo:
        x = au.read_wav(ruta, SR)
        pico = float(np.abs(x).max())
        aviso = "  SATURADA" if pico >= 0.999 else ""
        print(f"  {Path(ruta).name}: {len(x)/SR:.1f} s, pico {pico:.3f}{aviso}")
        if args.nivel_pico > 0 and pico > 0:
            x = (x * (args.nivel_pico / pico)).astype(np.float32)

        vents = ventanas_de(x, args.hop)
        corte = int(len(vents) * (1.0 - args.val_fraccion))
        pos_tr_base += vents[:corte]
        pos_va_base += vents[corte:]
        print(f"    {len(vents)} ventanas -> {corte} train / {len(vents)-corte} val")
    if args.nivel_pico > 0:
        print(f"  todas reescaladas a pico {args.nivel_pico:g} (quita la saturacion "
              f"y iguala el nivel entre grabaciones)")

    # El fondo de las variantes sale SOLO de los negativos de entrenamiento:
    # mezclar ambiente de validacion en los positivos de train seria una fuga.
    pos_tr, pos_va = list(pos_tr_base), list(pos_va_base)
    if args.aug > 0:
        # El fondo de las variantes sale SOLO de los negativos del mismo reparto:
        # mezclar ambiente de validacion en los positivos de train seria una fuga.
        rango = (min(args.dist_db), max(args.dist_db))
        for v in pos_tr_base:
            pos_tr += variantes_a_distancia(v, rng_np, args.aug, neg_tr, rango)
        for v in pos_va_base:
            pos_va += variantes_a_distancia(v, rng_np, max(1, args.aug // 3),
                                            neg_va, rango)
        print(f"  + variantes sinteticas a distancia "
              f"{rango[0]:+.0f}..{rango[1]:+.0f} dB")
    print(f"  positivos: {len(pos_tr)} train / {len(pos_va)} val")

    # -- MFCC --
    print(f"\n[3/5] MFCC (preset 'training', el mismo que la deteccion en vivo)")
    Xtr = np.concatenate([a_mfcc(pos_tr, "pos-train"), a_mfcc(neg_tr, "neg-train")])
    ytr = np.concatenate([np.ones(len(pos_tr)), np.zeros(len(neg_tr))]).astype(np.float32)
    Xva = np.concatenate([a_mfcc(pos_va, "pos-val"), a_mfcc(neg_va, "neg-val")])
    yva = np.concatenate([np.ones(len(pos_va)), np.zeros(len(neg_va))]).astype(np.float32)
    print(f"  train {Xtr.shape}   val {Xva.shape}")

    orden = rng_np.permutation(len(Xtr))
    Xtr, ytr = Xtr[orden], ytr[orden]

    # -- entrenamiento --
    print(f"\n[4/5] Entrenando")
    import tensorflow as tf
    from sklearn.utils import class_weight

    pesos = class_weight.compute_class_weight(
        "balanced", classes=np.unique(ytr), y=ytr)
    modelo = construir(Xtr.shape[1:])
    modelo.compile(optimizer="adam", loss="binary_crossentropy", metrics=["accuracy"])
    modelo.fit(Xtr, ytr, epochs=args.epochs, batch_size=32,
               validation_data=(Xva, yva), class_weight=dict(enumerate(pesos)),
               callbacks=[tf.keras.callbacks.EarlyStopping(
                   monitor="val_loss", patience=6, restore_best_weights=True)],
               verbose=2)

    # -- informe --
    print(f"\n[5/5] Resultados sobre la cola de la grabacion (no vista al entrenar)")
    p = modelo.predict(Xva, verbose=0).flatten()
    umbral, f1 = barrido_umbral(yva, p)
    print(f"\n  mejor umbral: {umbral:.2f}  (F1 = {f1:.2f})")
    print(f"  p media en las ventanas de TU dron : {p[:len(pos_va)].mean():.3f}")
    print(f"  p media en los negativos           : {p[len(pos_va):].mean():.3f}")

    # -- control: material que el modelo NO vio y que deberia rechazar --
    for carpeta in args.control:
        clips = cargar_carpeta(Path(carpeta), args.max_por_carpeta, rng_py)
        pc = modelo.predict(a_mfcc(clips, "control"), verbose=0).flatten()
        print(f"\n  Control {Path(carpeta).name} (nunca visto, deberia dar p baja):")
        print(f"    p media {pc.mean():.3f}   "
              f"disparos al umbral {umbral:.2f}: {100*np.mean(pc >= umbral):.1f} %")
    if not args.igualar_canal:
        print("\n  AVISO: sin --igualar-canal estas cifras estan infladas. Positivos y\n"
              "  negativos vienen de cadenas de grabacion distintas y la red puede\n"
              "  estar separando por eso, no por el sonido del dron.")

    destino = Path(args.salida)
    if not destino.is_absolute():
        destino = Path(__file__).resolve().parent.parent / destino
    n_bytes = a_tflite(modelo, destino)
    print(f"\nModelo guardado: {destino}  ({n_bytes} bytes)")
    print(f"Para usarlo:\n"
          f"  python drone_detector_gui.py --model \"{destino.name}\" --decide model "
          f"--agc")


if __name__ == "__main__":
    main()
