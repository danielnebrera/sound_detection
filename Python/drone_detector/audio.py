"""
Captura de audio y utilidades de E/S para la version Windows.

Centraliza lo que antes estaba duplicado entre el CLI y la GUI:
  - resolucion del dispositivo de entrada (indice o nombre parcial)
  - acumulacion de bloques del callback en ventanas EXACTAS de 1 s
  - lectura de WAV con reamostrado a 44.1 kHz
"""

from __future__ import annotations

import queue
import sys
import threading
from typing import Iterator, Optional

import numpy as np


SR_DEFAULT = 44100

# El aviso de "sin scipy" se emite una sola vez por proceso: en captura en vivo
# saltaria en cada ventana de 1 s.
_aviso_reamostrado_emitido = False


# -- Dispositivos ---------------------------------------------
def list_input_devices() -> list[tuple[int, dict]]:
    """Devuelve [(indice, info)] de todas las entradas disponibles."""
    import sounddevice as sd
    return [(i, d) for i, d in enumerate(sd.query_devices())
            if d["max_input_channels"] > 0]


def print_input_devices() -> None:
    import sounddevice as sd
    for i, d in list_input_devices():
        api = sd.query_hostapis(d["hostapi"])["name"]
        print(f"  [{i:2d}] {d['name']}  "
              f"(in={d['max_input_channels']}, "
              f"sr={d['default_samplerate']:.0f}, {api})")


def resolve_device(device: int | str | None) -> int | None:
    """Acepta indice ('3' o 3) o subcadena del nombre. None = predeterminado."""
    if device is None or device == "":
        return None
    if isinstance(device, int):
        return device
    if device.isdigit():
        return int(device)

    needle = device.lower()
    matches = [i for i, d in list_input_devices() if needle in d["name"].lower()]
    if not matches:
        raise SystemExit(f"No hay ningun dispositivo de entrada que contenga '{device}'. "
                         f"Usa list-devices para verlos.")
    return matches[0]


# -- Captura en ventanas de 1 s -------------------------------
class MicStream:
    """
    Entrega ventanas de exactamente `window` muestras mono, sin perder audio
    entre ventanas (el codigo anterior descartaba el resto de cada bloque).

    Uso:
        with MicStream(device=3) as mic:
            for segundo in mic.windows():
                ...
    """

    def __init__(self, device: int | str | None = None,
                 samplerate: int = SR_DEFAULT,
                 blocksize: int = 4096,
                 channel: int = 0,
                 window: Optional[int] = None,
                 hop: Optional[int] = None,
                 max_lag_s: float = 1.0) -> None:
        self.device      = resolve_device(device)
        self.samplerate  = samplerate          # tasa que entrega este objeto
        self.capture_rate = samplerate         # tasa real de captura (puede diferir)
        self.blocksize   = blocksize
        self.channel     = channel
        self.window      = window or samplerate
        # Avance entre ventanas. Si es menor que `window` las ventanas se solapan:
        # el analisis sigue viendo 1 s de contexto pero se refresca antes, que es
        # lo que hace que la deteccion se sienta en tiempo real.
        self.hop         = hop or self.window

        # Tope de retraso tolerado. Pasado eso se TIRA audio viejo en vez de
        # acumularlo: mas vale saltarse medio segundo que ir cada vez mas atras.
        self.max_lag_s = max_lag_s

        # Cola acotada. Si el consumidor se atasca, el callback descarta el
        # bloque mas antiguo en lugar de dejar crecer la cola sin limite.
        n_bloques = max(4, int(round(max_lag_s * samplerate / max(blocksize, 1))))
        self._q: queue.Queue[np.ndarray] = queue.Queue(maxsize=n_bloques)
        self._stop = threading.Event()
        self._stream = None
        self.overflows = 0
        self.dropped_blocks = 0     # bloques tirados por cola llena
        self.skipped_windows = 0    # ventanas saltadas por ir con retraso

    # -- callback de sounddevice --
    def _callback(self, indata, frames, time_info, status):
        if status:
            self.overflows += 1
            sys.stderr.write(f"[AUDIO] {status}\n")
        bloque = indata[:, self.channel].astype(np.float32).copy()
        try:
            self._q.put_nowait(bloque)
        except queue.Full:
            # Tirar el mas viejo y meter el nuevo: siempre preferimos audio
            # reciente. Es lo que impide que esto se convierta en una bola.
            try:
                self._q.get_nowait()
                self.dropped_blocks += 1
            except queue.Empty:
                pass
            try:
                self._q.put_nowait(bloque)
            except queue.Full:
                self.dropped_blocks += 1

    def _pick_capture_rate(self) -> int:
        """
        Los dispositivos WDM-KS y WASAPI suelen ser de tasa fija (48 kHz), asi que
        pedirles 44100 falla. Si la tasa pedida no vale, capturamos a la nativa del
        dispositivo y reamostramos cada ventana.
        """
        import sounddevice as sd
        try:
            sd.check_input_settings(device=self.device, channels=self.channel + 1,
                                    samplerate=self.samplerate, dtype="float32")
            return self.samplerate
        except Exception:
            if self.device is not None:
                info = sd.query_devices(self.device, "input")
            else:
                info = sd.query_devices(kind="input")
            nativa = int(info["default_samplerate"])
            sys.stderr.write(
                f"[AUDIO] El dispositivo no acepta {self.samplerate} Hz; "
                f"capturando a {nativa} Hz y reamostrando.\n"
            )
            return nativa

    def __enter__(self) -> "MicStream":
        import sounddevice as sd
        self.capture_rate = self._pick_capture_rate()
        self._stream = sd.InputStream(
            samplerate=self.capture_rate, channels=self.channel + 1,
            blocksize=self.blocksize, device=self.device,
            dtype="float32", callback=self._callback,
        )
        self._stream.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def stop(self) -> None:
        self._stop.set()
        if self._stream is not None:
            self._stream.stop()
            self._stream.close()
            self._stream = None

    def windows(self) -> Iterator[np.ndarray]:
        """
        Generador de ventanas de `window` muestras a `samplerate`, contiguas y sin
        huecos. Si la captura va a otra tasa, cada ventana se reamostrea aqui.
        """
        # Duracion de ventana y avance, en muestras de captura
        escala    = self.capture_rate / self.samplerate
        n_captura = int(round(self.window * escala))
        n_avance  = int(round(self.hop * escala))
        # Tope duro del buffer: una ventana mas el margen de retraso. Sin esto,
        # si el consumidor se retrasa el buffer crece y cada concatenate cuesta
        # mas, con lo que se retrasa mas todavia.
        tope = n_captura + int(round(self.max_lag_s * self.capture_rate))

        buf = np.zeros(0, dtype=np.float32)
        while not self._stop.is_set():
            try:
                chunk = self._q.get(timeout=0.2)
            except queue.Empty:
                continue
            trozos = [buf, chunk]

            # Vaciar de golpe lo que se haya acumulado mientras analizabamos
            while True:
                try:
                    trozos.append(self._q.get_nowait())
                except queue.Empty:
                    break
            buf = np.concatenate(trozos)

            if len(buf) > tope:
                buf = buf[-tope:]            # tirar lo viejo, quedarse con lo reciente

            if len(buf) < n_captura:
                continue

            # Si caben varias ventanas, se analiza SOLO la mas reciente. Las
            # intermedias se saltan: preferimos ir al dia a procesarlo todo.
            pendientes = 1 + (len(buf) - n_captura) // n_avance
            if pendientes > 1:
                self.skipped_windows += pendientes - 1
                buf = buf[(pendientes - 1) * n_avance:]

            ventana = buf[:n_captura]
            if self.capture_rate != self.samplerate:
                ventana = resample_to(ventana, self.capture_rate, self.samplerate)
                if len(ventana) != self.window:
                    ventana = np.resize(ventana, self.window)
            yield np.ascontiguousarray(ventana, dtype=np.float32)

            buf = buf[n_avance:]


