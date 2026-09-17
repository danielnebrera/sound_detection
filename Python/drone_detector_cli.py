"""
Detector de drones — versión Windows.

    python drone_detector_cli.py list-devices            # ver entradas de audio
    python drone_detector_cli.py calibrate               # ajustar --gain a tu micro
    python drone_detector_cli.py mic --gain 25           # detección en vivo
    python drone_detector_cli.py wav audio.wav           # procesar un WAV
    python drone_detector_cli.py record --label dron     # grabar dataset
    python drone_detector_cli.py compare audio.wav       # comparar pipelines MFCC

Formato de salida igual al que imprime la Portenta por UART:

    Mic1:  -12dB p=0.14 | EMA:0.09  [RASTREANDO 1/3]
    Mic1:  -11dB p=0.87 | EMA:0.60  >>> ALERTA ROJA: DRON DETECTADO (60%)
"""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

import numpy as np

# Permitir ejecutar desde cualquier carpeta
sys.path.insert(0, str(Path(__file__).resolve().parent))

from drone_detector import (                                    # noqa: E402
    DroneDetector, DetectorConfig, DetectionResult, PRESETS,
    preprocess_pcm, dbfs,
)
from drone_detector import audio as au                          # noqa: E402


SR = 44100


# ── Colores ANSI (mismo esquema que Python/can_monitor.py) ─────
COLORS = {0: "\033[32m", 1: "\033[33m", 2: "\033[33m", 3: "\033[31m"}
RESET  = "\033[0m"
BOLD   = "\033[1m"


# ── Presentación (imita drone_detection.c y can_monitor.py) ────
def print_result(res: DetectionResult, elapsed_ms: float, prefix: str = "",
                 agc: bool = False, decide: str = "harmonic") -> None:
    """El asterisco marca la puntuación que decide la alerta."""
    color = COLORS.get(res.alerta, "")
    parts = []
    n = len(res.p_channels)
    overs = res.over_floor_channels or [float("nan")] * n
    hs    = res.h_channels or [float("nan")] * n
    f0s   = res.f0_channels or [float("nan")] * n
    for i, (p, db, ok, over, h, f0) in enumerate(
            zip(res.p_channels, res.db_channels, res.valid_channels, overs, hs, f0s)):
        # Con AGC lo informativo no es el nivel absoluto, sino cuanto sobresale
        # sobre el ruido de fondo de la sala.
        nivel = f"+{over:4.1f}dB/fondo" if agc else f"{db:5.0f}dB"
        marca_p = "*" if decide == "model" else ""
        marca_h = "*" if decide == "harmonic" else ""
        if not ok:
            # Con la puerta cerrada no hay p (no se ejecuta la red), pero H si:
            # no depende del nivel, asi que se sigue enseñando.
            trozo = f"Mic{i+1}:{nivel} [gate]"
            if np.isfinite(h):
                trozo += f" H={h:.2f}{marca_h}"
            parts.append(trozo)
            continue
        trozo = f"Mic{i+1}:{nivel} p={p:.2f}{marca_p}"
        if np.isfinite(h):
            # H = parecido de la señal con el peine armonico de un dron,
            # independiente del volumen. f0 = frecuencia de paso de pala.
            trozo += f" H={h:.2f}{marca_h}"
            if h >= 0.25 and np.isfinite(f0):
                trozo += f"({f0:.0f}Hz)"
        parts.append(trozo)
    line = prefix + " | ".join(parts) + f" | EMA:{res.ema:.2f}"

    if res.alerta == 3:
        line += f"  >>> {color}{BOLD}ALERTA ROJA: DRON DETECTADO ({res.ema*100:.0f}%){RESET}"
    elif res.alerta == 2:
        line += f"  >>> {color}{BOLD}ALERTA NARANJA: DRON LEJANO CONFIRMADO{RESET}"
    elif res.alerta == 1:
        line += f"  {color}[RASTREANDO {res.persistence}/3]{RESET}"

    line += f"  ({elapsed_ms:.0f} ms)"
    print(line, flush=True)


