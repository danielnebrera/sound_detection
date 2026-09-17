"""
Extractor MFCC compatible con el pipeline del firmware Portenta H7.

Replica la cadena de CM7/Core/Src/mfcc_stm32.c con matrices consistentes
(64 filtros mel × 1025 bins, DCT-II ortho 20 × 64) — que es lo que el
modelo drone_mfcc_model.tflite espera en su entrada [1, 100, 20, 1].

Parámetros idénticos al firmware:
    SR=44100, N_FFT=2048, HOP=442, N_MELS=64, N_MFCC=20
    Ventana Hann (denominador N-1, igual que arm_rfft_fast + código C)
    Padding por reflexión (librosa center=True)
    Bins 0..6 puestos a 0 (elimina <150 Hz)
    power_to_db: ref=1.0, floor -80 dB
    DCT-II ortho tipo librosa
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


# ── Configuración de parámetros ───────────────────────────────
@dataclass(frozen=True)
class MFCCParams:
    sample_rate: int = 44100
    n_fft: int       = 2048
    hop: int         = 442
    n_mels: int      = 64
    n_mfcc: int      = 20         # coeficientes REALMENTE calculados
    pad_to: int      = 20         # dimensión final del vector por frame (>= n_mfcc)
    target_frames: int = 100
    zero_bins_below: int = 7      # bins 0..6 → 0 (idéntico a mfcc_stm32.c:161)
    amin: float      = 1e-10
    top_db: float    = 80.0
    mel_fmin: float  = 0.0
    mel_fmax: float | None = None
    ref_max: bool    = False      # True = restar el maximo (power_to_db(ref=np.max))
    db_floor_relative: bool = False
    """Como aplica top_db. librosa NO recorta en -top_db absoluto: recorta en
    (maximo - top_db). El firmware si usa suelo absoluto. Cambia mucho el MFCC."""
    pad_mode: str = "reflect"     # "reflect" (firmware) / "constant" (librosa >=0.10)
    dsp: str = "firmware"         # "firmware" = + quitar media + preenfasis
                                  # "esp32"    = solo HPF + x15 + tanh (el entrenamiento)


# ── Presets ───────────────────────────────────────────────────
# "clean": los parametros que mfcc_stm32.h declara como "identicos al ESP32"
#   (SR=44100, N_FFT=2048, HOP=442, N_MELS=64, N_MFCC=20, power_to_db ref=1.0).
#   Es el pipeline que el modelo deberia esperar.
MFCC_PARAMS = MFCCParams()

# "header": reproduce las dimensiones de las matrices que hay REALMENTE en
#   CM7/Core/Inc/mfcc_matrices_44k.h (mel 40x513 con n_fft=1024, DCT 13x40).
#   Ese header no coincide con lo que indexa mfcc_stm32.c (64x1025 y 20x64),
#   por lo que el firmware lee fuera del array: este preset existe solo para
#   comparar, no como referencia de lo correcto.
MFCC_PARAMS_HEADER = MFCCParams(
    n_fft=1024,
    hop=442,            # se mantiene: da 100 frames por segundo
    n_mels=40,
    n_mfcc=13,
    pad_to=20,          # el resto del vector queda a cero
    ref_max=False,      # el .c hace power_to_db con ref=1.0
    zero_bins_below=7,  # el .c pone a cero los bins 0..6 en cualquier caso
)

# "librosa": convenio por defecto de librosa.power_to_db(ref=np.max) y sin
#   descartar bins bajos. Util para descartar hipotesis sobre el entrenamiento.
MFCC_PARAMS_LIBROSA = MFCCParams(ref_max=True, zero_bins_below=0)

# "training": copia EXACTA de extract_mfcc() en
#   sound_recognition/v44100Khz/modelo_tensor_audio_MFFCs.py
#   -> el pipeline con el que se entrenaron los pesos del .tflite.
# Diferencias con lo que hacia el firmware: sin quitar la media, sin preenfasis,
# sin poner a cero los bins bajos, y el suelo de dB relativo al maximo.
MFCC_PARAMS_TRAINING = MFCCParams(
    zero_bins_below=0,
    ref_max=False,
    db_floor_relative=True,
    pad_mode="constant",
    dsp="esp32",
)

PRESETS = {
    "training": MFCC_PARAMS_TRAINING,
    "clean":   MFCC_PARAMS,
    "header":  MFCC_PARAMS_HEADER,
    "librosa": MFCC_PARAMS_LIBROSA,
}

# Nombre antiguo, mantenido para no romper llamadas existentes.
MFCC_PARAMS_FIRMWARE = MFCC_PARAMS_HEADER


# ── Utilidades ────────────────────────────────────────────────
def _hz_to_mel(f: np.ndarray) -> np.ndarray:
    """Escala mel de Slaney (idéntica a librosa por defecto)."""
    f_min = 0.0
    f_sp = 200.0 / 3
    min_log_hz = 1000.0
    min_log_mel = (min_log_hz - f_min) / f_sp
    logstep = np.log(6.4) / 27.0

    mels = (f - f_min) / f_sp
    log_region = f >= min_log_hz
    if np.any(log_region):
        with np.errstate(divide="ignore", invalid="ignore"):
            log_vals = min_log_mel + np.log(np.where(log_region, f / min_log_hz, 1.0)) / logstep
        mels = np.where(log_region, log_vals, mels)
    return mels


def _mel_to_hz(m: np.ndarray) -> np.ndarray:
    f_min = 0.0
    f_sp = 200.0 / 3
    min_log_hz = 1000.0
    min_log_mel = (min_log_hz - f_min) / f_sp
    logstep = np.log(6.4) / 27.0

    freqs = f_min + f_sp * m
    log_region = m >= min_log_mel
    freqs = np.where(log_region, min_log_hz * np.exp(logstep * (m - min_log_mel)), freqs)
    return freqs


def build_mel_filterbank(sr: int, n_fft: int, n_mels: int,
                         fmin: float = 0.0, fmax: float | None = None) -> np.ndarray:
    """Banco de filtros mel (Slaney, normalizado 'slaney'). Devuelve (n_mels, n_fft//2+1)."""
    if fmax is None:
        fmax = sr / 2.0

    n_bins = n_fft // 2 + 1
    fft_freqs = np.linspace(0, sr / 2.0, n_bins)

    mel_min = _hz_to_mel(np.array([fmin]))[0]
    mel_max = _hz_to_mel(np.array([fmax]))[0]
    mel_points = np.linspace(mel_min, mel_max, n_mels + 2)
    hz_points = _mel_to_hz(mel_points)

    fb = np.zeros((n_mels, n_bins), dtype=np.float32)
    for m in range(n_mels):
        f_l, f_c, f_r = hz_points[m], hz_points[m + 1], hz_points[m + 2]
        left  = (fft_freqs - f_l) / (f_c - f_l)
        right = (f_r - fft_freqs) / (f_r - f_c)
        fb[m] = np.maximum(0.0, np.minimum(left, right))

        # Normalización Slaney → área de cada triángulo = 2/(f_r - f_l)
        enorm = 2.0 / (f_r - f_l)
        fb[m] *= enorm

    return fb


def build_dct_matrix(n_mfcc: int, n_mels: int) -> np.ndarray:
    """DCT-II ortonormal, misma convención que scipy.fft.dct(..., norm='ortho').
    Devuelve (n_mfcc, n_mels)."""
    n = np.arange(n_mels, dtype=np.float64)
    dct = np.zeros((n_mfcc, n_mels), dtype=np.float64)
    for k in range(n_mfcc):
        dct[k] = np.cos(np.pi * k * (2 * n + 1) / (2 * n_mels))
    dct[0]  *= np.sqrt(1.0 / n_mels)
    dct[1:] *= np.sqrt(2.0 / n_mels)
    return dct.astype(np.float32)


def _reflect_slice(x: np.ndarray, start: int, length: int) -> np.ndarray:
    """Extrae `length` muestras a partir de `start`, replicando la función
    `reflect_at` de mfcc_stm32.c (idéntica a np.pad(mode='reflect'))."""
    n = x.shape[0]
    idx = np.arange(start, start + length)
    # Reflexión en dos bordes
    while np.any(idx < 0) or np.any(idx >= n):
        neg = idx < 0
        idx = np.where(neg, -idx - 1, idx)
        big = idx >= n
        idx = np.where(big, 2 * n - idx - 1, idx)
    return x[idx]


def _zero_pad_slice(x: np.ndarray, start: int, length: int) -> np.ndarray:
    """Relleno con ceros, que es lo que hace librosa >= 0.10 (pad_mode='constant')."""
    n = x.shape[0]
    out = np.zeros(length, dtype=np.float32)
    i0, i1 = max(0, start), min(n, start + length)
    if i1 > i0:
        out[i0 - start:i1 - start] = x[i0:i1]
    return out


# ── Extractor ─────────────────────────────────────────────────
class MFCCExtractor:
    """Convierte 1 segundo de audio [-1,1] → matriz MFCC [100, 20]."""

    def __init__(self, params: MFCCParams = MFCC_PARAMS) -> None:
        self.p = params
        self.window = np.hanning(params.n_fft).astype(np.float32)  # denominador N-1
        # Alinear con la ventana del firmware (0.5 * (1 - cos(2πi/(N-1))))
        # np.hanning ya usa exactamente esa fórmula.
        self.mel_fb = build_mel_filterbank(
            params.sample_rate, params.n_fft, params.n_mels,
            params.mel_fmin, params.mel_fmax
        )
        self.dct = build_dct_matrix(params.n_mfcc, params.n_mels)

    def compute(self, pcm_1s: np.ndarray) -> np.ndarray:
        """Idéntico a `compute_full` pero devolviendo solo la matriz MFCC."""
        return self.compute_full(pcm_1s)["mfcc"]

    def compute_full(self, pcm_1s: np.ndarray) -> dict:
        """
        Calcula MFCC + intermedios útiles para visualización.

        Devuelve dict con:
            'mfcc'       (target_frames, pad_to) float32   (con relleno de ceros si pad_to > n_mfcc)
            'mel_log_db' (target_frames, n_mels) float32
            'power'      (target_frames, n_fft//2+1) float32
            'freqs_hz'   (n_fft//2+1,) float32
            'times_s'    (target_frames,) float32
        """
        p   = self.p
        pad = p.n_fft // 2
        n_frames = 1 + (len(pcm_1s) + 2 * pad - p.n_fft) // p.hop
        n_frames = min(n_frames, p.target_frames)
        n_bins   = p.n_fft // 2 + 1

        mfcc_out = np.zeros((p.target_frames, p.pad_to),  dtype=np.float32)
        mel_out  = np.full((p.target_frames, p.n_mels),  -p.top_db, dtype=np.float32)
        pow_out  = np.zeros((p.target_frames, n_bins),   dtype=np.float32)

        for f in range(n_frames):
            start = f * p.hop - pad
            if p.pad_mode == "constant":
                frame = _zero_pad_slice(pcm_1s, start, p.n_fft) * self.window
            else:
                frame = _reflect_slice(pcm_1s, start, p.n_fft) * self.window
            spec  = np.fft.rfft(frame, n=p.n_fft)
            power = (spec.real ** 2 + spec.imag ** 2).astype(np.float32)

            if p.zero_bins_below > 0:
                power[:p.zero_bins_below] = 0.0
            pow_out[f] = power

            mel = self.mel_fb @ power
            mel = np.maximum(mel, p.amin)
            mel_db = 10.0 * np.log10(mel)
            mel_out[f] = mel_db

        # Referencia y suelo para power_to_db, igual que librosa:
        #   log_spec = 10*log10(max(amin,S)) - 10*log10(max(amin,ref))
        #   if top_db: log_spec = max(log_spec, log_spec.max() - top_db)
        # Ojo: el suelo es RELATIVO al maximo, no un -80 absoluto.
        if p.ref_max:
            mel_out = mel_out - mel_out.max()
        if p.db_floor_relative:
            mel_out = np.maximum(mel_out, mel_out.max() - p.top_db)
        else:
            mel_out = np.maximum(mel_out, -p.top_db)

        # DCT-II ortho (una única multiplicación matricial)
        # mfcc[t, k] = sum_m dct[k, m] * mel_out[t, m]
        mfcc_calc = mel_out @ self.dct.T          # (target_frames, n_mfcc)
        mfcc_out[:, :p.n_mfcc] = mfcc_calc
        # las columnas pad_to..-1 quedan en 0 (relleno)

        freqs = np.linspace(0, p.sample_rate / 2.0, n_bins, dtype=np.float32)
        times = np.arange(p.target_frames, dtype=np.float32) * p.hop / p.sample_rate

        return {
            "mfcc":        mfcc_out,
            "mel_log_db":  mel_out,
            "power":       pow_out,
            "freqs_hz":    freqs,
            "times_s":     times,
        }


# ── Preprocesado DSP idéntico a drone_detection.c ────────────
def highpass(samples: np.ndarray, alpha: float = 0.9) -> np.ndarray:
    """
    Primera etapa de la cadena del firmware (drone_detection.c:79-90):

        hpf[i] = α · (hpf[i-1] + x[i] - x[i-1])

    Es un IIR de primer orden, hay que resolverlo muestra a muestra. Se expone
    aparte porque es LINEAL, y eso permite medir el nivel aquí y aplicar la
    ganancia después: multiplicar antes o después del filtro da lo mismo, así
    que el AGC puede decidir con el nivel ya filtrado sin filtrar dos veces.
    """
    x = np.asarray(samples, dtype=np.float32)

    # Mismo filtro escrito como IIR estandar:
    #   y[n] = alpha*y[n-1] + alpha*x[n] - alpha*x[n-1]
    # Con scipy se resuelve en C (~1 ms en vez de ~15 ms por segundo de audio).
    try:
        from scipy.signal import lfilter
        return lfilter([alpha, -alpha], [1.0, -alpha], x).astype(np.float32)
    except ImportError:
        pass

    n = x.shape[0]
    prev_in  = 0.0
    prev_out = 0.0
    h = np.empty(n, dtype=np.float32)
    for i in range(n):
        cur = alpha * (prev_out + x[i] - prev_in)
        prev_in  = x[i]
        prev_out = cur
        h[i] = cur
    return h


def finish_dsp(hpf_out: np.ndarray, gain: float = 1.0) -> np.ndarray:
    """
    Resto de la cadena del firmware, partiendo de la salida del paso alto:

      2. tanh(hpf × 15 × gain)  (soft clipping)
      3. Eliminación de la media  (process_channel líneas 133-136)
      4. Pre-énfasis y[n]=x[n]-0.97·x[n-1]  (líneas 138-139)

    `gain` no existe en el firmware (allí es 1.0): compensa que cada micrófono
    entrega un nivel distinto. Ojo, el tanh satura, así que la ganancia no es
    solo volumen — también decide cuánta distorsión armónica entra en el MFCC.
    """
    y = np.tanh(np.asarray(hpf_out, dtype=np.float32) * (15.0 * float(gain)))

    y -= y.mean()
    y[1:] = y[1:] - 0.97 * y[:-1]
    return y


def finish_dsp_esp32(hpf_out: np.ndarray, gain: float = 1.0) -> np.ndarray:
    """
    Cadena del ESP32 tal y como la emula el script de entrenamiento
    (apply_esp32_dsp): solo tanh(x * 15). NI quitar la media NI preenfasis —
    esos dos los añadio drone_detection.c en la Portenta y no estaban en el
    entrenamiento, que es justo por lo que el modelo no reconocia nada.
    """
    return np.tanh(np.asarray(hpf_out, dtype=np.float32) * (15.0 * float(gain)))


def preprocess_pcm(samples: np.ndarray, gain: float = 1.0,
                   dsp: str = "firmware") -> np.ndarray:
    """
    Cadena completa sobre 1 segundo de audio. Entrada en [-1,1].
    dsp="firmware" reproduce drone_detection.c; dsp="esp32", el entrenamiento.
    """
    h = highpass(samples)
    return finish_dsp_esp32(h, gain) if dsp == "esp32" else finish_dsp(h, gain)


def dbfs(x: np.ndarray) -> float:
    """dBFS RMS (idéntico a compute_dbfs en drone_detection.c:123-128)."""
    if x.size == 0:
        return -120.0
    rms = float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))
    return 20.0 * np.log10(rms) if rms > 1e-12 else -120.0
