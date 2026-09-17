"""
Puntuación de "peine armónico": cuánto se parece la señal a la de un dron,
sin mirar el volumen y sin usar el modelo.

Por qué esto y no la energía
----------------------------
Un multirrotor gira a RPM casi constantes. Cada rotor genera un tono a la
frecuencia de paso de pala (BPF = RPM/60 × nº de palas, típicamente 100–300 Hz)
acompañado de muchos armónicos. Esa estructura —un peine de picos igualmente
espaciados, sostenido en el tiempo— es lo que distingue a un dron de un portazo,
de un coche o del ruido de la sala, y **no depende de lo fuerte que suene**.

Cómo se consigue la invariancia al nivel
----------------------------------------
No se mide la energía de cada frecuencia, sino **cuánto sobresale cada frecuencia
sobre su propio entorno espectral**. Si multiplicas toda la señal por una
constante, el espectro en dB se desplaza en bloque y esa diferencia no cambia.
La invariancia es por construcción, no por calibración.

Qué se calcula
--------------
1. STFT del segundo con buena resolución en frecuencia (n_fft grande: un peine
   de 140 Hz necesita separar picos de 140 Hz).
2. Fondo espectral por percentil en bloques, e interpolado. El residuo
   (espectro − fondo, en dB) dice cuánto sobresale cada bin.
3. Búsqueda del f0 cuyos múltiplos caen sobre los picos del residuo.
4. Estabilidad: si ese f0 se mantiene a lo largo del segundo. Aquí es donde un
   dron se separa de la voz, que también es un peine pero con f0 inestable.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class HarmonicConfig:
    sample_rate: int = 44100

    n_fft: int = 8192
    """Resolución 44100/8192 ≈ 5.4 Hz. Con el n_fft=2048 del MFCC los bins son
    de 21.5 Hz y un peine de 120 Hz apenas se resuelve."""

    hop: int = 4410

    f0_min: float = 60.0
    f0_max: float = 400.0
    """Rango de frecuencia de paso de pala. 60–400 Hz cubre desde un multirrotor
    grande y lento hasta uno pequeño de carreras."""

    f0_step: float = 0.5

    f_max_analysis: float = 5000.0
    """Por encima de esto los armónicos de un dron se pierden en el ruido."""

    max_harmonics: int = 30

    bg_block_bins: int = 64
    """Anchura de bloque para estimar el fondo espectral."""

    bg_percentile: float = 25.0

    min_harmonics: int = 4
    """Menos de esto no es un peine: es un tono suelto."""

    dynamic_range_db: float = 70.0
    """Solo cuentan las frecuencias que estan como mucho esto por debajo del pico
    del espectro. Sin esta condicion, en una señal limpia el 'fondo' es el suelo
    numerico del FFT, donde las fluctuaciones en dB son enormes, y un tono puro
    puntua como si tuviera un peine de armonicos fantasma."""

    harmonic_min_db: float = 3.0
    """Cuanto tiene que sobresalir un armonico para contarlo como presente.
    Medido con un barrido de SNR: con 3 dB se sigue detectando un dron enterrado
    a -10 dB bajo el ruido, y el ruido blanco se queda en 0.08."""

    hnr_full_scale_db: float = 5.0
    """Cuánto tiene que sobresalir el peine (en dB, mediana sobre sus armónicos)
    para puntuar 1.0 en ese término."""

    harmonics_full_scale: int = 6
    f0_tolerance: float = 0.05
    """Un f0 por frame cuenta como 'el mismo' si cae dentro de este margen."""


@dataclass
class HarmonicResult:
    score: float          # 0..1, combinación de los tres términos
    f0_hz: float          # frecuencia de paso de pala estimada
    hnr_db: float         # cuánto sobresale el peine sobre el fondo
    n_harmonics: int      # armónicos que superan harmonic_min_db sobre el fondo
    stability: float      # 0..1, consistencia de f0 durante el segundo
    residual_db: np.ndarray   # espectro menos su fondo (para pintar)
    freqs_hz: np.ndarray
    harmonic_freqs: np.ndarray    # múltiplos de f0 detectados


# ── Fondo espectral ───────────────────────────────────────────
def _spectral_background(db: np.ndarray, block: int, pct: float) -> np.ndarray:
    """
    Fondo suave por percentil en bloques, interpolado entre centros.

    Un filtro de mediana deslizante sería más limpio pero cuesta cientos de ms
    por segundo de audio; esto da prácticamente lo mismo en O(n) y permite correr
    en tiempo real.
    """
    n = db.shape[-1]
    n_blocks = max(2, int(np.ceil(n / block)))
    bordes = np.linspace(0, n, n_blocks + 1).astype(int)

    centros = np.empty(n_blocks)
    valores = np.empty(n_blocks)
    for b in range(n_blocks):
        i0, i1 = bordes[b], max(bordes[b] + 1, bordes[b + 1])
        centros[b] = 0.5 * (i0 + i1 - 1)
        valores[b] = np.percentile(db[i0:i1], pct)

    return np.interp(np.arange(n), centros, valores)


# ── Búsqueda del peine ────────────────────────────────────────
def _comb_scores(residual: np.ndarray, freqs: np.ndarray,
                 f0_grid: np.ndarray, cfg: HarmonicConfig) -> np.ndarray:
    """
    Para cada f0 candidato, mediana de lo que sobresalen sus armónicos.

    La mediana (y no la media) hace que un peine al que le falten armónicos
    sueltos siga puntuando, y que un f0 a la mitad del verdadero —donde uno de
    cada dos "armónicos" cae en el fondo— se hunda.
    """
    k = np.arange(1, cfg.max_harmonics + 1)
    f_arm = f0_grid[:, None] * k[None, :]              # (n_f0, K)

    dentro = f_arm <= cfg.f_max_analysis
    vals = np.interp(f_arm, freqs, residual)
    vals = np.clip(vals, 0.0, None)                     # hundirse no puntúa
    vals = np.where(dentro, vals, np.nan)

    with np.errstate(invalid="ignore"):
        scores = np.nanmedian(vals, axis=1)
    # Si un f0 no llega al mínimo de armónicos en rango, no es candidato
    n_dentro = dentro.sum(axis=1)
    scores[n_dentro < cfg.min_harmonics] = 0.0
    return np.nan_to_num(scores)


def _pick_f0(scores: np.ndarray, f0_grid: np.ndarray) -> tuple[float, float]:
    """
    Elige el f0. Un peine de f0 puntúa igual de bien en 2·f0 (que acierta los
    armónicos pares), así que ante puntuaciones parecidas se prefiere el f0 más
    bajo: el verdadero fundamental.
    """
    mejor = float(scores.max())
    if mejor <= 0.0:
        return float("nan"), 0.0
    candidatos = np.flatnonzero(scores >= 0.9 * mejor)
    idx = int(candidatos[0])
    return float(f0_grid[idx]), float(scores[idx])


# ── Análisis completo ─────────────────────────────────────────
def analyze_harmonics(pcm_1s: np.ndarray,
                      cfg: HarmonicConfig | None = None) -> HarmonicResult:
    cfg = cfg or HarmonicConfig()
    x = np.asarray(pcm_1s, dtype=np.float32)

    n_bins = cfg.n_fft // 2 + 1
    freqs  = np.linspace(0.0, cfg.sample_rate / 2.0, n_bins)
    ventana = np.hanning(cfg.n_fft).astype(np.float32)

    inicios = list(range(0, max(1, len(x) - cfg.n_fft + 1), cfg.hop))
    if not inicios:
        inicios = [0]
        x = np.pad(x, (0, cfg.n_fft - len(x)))

    residuos = np.empty((len(inicios), n_bins))
    for i, s0 in enumerate(inicios):
        trozo = x[s0:s0 + cfg.n_fft] * ventana
        esp = np.fft.rfft(trozo, n=cfg.n_fft)
        pot = esp.real ** 2 + esp.imag ** 2
        db  = 10.0 * np.log10(np.maximum(pot, 1e-20))
        # Aquí se va el nivel absoluto: lo que queda es la forma del espectro.
        residuo = db - _spectral_background(db, cfg.bg_block_bins,
                                            cfg.bg_percentile)
        # Lo que está muy por debajo del pico no es señal, es suelo numérico.
        residuo[db < db.max() - cfg.dynamic_range_db] = 0.0
        residuos[i] = residuo

    f0_grid = np.arange(cfg.f0_min, cfg.f0_max + cfg.f0_step, cfg.f0_step)

    # Peine sobre el residuo promedio: un dron es estacionario, promediar realza
    # su peine y apaga los transitorios.
    residual_medio = residuos.mean(axis=0)
    f0, hnr = _pick_f0(_comb_scores(residual_medio, freqs, f0_grid, cfg), f0_grid)

    if not np.isfinite(f0):
        return HarmonicResult(0.0, float("nan"), 0.0, 0, 0.0,
                              residual_medio, freqs, np.empty(0))

    # Estabilidad: ¿cada frame por separado ve el mismo f0?
    f0_por_frame = np.array([
        _pick_f0(_comb_scores(r, freqs, f0_grid, cfg), f0_grid)[0]
        for r in residuos
    ])
    validos = np.isfinite(f0_por_frame)
    if validos.any():
        cerca = np.abs(f0_por_frame[validos] - f0) <= cfg.f0_tolerance * f0
        stability = float(cerca.sum()) / len(f0_por_frame)
    else:
        stability = 0.0

    # Armónicos que realmente sobresalen
    k = np.arange(1, cfg.max_harmonics + 1)
    f_arm = f0 * k
    f_arm = f_arm[f_arm <= cfg.f_max_analysis]
    niveles = np.interp(f_arm, freqs, residual_medio)
    presentes = f_arm[niveles > cfg.harmonic_min_db]
    n_arm = int(presentes.size)

    if n_arm < cfg.min_harmonics:
        # Un pico suelto no es un dron por muy destacado que esté.
        score = 0.0
    else:
        score = (min(hnr / cfg.hnr_full_scale_db, 1.0)
                 * stability
                 * min(n_arm / cfg.harmonics_full_scale, 1.0))

    return HarmonicResult(
        score=float(np.clip(score, 0.0, 1.0)),
        f0_hz=f0, hnr_db=hnr, n_harmonics=n_arm, stability=stability,
        residual_db=residual_medio, freqs_hz=freqs, harmonic_freqs=presentes,
    )