def build_detector(args) -> DroneDetector:
    agc = getattr(args, "agc", False)
    cfg = DetectorConfig(
        mic_gain=args.gain, silence_db=args.gate, agc=agc,
        agc_target_floor_dbfs=getattr(args, "agc_floor", -45.0),
        gate_over_floor_db=getattr(args, "gate_over_floor", 6.0),
        harmonic=not getattr(args, "no_harmonic", False),
        decision=getattr(args, "decide", "harmonic"),
    )
    det = DroneDetector(config=cfg, mfcc_params=PRESETS[args.mode])
    p = PRESETS[args.mode]
    print(f"[CFG] pipeline={args.mode} (n_fft={p.n_fft} n_mels={p.n_mels} "
          f"n_mfcc={p.n_mfcc} ref_max={p.ref_max})")
    if cfg.harmonic:
        print("[CFG] H = peine armónico: parecido de la señal con la firma de un "
              "dron (0..1), independiente del volumen y del modelo")
    quien = {"harmonic": "H (peine armónico)",
             "model":    "p (red neuronal)",
             "both":     "min(p, H) — tienen que coincidir los dos"}[cfg.decision]
    print(f"[CFG] las alertas las decide: {quien}")
    if agc:
        print(f"[CFG] AGC ON — el fondo de la sala se lleva a "
              f"{cfg.agc_target_floor_dbfs:g} dBFS y la puerta se abre cuando la "
              f"ventana supera ese fondo en {cfg.gate_over_floor_db:g} dB")
    else:
        print(f"[CFG] ganancia fija {args.gain:g}  ·  puerta {args.gate:g} dBFS "
              f"absolutos (como el firmware)")
    return det


# ── Modo micrófono ────────────────────────────────────────────
def run_mic(args) -> None:
    detector = build_detector(args)
    print(f"[MIC] Abriendo dispositivo {args.device or 'predeterminado'} @ {SR} Hz")
    print("[MIC] Ctrl+C para salir\n")

    try:
        with au.MicStream(device=args.device, samplerate=SR, blocksize=1024,
                          hop=int(round(args.hop * SR)),
                          max_lag_s=args.max_lag) as mic:
            for segundo in mic.windows():
                t0 = time.perf_counter()
                res = detector.process([segundo])
                print_result(res, (time.perf_counter() - t0) * 1000,
                             agc=getattr(args, "agc", False),
                             decide=getattr(args, "decide", "harmonic"))
    except KeyboardInterrupt:
        print("\n[MIC] Detenido.")


# ── Modo archivo WAV ──────────────────────────────────────────
def run_wav(args) -> None:
    x = au.read_wav(args.path, SR)
    n_windows = len(x) // SR
    print(f"[WAV] {args.path}  ({len(x)/SR:.2f} s)")
    if n_windows == 0:
        raise SystemExit("El WAV es más corto de 1 segundo — mínimo requerido.")

    detector = build_detector(args)
    for w in range(n_windows):
        t0 = time.perf_counter()
        res = detector.process([x[w * SR:(w + 1) * SR]])
        print_result(res, (time.perf_counter() - t0) * 1000,
                     prefix=f"[t={w:3d}s] ", agc=getattr(args, "agc", False),
                     decide=getattr(args, "decide", "harmonic"))


# ── Calibración de ganancia ───────────────────────────────────
def _post_dsp_dbfs(x: np.ndarray, gain: float) -> float:
    return dbfs(preprocess_pcm(x, gain))