def record_seconds(seconds: float, device: int | str | None = None,
                   samplerate: int = SR_DEFAULT) -> np.ndarray:
    """
    Graba `seconds` segundos de mono a `samplerate` y los devuelve como float32.
    Si el dispositivo no acepta esa tasa (tipico en WDM-KS y WASAPI, que suelen
    ser de 48 kHz fijos), graba a la nativa y reamostrea.
    """
    import sounddevice as sd
    dev = resolve_device(device)

    try:
        sd.check_input_settings(device=dev, channels=1,
                                samplerate=samplerate, dtype="float32")
        captura = samplerate
    except Exception:
        info = sd.query_devices(dev, "input") if dev is not None \
               else sd.query_devices(kind="input")
        captura = int(info["default_samplerate"])
        sys.stderr.write(f"[AUDIO] El dispositivo no acepta {samplerate} Hz; "
                         f"grabando a {captura} Hz y reamostrando.\n")

    rec = sd.rec(int(round(seconds * captura)), samplerate=captura,
                 channels=1, dtype="float32", device=dev)
    sd.wait()
    return resample_to(rec[:, 0].astype(np.float32), captura, samplerate)


# -- WAV ------------------------------------------------------
def resample_to(x: np.ndarray, sr_in: int, sr_out: int = SR_DEFAULT) -> np.ndarray:
    """Reamostrea a sr_out. Usa scipy si esta disponible; si no, interpolacion lineal."""
    if sr_in == sr_out:
        return np.asarray(x, dtype=np.float32)

    try:
        from math import gcd
        from scipy.signal import resample_poly
        g = gcd(sr_in, sr_out)
        return resample_poly(x, sr_out // g, sr_in // g).astype(np.float32)
    except ImportError:
        global _aviso_reamostrado_emitido
        n_out = int(round(len(x) * sr_out / sr_in))
        t_in  = np.arange(len(x), dtype=np.float64)
        t_out = np.linspace(0, len(x) - 1, n_out)
        if not _aviso_reamostrado_emitido:
            _aviso_reamostrado_emitido = True
            sys.stderr.write(
                f"[AUDIO] scipy no disponible: reamostrado lineal {sr_in}->{sr_out} Hz. "
                f"Instala scipy (pip install scipy) para mejor calidad.\n"
            )
        return np.interp(t_out, t_in, x).astype(np.float32)


def read_wav(path: str, samplerate: int = SR_DEFAULT) -> np.ndarray:
    """Lee un WAV, lo pasa a mono y lo reamostrea a `samplerate`."""
    import soundfile as sf
    data, sr = sf.read(path, dtype="float32", always_2d=True)
    mono = data.mean(axis=1) if data.shape[1] > 1 else data[:, 0]
    return resample_to(mono, sr, samplerate)


def write_wav(path: str, x: np.ndarray, samplerate: int = SR_DEFAULT) -> None:
    import soundfile as sf
    sf.write(path, np.asarray(x, dtype=np.float32), samplerate, subtype="FLOAT")
