"""
Detector de drones — ventana con pestañas + diagrama del algoritmo.

Pestañas:
  • Detección   — P del modelo, H del peine armónico, medidores y forma de onda
  • Espectro    — espectro medio del segundo y espectrograma
  • MFCC        — bandas mel y la matriz 100×20 que entra en la red
  • Armónicos   — residuo espectral y los armónicos que sostienen H

Ventana aparte:
  • Workflow    — cada paso del algoritmo en una caja, con sus valores en vivo

Solo se repinta la pestaña visible, que es lo que mantiene el coste bajo.

Uso:
    python drone_detector_gui.py --device 12 --agc
    python drone_detector_gui.py --list-devices
    python drone_detector_gui.py --no-workflow      # sin la pestaña del diagrama
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import ttk

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from drone_detector import DroneDetector, DetectorConfig, MFCC_PARAMS, PRESETS  # noqa: E402
from drone_detector import audio as au                                         # noqa: E402


SR    = MFCC_PARAMS.sample_rate
BLOCK = 1024      # 43 ms de buffer en vez de 171: menos retraso de captura
H_UMBRAL = 0.60   # a partir de aquí el peine se parece de verdad a un dron

FONDO   = "#0e0f11"
PANEL   = "#17181a"
BORDE   = "#4a4d52"
TEXTO   = "#e8eaed"
APAGADO = "#6f757c"
AZUL    = "#4dc3ff"

COLOR_ALERTA = {0: "#2ca02c", 1: "#ffe066", 2: "#ffa500", 3: "#ff3b3b"}
ALERT_TXT    = {0: "SIN DRON", 1: "RASTREANDO", 2: "ALERTA NARANJA", 3: "ALERTA ROJA"}


# ── Utilidades ────────────────────────────────────────────────
def find_top_peaks(spectrum: np.ndarray, freqs: np.ndarray,
                   n_peaks: int = 5, min_hz_sep: float = 80.0,
                   fmin: float = 100.0, fmax: float | None = None) -> list[tuple[float, float]]:
    """Devuelve lista de (freq_hz, power) con los picos más altos separados."""
    if fmax is None:
        fmax = freqs[-1]

    order = np.argsort(spectrum)[::-1]
    picked: list[tuple[float, float]] = []
    for idx in order:
        f = float(freqs[idx])
        if f < fmin or f > fmax:
            continue
        if all(abs(f - pf) >= min_hz_sep for pf, _ in picked):
            picked.append((f, float(spectrum[idx])))
            if len(picked) >= n_peaks:
                break
    return picked


def hz_label(f: float) -> str:
    return f"{f/1000:.1f} kHz" if f >= 1000 else f"{f:.0f} Hz"


def color_para(v: float, umbral_rojo: float = H_UMBRAL) -> str:
    if v >= umbral_rojo: return "#ff3b3b"
    if v >= 0.35:        return "#ffa500"
    if v >= 0.20:        return "#ffe066"
    return "#2ca02c"


# ── Hilo de audio ─────────────────────────────────────────────
class AudioWorker(threading.Thread):
    def __init__(self, detector: DroneDetector, device, hop_s: float = 0.5,
                 max_lag_s: float = 1.0):
        super().__init__(daemon=True)
        self.detector = detector
        self.device   = device
        self.hop_s    = hop_s
        self.max_lag_s = max_lag_s
        self.result_lock = threading.Lock()
        self.latest: dict | None = None
        self.stop_flag = threading.Event()
        self.error: str | None = None
        self._mic: au.MicStream | None = None

    def stop(self) -> None:
        """Corta la captura sin esperar a que termine la ventana en curso."""
        self.stop_flag.set()
        if self._mic is not None:
            self._mic.stop()

    def run(self):
        try:
            with au.MicStream(device=self.device, samplerate=SR, blocksize=BLOCK,
                              hop=int(round(self.hop_s * SR)),
                              max_lag_s=self.max_lag_s) as mic:
                self._mic = mic
                for segundo in mic.windows():
                    if self.stop_flag.is_set():
                        break
                    t0 = time.perf_counter()
                    res = self.detector.analyze(segundo)
                    res["latency_ms"] = (time.perf_counter() - t0) * 1000
                    res["raw_pcm"]    = segundo
                    with self.result_lock:
                        self.latest = res
        except Exception as exc:
            self.error = f"Error de captura: {exc}"

    def snapshot(self) -> dict | None:
        with self.result_lock:
            return self.latest


# ── Pestaña 1: detección ──────────────────────────────────────
def tab_deteccion(fig, cfg: DetectorConfig):
    from matplotlib.gridspec import GridSpec
    from matplotlib.patches import Rectangle

    gs = GridSpec(2, 1, figure=fig, height_ratios=[2.7, 1.3], hspace=0.45,
                  left=0.07, right=0.97, top=0.95, bottom=0.10)

    ax = fig.add_subplot(gs[0])
    ax.set_xlim(0, 1); ax.set_ylim(0, 1); ax.axis("off")

    txt_p   = ax.text(0.0, 0.92, "P = 0.00", va="center", ha="left",
                      fontsize=30, fontweight="bold", color="#2ca02c")
    txt_ema = ax.text(0.5, 0.92, "EMA 0.00", va="center", ha="center",
                      fontsize=15, fontweight="bold", color=TEXTO)
    txt_est = ax.text(1.0, 0.92, "SIN DRON", va="center", ha="right",
                      fontsize=21, fontweight="bold", color="#2ca02c")

    def medidor(y, alto):
        ax.add_patch(Rectangle((0, y), 1.0, alto, facecolor=PANEL,
                               edgecolor=BORDE, linewidth=1.0, zorder=1))
        r = Rectangle((0, y), 0.0, alto, facecolor="#2ca02c", edgecolor="none",
                      zorder=2)
        ax.add_patch(r)
        return r

    MY, MH = 0.64, 0.14
    barra_p = medidor(MY, MH)
    for valor, etiqueta in ((cfg.thresh_suspicion, "sospecha"),
                            (cfg.thresh_instant, "instantáneo"),
                            (cfg.thresh_trigger_fast, "disparo")):
        ax.plot([valor, valor], [MY, MY + MH], color="#8b9099", lw=1.0,
                ls=(0, (3, 3)), zorder=3)
        ax.text(valor, MY - 0.045, f"{etiqueta} {valor:.2f}", ha="center",
                va="top", fontsize=7.5, color="#8b9099", zorder=3)
    marca_ema, = ax.plot([0, 0], [MY - 0.02, MY + MH + 0.02], color="#ffffff",
                         lw=2.0, zorder=4)

    manda = {"model": ("  ← decide la alerta", ""),
             "harmonic": ("", "  ← decide la alerta"),
             "both": ("  ← decide junto con H", "  ← decide junto con p")}[cfg.decision]
    ax.text(0.0, MY - 0.045, "modelo (red neuronal)" + manda[0], fontsize=8,
            color=TEXTO if manda[0] else APAGADO, va="top", ha="left")

    txt_h  = ax.text(0.0, 0.36, "H = 0.00", va="center", ha="left",
                     fontsize=24, fontweight="bold", color="#2ca02c")
    txt_hd = ax.text(1.0, 0.36, "", va="center", ha="right", fontsize=10.5,
                     color="#9aa0a6")
    M2Y, M2H = 0.10, 0.14
    barra_h = medidor(M2Y, M2H)
    ax.plot([H_UMBRAL, H_UMBRAL], [M2Y, M2Y + M2H], color="#8b9099", lw=1.0,
            ls=(0, (3, 3)), zorder=3)
    ax.text(H_UMBRAL, M2Y - 0.045, f"parecido a dron {H_UMBRAL:.2f}", ha="center",
            va="top", fontsize=7.5, color="#8b9099", zorder=3)
    ax.text(0.0, M2Y - 0.045,
            "peine armónico — sin modelo, independiente del volumen" + manda[1],
            fontsize=8, color=TEXTO if manda[1] else APAGADO, va="top", ha="left")
    txt_info = ax.text(0.0, -0.06, "", va="top", ha="left", fontsize=10,
                       color="#9aa0a6")

    axw = fig.add_subplot(gs[1])
    axw.set_title("Forma de onda (1 s, envolvente del micrófono)", fontsize=10,
                  loc="left", color="#ccc")
    axw.set_xlim(0, 1); axw.set_ylim(-0.05, 0.05)
    axw.set_xlabel("s", fontsize=8); axw.set_ylabel("amp", fontsize=8)
    axw.axhline(0, color="#444", lw=0.4)
    NB = 400
    t_ds = np.linspace(0, 1, NB)
    l_hi, = axw.plot(t_ds, np.zeros(NB), lw=0.4, color=AZUL)
    l_lo, = axw.plot(t_ds, np.zeros(NB), lw=0.4, color=AZUL)
    relleno = axw.fill_between(t_ds, 0, 0, color=AZUL, alpha=0.35)
    txt_pico = axw.text(1.0, 1.06, "", transform=axw.transAxes, ha="right",
                        va="bottom", fontsize=8.5, color="#aaa")
    txt_clip = axw.text(0.45, 1.06, "", transform=axw.transAxes, ha="left",
                        va="bottom", fontsize=9, color="#ff3b3b", fontweight="bold")

    def envolvente(x):
        bs = max(1, len(x) // NB)
        t = x[:bs * NB].reshape(NB, bs)
        return t.max(axis=1), t.min(axis=1)

    def update(snap, worker):
        p, ema, alrt = snap["p"], snap["ema"], snap["alerta"]
        c_alerta = COLOR_ALERTA[alrt]
        c_p = color_para(p, cfg.thresh_trigger_fast)

        barra_p.set_width(p); barra_p.set_facecolor(c_p)
        txt_p.set_text(f"P = {p:.2f}"); txt_p.set_color(c_p)
        txt_est.set_text(ALERT_TXT[alrt]); txt_est.set_color(c_alerta)
        marca_ema.set_xdata([ema, ema])
        txt_ema.set_text(f"EMA {ema:.2f}"); txt_ema.set_color(c_alerta)

        harm = snap.get("harmonic")
        if harm is None:
            barra_h.set_width(0.0)
            txt_h.set_text("H = —"); txt_h.set_color(APAGADO)
            txt_hd.set_text("peine armónico desactivado")
        else:
            c_h = color_para(harm.score)
            barra_h.set_width(harm.score); barra_h.set_facecolor(c_h)
            txt_h.set_text(f"H = {harm.score:.2f}"); txt_h.set_color(c_h)
            if harm.score < 0.15:
                txt_hd.set_text("sin estructura armónica estable")
            else:
                f0 = f"{harm.f0_hz:.0f} Hz" if np.isfinite(harm.f0_hz) else "—"
                txt_hd.set_text(
                    f"paso de pala {f0}   ·   {harm.n_harmonics} armónicos   ·   "
                    f"{harm.hnr_db:.0f} dB sobre el fondo   ·   "
                    f"estabilidad {harm.stability*100:.0f} %")

        perdidas = ""
        mic = worker._mic
        if mic is not None and (mic.dropped_blocks or mic.skipped_windows):
            perdidas = (f"   ·   descartado: {mic.dropped_blocks} bloques, "
                        f"{mic.skipped_windows} ventanas")
        if cfg.agc:
            txt_info.set_text(
                f"sobre el fondo {snap['over_floor_db']:+.1f} dB   ·   "
                f"fondo de sala {snap['floor_db']:+.1f} dBFS   ·   "
                f"AGC {snap['gain_db']:+.1f} dB   ·   "
                f"gate {'abierto' if snap['valid'] else 'cerrado'}   ·   "
                f"{snap['latency_ms']:.0f} ms" + perdidas)
        else:
            txt_info.set_text(
                f"dBFS {snap['dbfs']:+.1f}   ·   "
                f"gate {'abierto' if snap['valid'] else 'cerrado'}   ·   "
                f"ganancia {cfg.mic_gain:g}   ·   "
                f"{snap['latency_ms']:.0f} ms" + perdidas)

        raw = snap["raw_pcm"]
        hi, lo = envolvente(raw)
        l_hi.set_ydata(hi); l_lo.set_ydata(lo)
        relleno.set_verts([np.concatenate([np.column_stack([t_ds, hi]),
                                           np.column_stack([t_ds[::-1], lo[::-1]])])])
        pico = float(np.abs(raw).max()) if raw.size else 0.0
        span = max(pico * 1.45, 0.02)
        axw.set_ylim(-span, span)
        txt_pico.set_text(f"pico {pico:.4f} ({20*np.log10(pico+1e-12):+.1f} dBFS)")
        txt_clip.set_text("⚠ CLIPPING" if pico >= 0.995 else "")

    return update


# ── Pestaña 2: espectro ───────────────────────────────────────
def tab_espectro(fig, params, n_peaks, fmax):
    from matplotlib.gridspec import GridSpec
    gs = GridSpec(2, 1, figure=fig, height_ratios=[1.0, 1.8], hspace=0.35,
                  left=0.08, right=0.93, top=0.92, bottom=0.08)

    n_bins = params.n_fft // 2 + 1
    freqs = np.linspace(0, SR / 2.0, n_bins)
    vis = min(n_bins, int(np.searchsorted(freqs, fmax)) + 1)

    axf = fig.add_subplot(gs[0])
    axf.set_title("Espectro de frecuencias (media del segundo) — intensidad en dB",
                  fontsize=10, loc="left", color="#ccc")
    axf.set_xlim(0, fmax); axf.set_ylim(-80, 0); axf.grid(True, alpha=0.15)
    axf.set_xlabel("Hz", fontsize=8); axf.set_ylabel("dB", fontsize=8)
    linea, = axf.plot(freqs[:vis], np.full(vis, -80.0), lw=0.9, color=AZUL)
    marcas_f = [axf.axvline(0, color="#ff3b3b", lw=0.7, alpha=0.55, ls=":",
                            visible=False) for _ in range(n_peaks)]
    txt_pf = axf.text(0.995, 0.06, "", transform=axf.transAxes, ha="right",
                      va="bottom", fontsize=8.5, color="#ff8888",
                      bbox=dict(facecolor="#000000cc", edgecolor="none", pad=2.0))

    axs = fig.add_subplot(gs[1])
    axs.set_title(f"Espectrograma STFT ({params.target_frames} frames × {n_bins} bins) "
                  "— picos marcados en rojo", fontsize=10, loc="left", color="#ccc")
    im = axs.imshow(np.full((vis, params.target_frames), -80.0, dtype=np.float32),
                    origin="lower", aspect="auto",
                    extent=[0.0, 1.0, 0.0, float(freqs[vis - 1])],
                    vmin=-60, vmax=0, cmap="magma")
    axs.set_ylim(0, fmax)
    axs.set_xlabel("s", fontsize=8); axs.set_ylabel("Hz", fontsize=8)
    cb = fig.colorbar(im, ax=axs, pad=0.01, shrink=0.85); cb.set_label("dB", fontsize=8)
    marcas_s = [axs.axhline(0, color="#ff3b3b", lw=0.9, alpha=0.75, ls="--",
                            visible=False) for _ in range(n_peaks)]
    txt_ps = axs.text(0.995, 0.96, "", transform=axs.transAxes, ha="right",
                      va="top", fontsize=8.5, color="#ff8888",
                      bbox=dict(facecolor="#000000cc", edgecolor="none", pad=2.0))

    def update(snap, worker):
        power = snap["power"]
        pdb = np.clip(10.0 * np.log10(np.maximum(power, 1e-10)), -80, None)
        mx = pdb.max()
        off = mx if mx > -80 else 0
        pdb = pdb - off
        im.set_data(pdb[:, :vis].T)

        media = power.mean(axis=0)
        mdb = np.clip(10.0 * np.log10(np.maximum(media, 1e-10)) - off, -80, 0)
        linea.set_ydata(mdb[:vis])

        picos = find_top_peaks(media, snap["freqs_hz"], n_peaks=n_peaks, fmax=fmax)
        for i in range(n_peaks):
            hay = i < len(picos)
            marcas_f[i].set_visible(hay); marcas_s[i].set_visible(hay)
            if hay:
                marcas_f[i].set_xdata([picos[i][0], picos[i][0]])
                marcas_s[i].set_ydata([picos[i][0], picos[i][0]])
        etiquetas = "  ".join(hz_label(f) for f, _ in sorted(picos, key=lambda q: q[0]))
        texto = f"picos: {etiquetas}" if etiquetas else "sin picos"
        txt_pf.set_text(texto); txt_ps.set_text(texto)

    return update


# ── Pestaña 3: MFCC ───────────────────────────────────────────
def tab_mfcc(fig, params):
    from matplotlib.gridspec import GridSpec
    gs = GridSpec(2, 1, figure=fig, height_ratios=[1.0, 1.2], hspace=0.40,
                  left=0.08, right=0.93, top=0.92, bottom=0.08)

    axm = fig.add_subplot(gs[0])
    axm.set_title(f"Energías del banco de filtros mel ({params.n_mels} bandas, dB) "
                  "— lo que entra al DCT", fontsize=10, loc="left", color="#ccc")
    x = np.arange(params.n_mels)
    barras = axm.bar(x, np.full(params.n_mels, -80.0), width=0.9, color=AZUL,
                     edgecolor="none")
    axm.set_xlim(-0.5, params.n_mels - 0.5); axm.set_ylim(-80, 20)
    linea_base = axm.axhline(0, color="#555", lw=0.5)
    axm.set_xlabel("banda mel", fontsize=8); axm.set_ylabel("dB", fontsize=8)

    axc = fig.add_subplot(gs[1])
    axc.set_title(f"Matriz MFCC {params.target_frames}×{params.pad_to} "
                  "— el tensor exacto que recibe la red", fontsize=10, loc="left",
                  color="#ccc")
    im = axc.imshow(np.zeros((params.pad_to, params.target_frames)), origin="lower",
                    aspect="auto", cmap="viridis",
                    extent=[0, 1, -0.5, params.pad_to - 0.5])
    axc.set_xlabel("s", fontsize=8); axc.set_ylabel("coeficiente", fontsize=8)
    cb = fig.colorbar(im, ax=axc, pad=0.01, shrink=0.85)
    cb.set_label("valor MFCC", fontsize=8)
    txt = axc.text(0.005, 0.97, "", transform=axc.transAxes, ha="left", va="top",
                   fontsize=8.5, color=TEXTO,
                   bbox=dict(facecolor="#000000cc", edgecolor="none", pad=2.0))

    def update(snap, worker):
        mel = snap["mel_log_db"].mean(axis=0)
        # Con el preset "training" el suelo de dB es relativo al maximo, asi que
        # los valores no caen en un rango fijo: se escala y se colorea segun el
        # recorrido que haya en este segundo.
        lo, hi = float(mel.min()), float(mel.max())
        margen = max(5.0, 0.08 * (hi - lo))
        axm.set_ylim(lo - margen, hi + margen)
        rango = max(hi - lo, 1e-6)
        for b, v in zip(barras, mel):
            b.set_height(v)
            rel = (v - lo) / rango
            b.set_color("#ff3b3b" if rel >= 0.75 else "#ffa500" if rel >= 0.5
                        else AZUL if rel >= 0.25 else "#3a6a8a")
        linea_base.set_ydata([lo - margen, lo - margen])
        m = snap["mfcc"]
        im.set_data(m.T)
        # c0 es la energia y vale mucho mas que el resto: si entra en la escala
        # de color aplasta toda la textura. Se escala con c1..c19 y c0 satura.
        resto = m[:, 1:]
        im.set_clim(vmin=float(np.percentile(resto, 1)),
                    vmax=float(np.percentile(resto, 99)))
        txt.set_text(f"c0 (energía) medio {m[:, 0].mean():.1f}, fuera de escala   ·   "
                     f"c1..c19 en [{resto.min():.0f}, {resto.max():.0f}]   ·   "
                     f"las columnas 18 y 19 no llegan a la salida de la red")

    return update


# ── Pestaña 4: armónicos ──────────────────────────────────────
def tab_armonicos(fig):
    ax = fig.add_subplot(111)
    fig.subplots_adjust(left=0.08, right=0.97, top=0.86, bottom=0.12)
    ax.set_title("Residuo espectral: cuánto sobresale cada frecuencia sobre su propio "
                 "fondo\n(esto es lo que mide H — restar el fondo elimina el volumen)",
                 fontsize=10, loc="left", color="#ccc")
    ax.set_xlim(0, 3000); ax.set_ylim(-5, 40); ax.grid(True, alpha=0.15)
    ax.set_xlabel("Hz", fontsize=8); ax.set_ylabel("dB sobre el fondo", fontsize=8)
    linea, = ax.plot([], [], lw=0.8, color=AZUL)
    ax.axhline(3.0, color="#8b9099", lw=0.8, ls=(0, (3, 3)))
    ax.text(20, 3.6, "umbral de armónico (3 dB)", fontsize=7.5, color="#8b9099",
            ha="left")
    MAX_ARM = 30
    marcas = [ax.axvline(0, color="#ff3b3b", lw=0.9, alpha=0.7, ls="--",
                         visible=False) for _ in range(MAX_ARM)]
    txt = ax.text(0.995, 0.96, "", transform=ax.transAxes, ha="right", va="top",
                  fontsize=10, color=TEXTO,
                  bbox=dict(facecolor="#000000cc", edgecolor="none", pad=3.0))

    def update(snap, worker):
        h = snap.get("harmonic")
        if h is None:
            txt.set_text("peine armónico desactivado (--no-harmonic)")
            return
        linea.set_data(h.freqs_hz, h.residual_db)
        for i, m in enumerate(marcas):
            hay = i < len(h.harmonic_freqs)
            m.set_visible(hay)
            if hay:
                m.set_xdata([h.harmonic_freqs[i], h.harmonic_freqs[i]])
        f0 = f"{h.f0_hz:.1f} Hz" if np.isfinite(h.f0_hz) else "—"
        txt.set_text(f"H = {h.score:.2f}    f0 = {f0}    "
                     f"{h.n_harmonics} armónicos    "
                     f"{h.hnr_db:.1f} dB    estabilidad {h.stability*100:.0f} %")
        txt.set_color(color_para(h.score))

    return update


# ── Pestaña 5: workflow ───────────────────────────────────────
PASOS_WORKFLOW = [
    # (clave, titulo, subtitulo fijo, columna, fila)
    ("captura", "1 · CAPTURA",          "mic → 44100 Hz · ventana 1 s",  "c", 0),
    ("hpf",     "2 · PASO ALTO",        "IIR 1er orden  α = 0.9",        "c", 1),
    ("agc",     "3 · AGC",              "sigue el suelo de ruido",       "i", 0),
    ("puerta",  "4 · PUERTA",           "¿sobresale del fondo?",         "i", 1),
    ("tanh",    "5 · SOFT CLIP",        "tanh(x × 15)",                  "i", 2),
    ("stft",    "6 · STFT",             "n_fft 2048 · hop 442 · Hann",   "i", 3),
    ("mel",     "7 · BANCO MEL",        "64 filtros Slaney",             "i", 4),
    ("db",      "8 · POWER → dB",       "ref=1.0 · suelo = máx − 80",    "i", 5),
    ("dct",     "9 · DCT-II ortho",     "→ 20 coeficientes",             "i", 6),
    ("modelo",  "10 · RED NEURONAL",    "Conv16→Conv32→D64→D1",          "i", 7),
    ("hstft",   "11 · STFT FINA",       "n_fft 8192 → 5.4 Hz/bin",       "d", 0),
    ("fondo",   "12 · FONDO ESPECTRAL", "percentil 25 por bloques",      "d", 1),
    ("peine",   "13 · BUSCAR f0",       "peine 60–400 Hz · mediana",     "d", 2),
    ("estab",   "14 · ESTABILIDAD",     "¿el f0 aguanta el segundo?",    "d", 3),
    ("h",       "15 · H",               "parecido con un dron",          "d", 4),
    ("ema",     "16 · EMA",             "sube 0.60 / baja 0.15",         "c", 2),
    ("alerta",  "17 · ALERTA",          "persistencia 3 ticks",          "c", 3),
]

# Cuantos valores guarda cada minigrafica
N_HIST_WORKFLOW = 60

# Escala fija para los valores que ya viven en un rango conocido; el resto se
# escala solo con su propio minimo y maximo recientes.
ESCALAS_WORKFLOW = {
    "modelo": (0.0, 1.0),
    "h":      (0.0, 1.0),
    "ema":    (0.0, 1.0),
    "estab":  (0.0, 1.0),
    "alerta": (0.0, 3.0),
    "peine":  (60.0, 400.0),
}


def tab_workflow(fig, cfg: DetectorConfig):
    """
    Cada paso del algoritmo en una caja, con sus parámetros fijos, su valor en
    vivo y una minigráfica con el historial reciente de ese valor.

    La rama izquierda es el camino del modelo; la derecha, el del peine
    armónico. Las dos salen del mismo paso alto y confluyen en la decisión.
    """
    from collections import deque
    from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle

    ax = fig.add_subplot(111)
    ax.set_xlim(0, 1); ax.set_ylim(0, 1); ax.axis("off")
    fig.subplots_adjust(left=0.012, right=0.988, top=0.985, bottom=0.015)

    ANCHO, ALTO = 0.27, 0.070
    XC, XI, XD = 0.365, 0.035, 0.695
    Y_CENTRO = {0: 0.920, 1: 0.828, 2: 0.118, 3: 0.026}
    y_rama_i = [0.722 - i * 0.086 for i in range(8)]
    y_rama_d = [0.722 - i * 0.138 for i in range(5)]

    # Geometría de la minigráfica dentro de la caja (esquina inferior derecha)
    SP_W, SP_H = 0.092, 0.026
    SP_DX, SP_DY = ANCHO - SP_W - 0.008, 0.009

    cajas, textos, sparks, hist, pos = {}, {}, {}, {}, {}
    for clave, titulo, sub, col, fila in PASOS_WORKFLOW:
        if col == "c":
            x, y = XC, Y_CENTRO[fila]
        elif col == "i":
            x, y = XI, y_rama_i[fila]
        else:
            x, y = XD, y_rama_d[fila]
        pos[clave] = (x + ANCHO / 2, y + ALTO / 2)

        caja = FancyBboxPatch((x, y), ANCHO, ALTO,
                              boxstyle="round,pad=0.004,rounding_size=0.010",
                              facecolor=PANEL, edgecolor=BORDE, linewidth=1.2,
                              zorder=2)
        ax.add_patch(caja)
        ax.text(x + 0.010, y + ALTO - 0.020, titulo, fontsize=8.5,
                fontweight="bold", color=TEXTO, va="center", zorder=4)
        ax.text(x + 0.010, y + 0.020, sub, fontsize=7, color=APAGADO,
                va="center", zorder=4)
        textos[clave] = ax.text(x + ANCHO - 0.008, y + ALTO - 0.020, "—",
                                fontsize=9, fontweight="bold", color=APAGADO,
                                va="center", ha="right", zorder=4)

        # Fondo de la minigráfica + línea + punto en el valor actual
        sx, sy = x + SP_DX, y + SP_DY
        ax.add_patch(Rectangle((sx, sy), SP_W, SP_H, facecolor="#0b0c0e",
                               edgecolor="#2c2f34", linewidth=0.7, zorder=3))
        linea, = ax.plot([], [], lw=1.2, color=AZUL, zorder=5,
                         solid_joinstyle="round")
        punto, = ax.plot([], [], marker="o", markersize=2.6, color=AZUL, zorder=6)
        sparks[clave] = {"rect": (sx, sy, SP_W, SP_H), "linea": linea,
                         "punto": punto}
        hist[clave] = deque(maxlen=N_HIST_WORKFLOW)
        cajas[clave] = caja

    def flecha(a, b, curva=0.0):
        ax.add_patch(FancyArrowPatch(
            pos[a], pos[b], arrowstyle="-|>", mutation_scale=11,
            color="#5a6068", linewidth=1.1, zorder=1,
            connectionstyle=f"arc3,rad={curva}", shrinkA=22, shrinkB=22))

    rama_i = ["agc", "puerta", "tanh", "stft", "mel", "db", "dct", "modelo"]
    rama_d = ["hstft", "fondo", "peine", "estab", "h"]
    flecha("captura", "hpf")
    flecha("hpf", "agc", curva=0.2)
    flecha("hpf", "hstft", curva=-0.2)
    for a, b in zip(rama_i, rama_i[1:]):
        flecha(a, b)
    for a, b in zip(rama_d, rama_d[1:]):
        flecha(a, b)
    flecha("modelo", "ema", curva=0.2)
    flecha("h", "ema", curva=-0.2)
    flecha("ema", "alerta")

    ax.text(XI + ANCHO / 2, 0.800, "rama del MODELO", fontsize=8.5,
            color="#7aa7c7", ha="center", fontweight="bold")
    ax.text(XD + ANCHO / 2, 0.800, "rama del PEINE ARMÓNICO", fontsize=8.5,
            color="#c7a77a", ha="center", fontweight="bold")

    def pintar_spark(clave, color):
        """Dibuja el historial dentro del rectángulo de la caja."""
        datos = [v for v in hist[clave] if v is not None and np.isfinite(v)]
        s = sparks[clave]
        if len(datos) < 2:
            s["linea"].set_data([], []); s["punto"].set_data([], [])
            return
        sx, sy, sw, sh = s["rect"]
        y = np.asarray(datos, dtype=float)

        rango = ESCALAS_WORKFLOW.get(clave)
        if rango is None:                       # escala automática
            lo, hi = float(y.min()), float(y.max())
            if hi - lo < 1e-9:
                lo, hi = lo - 0.5, hi + 0.5
        else:
            lo, hi = rango
            y = np.clip(y, lo, hi)

        xs = np.linspace(sx + 0.002, sx + sw - 0.002, len(y))
        ys = sy + 0.002 + (y - lo) / (hi - lo) * (sh - 0.004)
        s["linea"].set_data(xs, ys); s["linea"].set_color(color)
        s["punto"].set_data([xs[-1]], [ys[-1]]); s["punto"].set_color(color)

    def update(snap, worker):
        mic = worker._mic
        harm = snap.get("harmonic")
        abierta = snap["valid"]

        def poner(clave, texto, valor=None, color=TEXTO):
            textos[clave].set_text(texto)
            textos[clave].set_color(color)
            hist[clave].append(valor)
            pintar_spark(clave, color if color != TEXTO else AZUL)

        raw = snap["raw_pcm"]
        pico_raw = float(np.abs(raw).max()) if raw.size else 0.0
        tasa = mic.capture_rate if mic else SR
        poner("captura", f"{tasa} → {SR} Hz", pico_raw)
        poner("hpf", f"{snap['dbfs_in']:+.1f} dBFS", snap["dbfs_in"])

        if cfg.agc:
            poner("agc", f"{snap['gain_db']:+.1f} dB · fondo {snap['floor_db']:+.0f}",
                  snap["gain_db"])
            poner("puerta",
                  f"{snap['over_floor_db']:+.1f} / +{cfg.gate_over_floor_db:g} dB",
                  snap["over_floor_db"], "#2ca02c" if abierta else APAGADO)
        else:
            poner("agc", f"fija ×{cfg.mic_gain:g}", cfg.mic_gain)
            poner("puerta", f"{snap['dbfs']:+.0f} / {cfg.silence_db:+.0f} dB",
                  snap["dbfs"], "#2ca02c" if abierta else APAGADO)

        pico = float(np.abs(snap["waveform"]).max())
        poner("tanh", f"pico {pico:.2f}", pico)
        pot_db = 10.0 * np.log10(max(float(snap["power"].max()), 1e-12))
        poner("stft", f"{snap['power'].shape[0]}×{snap['power'].shape[1]}", pot_db)
        mel = snap["mel_log_db"]
        poner("mel", f"{mel.shape[1]} bandas", float(mel.mean()))
        poner("db", f"[{mel.min():.0f}, {mel.max():.0f}]", float(mel.max()))
        m = snap["mfcc"]
        poner("dct", f"c0 {m[:, 0].mean():+.0f}", float(m[:, 0].mean()))
        poner("modelo", f"P = {snap['p']:.2f}" if abierta else "en pausa",
              snap["p"] if abierta else None,
              color_para(snap["p"], cfg.thresh_trigger_fast) if abierta else APAGADO)

        if harm is None:
            for k in ("hstft", "fondo", "peine", "estab", "h"):
                poner(k, "desactivado", None, APAGADO)
        else:
            poner("hstft", f"{len(harm.freqs_hz)} bins", float(harm.hnr_db))
            poner("fondo", f"máx {harm.residual_db.max():.0f} dB",
                  float(harm.residual_db.max()))
            f0 = f"{harm.f0_hz:.0f} Hz" if np.isfinite(harm.f0_hz) else "—"
            poner("peine", f"f0 {f0} · {harm.n_harmonics} arm",
                  float(harm.f0_hz) if np.isfinite(harm.f0_hz) else None)
            poner("estab", f"{harm.stability*100:.0f} %", float(harm.stability))
            poner("h", f"H = {harm.score:.2f}", float(harm.score),
                  color_para(harm.score))

        quien = {"harmonic": "H", "model": "P", "both": "mín(P,H)"}[cfg.decision]
        poner("ema", f"{quien} → {snap['ema']:.2f}", float(snap["ema"]),
              COLOR_ALERTA[snap["alerta"]])
        poner("alerta", ALERT_TXT[snap["alerta"]], float(snap["alerta"]),
              COLOR_ALERTA[snap["alerta"]])

        # Resaltar la rama que manda
        for clave in ("modelo", "h"):
            manda = ((clave == "h" and cfg.decision != "model") or
                     (clave == "modelo" and cfg.decision != "harmonic"))
            cajas[clave].set_edgecolor(TEXTO if manda else BORDE)
            cajas[clave].set_linewidth(2.0 if manda else 1.2)

    return update


# ── Ventana principal ─────────────────────────────────────────
def run_gui(device, n_peaks: int, fmax_display: float, mfcc_params,
            detector_cfg: DetectorConfig, refresh_ms: int = 500,
            hop_s: float = 0.5, max_lag_s: float = 1.0,
            con_workflow: bool = True) -> None:
    import matplotlib
    import matplotlib.style
    matplotlib.use("TkAgg")
    matplotlib.style.use("dark_background")
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    from matplotlib.figure import Figure

    detector = DroneDetector(config=detector_cfg, mfcc_params=mfcc_params)
    worker   = AudioWorker(detector, device, hop_s=hop_s, max_lag_s=max_lag_s)
    worker.start()

    root = tk.Tk()
    root.title("Detector de drones — Portenta H7 (Windows)")
    root.configure(bg=FONDO)
    root.geometry("1280x900")

    estilo = ttk.Style()
    try:
        estilo.theme_use("clam")
    except tk.TclError:
        pass
    estilo.configure("TNotebook", background=FONDO, borderwidth=0)
    estilo.configure("TNotebook.Tab", background=PANEL, foreground=APAGADO,
                     padding=(16, 7), borderwidth=0)
    estilo.map("TNotebook.Tab", background=[("selected", "#24262a")],
               foreground=[("selected", TEXTO)])
    estilo.configure("TFrame", background=FONDO)

    nb = ttk.Notebook(root)
    nb.pack(fill=tk.BOTH, expand=True)

    constructores = [
        ("Detección", lambda f: tab_deteccion(f, detector_cfg)),
        ("Espectro",  lambda f: tab_espectro(f, mfcc_params, n_peaks, fmax_display)),
        ("MFCC",      lambda f: tab_mfcc(f, mfcc_params)),
        ("Armónicos", lambda f: tab_armonicos(f)),
    ]
    if con_workflow:
        constructores.append(("Workflow", lambda f: tab_workflow(f, detector_cfg)))

    pestanas = []
    for nombre, constructor in constructores:
        marco = ttk.Frame(nb)
        nb.add(marco, text=nombre)
        fig = Figure(figsize=(12.6, 8.4), facecolor=FONDO)
        canvas = FigureCanvasTkAgg(fig, master=marco)
        canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)
        pestanas.append({"canvas": canvas, "update": constructor(fig)})

    barra = tk.Label(root, text="esperando audio...", bg=FONDO, fg=APAGADO,
                     anchor="w", font=("Segoe UI", 9))
    barra.pack(fill=tk.X, padx=10, pady=(0, 4))

    ocupado = {"v": False}

    def tick():
        if not ocupado["v"]:
            ocupado["v"] = True
            try:
                if worker.error:
                    barra.configure(text=f"[ERROR] {worker.error}", fg="#ff3b3b")
                snap = worker.snapshot()
                if snap is not None:
                    # Solo se repinta la pestaña visible. Las demas se actualizan
                    # cuando el usuario las trae al frente.
                    p = pestanas[nb.index(nb.select())]
                    p["update"](snap, worker)
                    p["canvas"].draw_idle()
                    mic = worker._mic
                    barra.configure(
                        text=f"ventana 1 s cada {hop_s:g} s   ·   "
                             f"análisis {snap['latency_ms']:.0f} ms   ·   "
                             f"descartado {mic.dropped_blocks if mic else 0} bloques / "
                             f"{mic.skipped_windows if mic else 0} ventanas",
                        fg=APAGADO)
            finally:
                ocupado["v"] = False
        root.after(refresh_ms, tick)

    def al_cambiar_pestana(_evt):
        snap = worker.snapshot()
        if snap is not None:
            p = pestanas[nb.index(nb.select())]
            p["update"](snap, worker)
            p["canvas"].draw_idle()

    nb.bind("<<NotebookTabChanged>>", al_cambiar_pestana)

    def cerrar():
        worker.stop()
        root.quit()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", cerrar)
    root.after(refresh_ms, tick)
    try:
        root.mainloop()
    finally:
        worker.stop()


# ── Main ──────────────────────────────────────────────────────
def main() -> None:
    parser = argparse.ArgumentParser(description="Detector de drones (GUI)")
    parser.add_argument("--device", default=None,
                        help="Índice o nombre parcial del micrófono")
    parser.add_argument("--list-devices", action="store_true",
                        help="Listar entradas disponibles y salir")
    parser.add_argument("--peaks", type=int, default=6,
                        help="Número de frecuencias pico a marcar (def 6)")
    parser.add_argument("--fmax", type=float, default=8000.0,
                        help="Frecuencia máxima mostrada en el espectrograma (Hz)")
    parser.add_argument("--no-workflow", action="store_true",
                        help="No añadir la pestaña con el diagrama del algoritmo")
    parser.add_argument("--max-lag", type=float, default=1.0,
                        help="Retraso máximo tolerado en segundos. Si el análisis se "
                             "retrasa más, se descarta audio viejo (def 1.0)")
    parser.add_argument("--hop", type=float, default=0.5,
                        help="Cada cuántos segundos se reanaliza. La ventana sigue "
                             "siendo de 1 s (def 0.5)")
    parser.add_argument("--refresh", type=int, default=0,
                        help="Milisegundos entre refrescos. 0 = al ritmo de --hop")
    parser.add_argument("--decide", choices=("harmonic", "model", "both"),
                        default="harmonic",
                        help="Qué puntuación dispara las alertas (def harmonic)")
    parser.add_argument("--no-harmonic", action="store_true",
                        help="No calcular H (ahorra ~40 ms por segundo)")
    parser.add_argument("--agc", action="store_true",
                        help="Ganancia automática anclada al ruido de fondo")
    parser.add_argument("--agc-floor", type=float, default=-45.0,
                        help="Nivel al que el AGC lleva el ruido de fondo (def -45)")
    parser.add_argument("--gate-over-floor", type=float, default=6.0,
                        help="Con --agc: dB sobre el fondo para abrir la puerta (def 6)")
    parser.add_argument("--gain", type=float, default=1.0,
                        help="Ganancia fija si no usas --agc (def 1.0)")
    parser.add_argument("--gate", type=float, default=-35.0,
                        help="Umbral de silencio en dBFS sin --agc (def -35)")
    parser.add_argument("--mode", choices=sorted(PRESETS), default="training",
                        help="Pipeline MFCC. training (def) = copia exacta del "
                             "script de entrenamiento")
    args = parser.parse_args()

    if args.list_devices:
        au.print_input_devices()
        return

    params = PRESETS[args.mode]
    cfg = DetectorConfig(mic_gain=args.gain, silence_db=args.gate, agc=args.agc,
                         agc_target_floor_dbfs=args.agc_floor,
                         gate_over_floor_db=args.gate_over_floor,
                         harmonic=not args.no_harmonic, decision=args.decide)
    refresco = args.refresh or int(round(args.hop * 1000))
    print(f"[MODE] MFCC {args.mode}: n_fft={params.n_fft} n_mels={params.n_mels} "
          f"n_mfcc={params.n_mfcc} ref_max={params.ref_max}")
    print(f"[CFG]  alertas decididas por: {cfg.decision}")
    print(f"[CFG]  ventana 1 s cada {args.hop:g} s  ·  refresco {refresco} ms")
    run_gui(args.device, args.peaks, args.fmax, params, cfg, refresco, args.hop,
            args.max_lag, not args.no_workflow)


if __name__ == "__main__":
    main()