def run_calibrate(args) -> None:
    """
    Mide el ruido de fondo y busca la ganancia que deja el ambiente justo por
    debajo del gate, para que el silencio se filtre y un dron sí lo supere.
    """
    target = args.gate - args.margin
    print(f"[CAL] Grabando {args.seconds:.0f} s de ruido de fondo — "
          f"no hagas ruido, y sobre todo NO pongas el dron ahora.")
    x = au.record_seconds(args.seconds, device=args.device, samplerate=SR)

    raw_rms = float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))
    raw_db  = 20 * np.log10(raw_rms) if raw_rms > 1e-12 else -120.0
    print(f"[CAL] Nivel crudo del micro: RMS={raw_rms:.6f} ({raw_db:+.1f} dBFS), "
          f"pico={np.abs(x).max():.4f}")

    if raw_rms < 1e-6:
        raise SystemExit("[CAL] El micro no está capturando nada. Revisa el "
                         "dispositivo y los permisos de Windows.")

    if raw_db < -70.0:
        print(f"\n[CAL] AVISO: {raw_db:+.1f} dBFS es prácticamente silencio digital. "
              "La ganancia que salga de aquí no servirá.")
        print("[CAL] Causas típicas en un portátil:")
        print("      - el micro está silenciado o al mínimo en Windows")
        print("      - el array Intel Smart Sound aplica supresión de ruido y "
              "recorta el ambiente (desactiva las mejoras de audio en")
        print("        Configuración > Sistema > Sonido > Micrófono)")
        print("      - el dispositivo predeterminado no es el que crees: prueba "
              "list-devices y --device <n>")
        print("[CAL] Calibra con algo de ruido de fondo real, no en una sala muda.\n")

    # El tanh satura: la relación gain -> dBFS no es lineal, pero sí monótona.
    # Búsqueda binaria sobre log(gain).
    ventana = x[:SR] if len(x) >= SR else np.pad(x, (0, SR - len(x)))
    lo, hi = 1e-3, 1e4
    if _post_dsp_dbfs(ventana, hi) < target:
        print(f"[CAL] Ni con ganancia {hi:g} se alcanza {target:+.1f} dBFS. "
              f"El micro está prácticamente mudo.")
        gain = hi
    else:
        for _ in range(60):
            mid = (lo * hi) ** 0.5
            if _post_dsp_dbfs(ventana, mid) < target:
                lo = mid
            else:
                hi = mid
        gain = (lo * hi) ** 0.5

    logrado = _post_dsp_dbfs(ventana, gain)
    print(f"\n[CAL] Ganancia sugerida: {BOLD}{gain:.1f}{RESET}")
    print(f"[CAL] Con ella el ruido de fondo queda en {logrado:+.1f} dBFS "
          f"({args.margin:g} dB por debajo del gate de {args.gate:+.1f}).")
    # El --device tiene que viajar con la ganancia: se calibro para ESE microfono.
    dev = f' --device "{args.device}"' if args.device else ""
    print(f"\n  python drone_detector_cli.py mic{dev} --gain {gain:.1f} --gate {args.gate:g}")
    print(f"  python drone_detector_gui.py{dev} --gain {gain:.1f} --gate {args.gate:g}\n")
    print("[CAL] Aviso: la cadena del firmware satura con tanh(), así que subir "
          "la ganancia no solo sube el volumen — también mete distorsión armónica "
          "en el MFCC. Si el detector se dispara con todo, prueba con menos ganancia.")


# ── Grabación de dataset ──────────────────────────────────────
def run_record(args) -> None:
    out_dir = Path(args.out) / args.label
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[REC] Grabando {args.seconds:.0f} s etiquetados como {args.label!r}...")
    x = au.record_seconds(args.seconds, device=args.device, samplerate=SR)

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = out_dir / f"{args.label}_{stamp}.wav"
    au.write_wav(str(path), x, SR)

    rms = float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))
    db = 20 * np.log10(rms) if rms > 1e-12 else -120.0
    print(f"[REC] {path}  ({len(x)/SR:.1f} s, {db:+.1f} dBFS, pico {np.abs(x).max():.3f})")
    if np.abs(x).max() >= 0.99:
        print("[REC] AVISO: hay clipping en la grabación, baja el volumen de entrada.")


