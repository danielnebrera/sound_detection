"""
Detector de drones — envuelve MFCC + TFLite y replica la lógica de decisión
de CM7/Core/Src/drone_detection.c (EMA, persistencia, umbrales).
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np

from .mfcc import (MFCCExtractor, MFCC_PARAMS, MFCC_PARAMS_TRAINING,
                   MFCCParams, highpass, finish_dsp, finish_dsp_esp32,
                   preprocess_pcm, dbfs)
from .agc import AutoGain, AutoGainConfig
from .harmonic import analyze_harmonics, HarmonicConfig, HarmonicResult


# ── Intérprete TFLite (varios backends posibles) ──────────────
def _load_interpreter(model_path: str):
    try:
        from ai_edge_litert.interpreter import Interpreter  # noqa: WPS433
    except ImportError:
        try:
            from tflite_runtime.interpreter import Interpreter  # noqa: WPS433
        except ImportError:
            from tensorflow.lite import Interpreter  # noqa: WPS433
    interp = Interpreter(model_path=model_path)
    interp.allocate_tensors()
    return interp


# ── Configuración (idéntica al firmware) ──────────────────────
@dataclass
class DetectorConfig:
    # Umbrales EMA (drone_detection.c líneas 17-23)
    alpha_rise: float          = 0.60
    alpha_fall: float          = 0.15
    thresh_trigger_fast: float = 0.85
    thresh_suspicion: float    = 0.25
    thresh_instant: float      = 0.60
    ticks_persistence: int     = 3
    silence_db: float          = -35.0

    # Ganancia de entrada aplicada ANTES de la cadena DSP del firmware.
    # El firmware no la tiene (gain=1.0); en Windows compensa la diferencia
    # de sensibilidad entre el ICS-43434 y el micro del portatil.
    # Calibrala con:  python drone_detector_cli.py calibrate
    mic_gain: float            = 1.0

    # AGC lento anclado al ruido de fondo (ver agc.py). Con el activado la
    # ganancia deja de ser fija y la puerta pasa a medirse RESPECTO AL AMBIENTE,
    # no contra un nivel absoluto: "6 dB por encima del fondo de esta sala".
    agc: bool                  = False
    agc_target_floor_dbfs: float = -45.0
    gate_over_floor_db: float  = 6.0

    # Puntuacion de peine armonico (harmonic.py): mide el PARECIDO de la señal
    # con la firma de un dron, sin mirar el volumen y sin usar el modelo.
    harmonic: bool             = True

    # Que puntuacion alimenta la EMA y, por tanto, las alertas:
    #   "harmonic" -> H  (por defecto: el modelo da 1.00 con cualquier ruido)
    #   "model"    -> p  (comportamiento del firmware)
    #   "both"     -> min(p, H): tienen que coincidir los dos
    decision: str              = "harmonic"

    # Ruta al modelo (por defecto: Python/drone_mfcc_model.tflite)
    model_path: str = str(Path(__file__).resolve().parent.parent / "drone_mfcc_model.tflite")


# ── Salida por ciclo ──────────────────────────────────────────
@dataclass
class DetectionResult:
    p_channels: list[float]        # probabilidad por canal (len 1..N)
    db_channels: list[float]       # dBFS por canal
    valid_channels: list[bool]     # activos tras gate de silencio
    p_mean: float                  # promedio p sobre canales válidos
    ema: float                     # EMA actual
    alerta: int                    # 0 nada / 1 rastreando / 2 naranja / 3 roja
    persistence: int               # cuenta ticks_persistence
    h_channels: list[float] | None = None            # peine armonico por canal
    f0_channels: list[float] | None = None            # f0 estimado por canal
    over_floor_channels: list[float] | None = None   # dB sobre el fondo (con AGC)
    floor_channels: list[float] | None = None        # suelo de ruido estimado
    gain_db_channels: list[float] | None = None      # ganancia aplicada


ALERT_NAMES = {0: "SIN DRON", 1: "RASTREANDO", 2: "ALERTA NARANJA", 3: "ALERTA ROJA"}


# ── Núcleo ────────────────────────────────────────────────────
class DroneDetector:
    """
    Un detector puede procesar N canales de audio en paralelo (idéntico al
    firmware que corre Mic1 y Mic3). Alimentas 1 s de PCM por canal y llamas
    a `process()` para obtener la decisión.
    """

    def __init__(self, config: Optional[DetectorConfig] = None,
                 mfcc_params: Optional[MFCCParams] = None) -> None:
        self.cfg = config or DetectorConfig()
        self.mfcc_params = mfcc_params or MFCC_PARAMS_TRAINING
        self.mfcc = MFCCExtractor(self.mfcc_params)
        self.interp = _load_interpreter(self.cfg.model_path)
        self.in_idx  = self.interp.get_input_details()[0]["index"]
        self.out_idx = self.interp.get_output_details()[0]["index"]

        in_shape = tuple(self.interp.get_input_details()[0]["shape"])
        expected = (1, self.mfcc_params.target_frames, self.mfcc_params.pad_to, 1)
        if in_shape != expected:
            raise RuntimeError(
                f"Forma inesperada del modelo: {in_shape} (esperada {expected})"
            )

        self.ema = 0.0
        self.persistence = 0
        self._agc: dict[int, AutoGain] = {}
        self.harmonic_cfg = HarmonicConfig(
            sample_rate=self.mfcc_params.sample_rate)

    def _decision_score(self, p: float, h: float) -> float:
        """Puntuacion que gobierna EMA, persistencia y alertas."""
        modo = self.cfg.decision
        if modo == "model":
            return p
        if modo == "both":
            return min(p, h)
        return h

    def _agc_for(self, channel: int) -> AutoGain:
        """Un seguidor de ruido de fondo por canal: cada microfono tiene el suyo."""
        if channel not in self._agc:
            self._agc[channel] = AutoGain(AutoGainConfig(
                target_floor_dbfs=self.cfg.agc_target_floor_dbfs))
        return self._agc[channel]

    def reset(self) -> None:
        """Vuelve al estado inicial (EMA, persistencia y AGC a cero)."""
        self.ema = 0.0
        self.persistence = 0
        for agc in self._agc.values():
            agc.reset()

    # ── Acondicionado + puerta ───────────────────────────────
    def _condition(self, pcm_1s: np.ndarray, channel: int = 0) -> dict:
        """
        Cadena DSP del firmware con ganancia fija o con AGC, y decision de puerta.

        Sin AGC reproduce el firmware: ganancia fija y puerta contra un nivel
        absoluto. Con AGC la ganancia sigue al ruido de fondo y la puerta mide
        cuanto sobresale la ventana SOBRE ese fondo, que es una condicion que no
        depende de la sala ni del microfono.
        """
        h      = highpass(pcm_1s)          # lineal: el nivel se mide aqui
        db_in  = dbfs(h)

        if self.cfg.agc:
            gain, info = self._agc_for(channel).update(db_in)
            valid = info["over_floor_db"] >= self.cfg.gate_over_floor_db
        else:
            gain = self.cfg.mic_gain
            info = {"floor_db": float("nan"), "gain_db": 20.0 * np.log10(max(gain, 1e-9)),
                    "over_floor_db": float("nan")}
            valid = None                   # se decide abajo, con el nivel de salida

        # El entrenamiento solo aplicaba tanh(x*15); quitar la media y el
        # preenfasis los añadio drone_detection.c y NO estaban al entrenar.
        remate = (finish_dsp_esp32 if self.mfcc_params.dsp == "esp32"
                  else finish_dsp)
        y      = remate(h, gain)
        db_out = dbfs(y)
        if valid is None:
            valid = db_out >= self.cfg.silence_db

        return {"y": y, "hpf": h, "db_in": db_in, "db_out": db_out,
                "valid": bool(valid), "gain": gain, **info}

    def harmonics(self, cond: dict) -> HarmonicResult | None:
        """
        Peine armonico sobre la salida del paso alto, NO sobre `y`.

        La cadena del firmware acaba en tanh(), que es un recortador suave: genera
        armonicos donde no los habia. Analizar el peine ahi daria un parecido a
        dron falso para cualquier sonido fuerte. El paso alto es lineal y no
        inventa nada.
        """
        if not self.cfg.harmonic:
            return None
        return analyze_harmonics(cond["hpf"], self.harmonic_cfg)

    # ── Inferencia sobre 1 s de audio (un canal) ─────────────
    def _infer_channel(self, pcm_1s: np.ndarray,
                       channel: int = 0) -> tuple[float, float, bool, dict]:
        """Devuelve (p_drone, dbfs_ch, valido, info_de_acondicionado)."""
        cond = self._condition(pcm_1s, channel)
        y, db = cond["y"], cond["db_out"]
        # H se calcula SIEMPRE, abierta o cerrada la puerta: es invariante al
        # nivel, asi que tiene sentido incluso con la sala en silencio, y evita
        # que el valor desaparezca de la pantalla cada vez que baja el ruido.
        cond["harmonic"] = self.harmonics(cond)

        if not cond["valid"]:
            return 0.0, db, False, cond

        mfcc = self.mfcc.compute(y)
        tensor = mfcc.reshape(1, self.mfcc_params.target_frames, self.mfcc_params.pad_to, 1)
        self.interp.set_tensor(self.in_idx, tensor.astype(np.float32))
        self.interp.invoke()
        p = float(self.interp.get_tensor(self.out_idx).flatten()[0])
        return p, db, True, cond

    def analyze(self, pcm_1s: np.ndarray) -> dict:
        """
        Versión mono-canal con intermedios (para GUI).
        Aplica EMA / persistencia igual que `process` con un canal.
        Devuelve dict con probabilidad, EMA, alerta, potencia, mel_log_db, etc.
        """
        cond = self._condition(pcm_1s)
        y, db = cond["y"], cond["db_out"]
        full = self.mfcc.compute_full(y)
        harm = self.harmonics(cond)

        if not cond["valid"]:
            p, valid = 0.0, False
            self.ema *= (1.0 - self.cfg.alpha_fall)
            self.persistence = 0
            alerta = 0
        else:
            tensor = full["mfcc"].reshape(
                1, self.mfcc_params.target_frames, self.mfcc_params.pad_to, 1
            ).astype(np.float32)
            self.interp.set_tensor(self.in_idx, tensor)
            self.interp.invoke()
            p = float(self.interp.get_tensor(self.out_idx).flatten()[0])
            valid = True

            score = self._decision_score(p, harm.score if harm else 0.0)
            alpha = self.cfg.alpha_rise if score > self.ema else self.cfg.alpha_fall
            self.ema = self.ema * (1.0 - alpha) + score * alpha

            if (self.ema >= self.cfg.thresh_trigger_fast
                    and score > self.cfg.thresh_instant):
                self.persistence = self.cfg.ticks_persistence
                alerta = 3
            elif self.ema >= self.cfg.thresh_suspicion:
                self.persistence = min(self.persistence + 1,
                                       self.cfg.ticks_persistence)
                alerta = 2 if self.persistence >= self.cfg.ticks_persistence else 1
            else:
                self.persistence = 0
                alerta = 0

        return {
            "waveform":   y,                    # 1 s ya preprocesado
            "dbfs":       db,
            "dbfs_in":    cond["db_in"],        # nivel tras el paso alto, sin ganancia
            "gain_db":    cond["gain_db"],
            "harmonic":   harm,                 # HarmonicResult o None
            "score":      (0.0 if not cond["valid"]
                           else self._decision_score(p, harm.score if harm else 0.0)),
            "decision":   self.cfg.decision,
            "floor_db":   cond["floor_db"],     # suelo de ruido estimado (solo con AGC)
            "over_floor_db": cond["over_floor_db"],
            "p":          p,
            "valid":      valid,
            "ema":        self.ema,
            "alerta":     alerta,
            "persistence": self.persistence,
            "mfcc":       full["mfcc"],
            "mel_log_db": full["mel_log_db"],
            "power":      full["power"],
            "freqs_hz":   full["freqs_hz"],
            "times_s":    full["times_s"],
        }

    # ── Procesar N canales (1 seg por canal) ─────────────────
    def process(self, channels: list[np.ndarray]) -> DetectionResult:
        p_ch:  list[float] = []
        db_ch: list[float] = []
        ok_ch: list[bool]  = []

        over_ch: list[float] = []
        floor_ch: list[float] = []
        gain_ch: list[float] = []
        h_ch: list[float] = []
        f0_ch: list[float] = []
        for canal, pcm in enumerate(channels):
            p, db, ok, cond = self._infer_channel(pcm, canal)
            p_ch.append(p)
            db_ch.append(db)
            ok_ch.append(ok)
            over_ch.append(cond["over_floor_db"])
            floor_ch.append(cond["floor_db"])
            gain_ch.append(cond["gain_db"])
            harm = cond.get("harmonic")
            h_ch.append(harm.score if harm else 0.0)
            f0_ch.append(harm.f0_hz if harm else float("nan"))

        p_valid = [p for p, ok in zip(p_ch, ok_ch) if ok]
        if not p_valid:
            self.ema *= (1.0 - self.cfg.alpha_fall)
            self.persistence = 0
            return DetectionResult(p_ch, db_ch, ok_ch, 0.0, self.ema, 0, 0,
                                   h_ch, f0_ch, over_ch, floor_ch, gain_ch)

        p_mean = float(np.mean(p_valid))
        h_valid = [h for h, ok in zip(h_ch, ok_ch) if ok]
        h_mean  = float(np.mean(h_valid)) if h_valid else 0.0
        score   = self._decision_score(p_mean, h_mean)

        alpha  = self.cfg.alpha_rise if score > self.ema else self.cfg.alpha_fall
        self.ema = self.ema * (1.0 - alpha) + score * alpha

        # Lógica de alerta (drone_detection.c líneas 197-215)
        if (self.ema >= self.cfg.thresh_trigger_fast
                and score > self.cfg.thresh_instant):
            self.persistence = self.cfg.ticks_persistence
            alerta = 3
        elif self.ema >= self.cfg.thresh_suspicion:
            self.persistence = min(self.persistence + 1, self.cfg.ticks_persistence)
            alerta = 2 if self.persistence >= self.cfg.ticks_persistence else 1
        else:
            self.persistence = 0
            alerta = 0

        return DetectionResult(
            p_channels=p_ch, db_channels=db_ch, valid_channels=ok_ch,
            p_mean=p_mean, ema=self.ema, alerta=alerta,
            persistence=self.persistence,
            h_channels=h_ch, f0_channels=f0_ch,
            over_floor_channels=over_ch, floor_channels=floor_ch,
            gain_db_channels=gain_ch,
        )