# ── Sondeo de dispositivos ────────────────────────────────────
def run_probe(args) -> None:
    """
    Abre cada entrada 1 s y mide el nivel real que entrega. Sirve para ver por
    qué camino llega la señal menos recortada: el mismo micro da niveles muy
    distintos según el host API, porque MME/DirectSound/WASAPI pasan por el
    motor de audio de Windows (con supresión de ruido) y WDM-KS no.
    """
    import sounddevice as sd

    print(f"Midiendo {args.seconds:g} s en cada entrada. Haz un ruido constante "
          f"(habla, frota la mesa) para comparar de verdad.\n")
    print(f"{'id':>3}  {'dispositivo':42s} {'api':16s} {'sr':>6} {'dBFS':>8}  estado")
    print("-" * 100)

    # Windows expone algunas salidas (y el loopback "Mezcla estéreo") como
    # entradas. Graban lo que suena por los altavoces, no el micro: no sirven
    # para detectar drones aunque den nivel alto.
    NO_SON_MICROS = ("altavoz", "speaker", "output", "salida",
                     "mezcla estéreo", "mezcla estereo", "stereo mix")

    candidatos = []
    for i, d in au.list_input_devices():
        api = sd.query_hostapis(d["hostapi"])["name"]
        sr  = int(d["default_samplerate"])
        nombre_full = d["name"]
        nombre = nombre_full[:42]
        try:
            rec = sd.rec(int(args.seconds * sr), samplerate=sr, channels=1,
                         dtype="float32", device=i)
            sd.wait()
            x = rec[:, 0].astype(np.float64)
            rms  = float(np.sqrt(np.mean(x ** 2)))
            pico = float(np.abs(x).max())
            db   = 20 * np.log10(rms) if rms > 1e-12 else -240.0

            es_salida = any(k in nombre_full.lower() for k in NO_SON_MICROS)

            if not np.isfinite(pico) or pico > 1.5:
                estado = "descartado: valores fuera de rango"
            elif es_salida:
                estado = "descartado: es una salida o loopback, no un micro"
            elif rms < 1e-7:
                estado = "sin señal (mudo o nada conectado)"
            else:
                estado = "ok"
                candidatos.append((db, i, nombre_full, api))
            print(f"{i:3d}  {nombre:42s} {api[:16]:16s} {sr:6d} {db:+8.1f}  {estado}")
        except Exception as exc:
            msg = str(exc).split("[")[0].strip()
            print(f"{i:3d}  {nombre:42s} {api[:16]:16s} {sr:6d} {'—':>8}  {msg}")

    if not candidatos:
        print("\nNinguna entrada dio señal. Revisa que el micro no esté silenciado.")
        return

    candidatos.sort(reverse=True)
    db, i, nombre, api = candidatos[0]
    print(f"\nLa que más señal entrega: [{i}] {nombre} ({api}, {db:+.1f} dBFS)")
    print(f"\n  python drone_detector_cli.py calibrate --device {i}")
    print(f"  python drone_detector_cli.py mic --device {i} --gain <la que salga>")
    print(f"\n  ...o por nombre, que aguanta mejor los cambios de índice:")
    print(f"  python drone_detector_cli.py mic --device \"{nombre.strip()}\"")
    print("\nMejor por nombre que por índice: los índices bailan al conectar o "
          "desconectar audio.\nY los pines WDM-KS son exclusivos — si uno da "
          "'Invalid device' es que lo tiene otro programa,\nprueba el siguiente.")


# ── Comparación de pipelines ──────────────────────────────────
def run_compare(args) -> None:
    """
    Pasa el mismo audio por todas las variantes de MFCC y enseña la probabilidad
    de cada una. Sirve para averiguar cuál coincide con el entrenamiento: con un
    WAV de dron real, la variante correcta debería dar p alta de forma sostenida,
    y baja con audio sin dron.
    """
    x = au.read_wav(args.path, SR)
    n_windows = len(x) // SR
    if n_windows == 0:
        raise SystemExit("El audio es más corto de 1 segundo.")

    variantes = [(name, gain) for name in sorted(PRESETS) for gain in args.gains]
    dets = {}
    for name, gain in variantes:
        dets[(name, gain)] = DroneDetector(
            config=DetectorConfig(mic_gain=gain, silence_db=args.gate),
            mfcc_params=PRESETS[name],
        )

    print(f"[CMP] {args.path}  ({len(x)/SR:.1f} s, {n_windows} ventanas)\n")
    cabecera = "  seg  " + "".join(f"{n[:7]}/g{g:g}".rjust(16) for n, g in variantes)
    print(cabecera)
    print("  " + "-" * (len(cabecera) - 2))

    acumulado = {k: [] for k in dets}
    for w in range(n_windows):
        seg = x[w * SR:(w + 1) * SR]
        fila = f"  {w:3d}  "
        for key, det in dets.items():
            r = det.analyze(seg)
            acumulado[key].append(r["p"] if r["valid"] else float("nan"))
            marca = " " if r["valid"] else "*"
            fila += f"{r['p']:.3f}{marca}".rjust(16)
        print(fila)

    print("\n  media (ignorando ventanas con gate cerrado, marcadas con *):")
    for key, vals in acumulado.items():
        arr = np.array(vals, dtype=float)
        n_ok = int(np.sum(~np.isnan(arr)))
        media = float(np.nanmean(arr)) if n_ok else float("nan")
        print(f"    {key[0]:8s} gain={key[1]:<6g} media={media:.3f}  "
              f"ventanas validas={n_ok}/{n_windows}")


# ── Main ──────────────────────────────────────────────────────
def add_common(p, with_mode=True):
    p.add_argument("--device", default=None,
                   help="Índice o nombre parcial del dispositivo de entrada")
    p.add_argument("--gain", type=float, default=1.0,
                   help="Ganancia de entrada antes del DSP (usa calibrate para "
                        "hallarla; 1.0 = igual que el firmware)")
    p.add_argument("--gate", type=float, default=-35.0,
                   help="Umbral de silencio en dBFS tras el DSP (def -35, igual "
                        "que SILENCE_DB del firmware)")
    p.add_argument("--agc", action="store_true",
                   help="Ganancia automática anclada al ruido de fondo: la puerta "
                        "pasa a medir cuánto sobresale el sonido SOBRE el ambiente "
                        "de la sala, en vez de un nivel absoluto. Hace innecesario "
                        "calibrate y quita la dependencia del volumen.")
    p.add_argument("--agc-floor", type=float, default=-45.0,
                   help="Nivel al que el AGC lleva el ruido de fondo (def -45 dBFS)")
    p.add_argument("--gate-over-floor", type=float, default=6.0,
                   help="Con --agc: dB sobre el fondo para abrir la puerta (def 6)")
    p.add_argument("--max-lag", type=float, default=1.0,
                   help="Retraso máximo tolerado (s). Si el análisis se retrasa "
                        "más, se descarta audio viejo en vez de acumularlo (def 1.0)")
    p.add_argument("--hop", type=float, default=1.0,
                   help="Cada cuántos segundos se reanaliza. La ventana sigue "
                        "siendo de 1 s; 1.0 = sin solape, como el firmware (def 1.0)")
    p.add_argument("--decide", choices=("harmonic", "model", "both"),
                   default="harmonic",
                   help="Qué puntuación dispara las alertas: harmonic = H (def), "
                        "model = p de la red (da 1.00 con casi cualquier ruido), "
                        "both = las dos a la vez")
    p.add_argument("--no-harmonic", action="store_true",
                   help="No calcular la puntuación de peine armónico (ahorra ~40 ms "
                        "por segundo de audio)")
    if with_mode:
        p.add_argument("--mode", choices=sorted(PRESETS), default="training",
                       help="Pipeline MFCC. training (def) = copia exacta del script "
                            "de entrenamiento modelo_tensor_audio_MFFCs.py, que es lo "
                            "que el modelo espera. clean = lo que hace el firmware de "
                            "la Portenta. header = matrices viejas. librosa = ref=max")


def main() -> None:
    parser = argparse.ArgumentParser(description="Detector de drones — Windows")
    sub = parser.add_subparsers(dest="cmd", required=True)

    add_common(sub.add_parser("mic", help="Capturar del micrófono en vivo"))

    p_wav = sub.add_parser("wav", help="Procesar un archivo WAV")
    p_wav.add_argument("path", help="Ruta al WAV (se reamostrea a 44100 Hz si hace falta)")
    add_common(p_wav)

    p_cal = sub.add_parser("calibrate", help="Calcular la ganancia para tu micrófono")
    p_cal.add_argument("--seconds", type=float, default=3.0,
                       help="Segundos de ruido de fondo a medir (def 3)")
    p_cal.add_argument("--margin", type=float, default=5.0,
                       help="dB por debajo del gate donde dejar el ambiente (def 5)")
    add_common(p_cal, with_mode=False)

    p_rec = sub.add_parser("record", help="Grabar un WAV etiquetado para el dataset")
    p_rec.add_argument("--label", default="dron",
                       help="Etiqueta/subcarpeta: dron, sin_dron, ... (def dron)")
    p_rec.add_argument("--seconds", type=float, default=10.0, help="Duración (def 10)")
    p_rec.add_argument("--out", default="dataset", help="Carpeta raíz (def ./dataset)")
    add_common(p_rec, with_mode=False)

    p_cmp = sub.add_parser("compare", help="Comparar todos los pipelines MFCC sobre un WAV")
    p_cmp.add_argument("path", help="Ruta al WAV")
    p_cmp.add_argument("--gains", type=float, nargs="+", default=[1.0],
                       help="Ganancias a probar (ej: --gains 1 10 30)")
    add_common(p_cmp, with_mode=False)

    sub.add_parser("list-devices", help="Listar dispositivos de entrada disponibles")

    p_probe = sub.add_parser("probe", help="Medir el nivel real de cada entrada y "
                                           "recomendar la mejor")
    p_probe.add_argument("--seconds", type=float, default=1.0,
                         help="Segundos a medir en cada dispositivo (def 1)")

    args = parser.parse_args()

    if args.cmd == "mic":
        run_mic(args)
    elif args.cmd == "wav":
        run_wav(args)
    elif args.cmd == "calibrate":
        run_calibrate(args)
    elif args.cmd == "record":
        run_record(args)
    elif args.cmd == "compare":
        run_compare(args)
    elif args.cmd == "list-devices":
        au.print_input_devices()
    elif args.cmd == "probe":
        run_probe(args)


if __name__ == "__main__":
    main()
