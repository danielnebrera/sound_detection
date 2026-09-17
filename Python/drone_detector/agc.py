"""
Control automático de ganancia lento, anclado al ruido de fondo.

El firmware usa una ganancia fija (×15 + tanh) pensada para el ICS-43434. Fuera
de ese hardware el nivel cambia con el micrófono y con la sala, y entonces el
umbral de silencio absoluto (−35 dBFS) deja de significar nada.

Aquí se estima el **suelo de ruido** del entorno y se aplica la ganancia que lo
lleva a un nivel de referencia fijo. Dos consecuencias:

  - Las características que ve el modelo dejan de depender del volumen absoluto.
  - La puerta puede expresarse como "X dB por encima del ambiente", que es una
    condición independiente de la sala.

No es un compresor: la ganancia es **una constante por ventana de 1 s** y se
mueve despacio. Un compresor con ataque/release rápidos aplastaría la envolvente
temporal, que es justo donde está la firma de un dron (sonido sostenido).
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class AutoGainConfig:
    target_floor_dbfs: float = -45.0
    """Nivel al que se lleva el suelo de ruido."""

    max_gain_db: float = 40.0
    """Tope de amplificación. Sin él, una sala muda se amplifica hasta que el
    ruido del propio conversor llena la escala."""

    min_gain_db: float = -20.0
    """Atenuación máxima, para salas muy ruidosas."""

    floor_rise_db_per_s: float = 0.7
    """Velocidad a la que el suelo estimado puede SUBIR. Baja al instante (si
    llega algo más silencioso, ese es el nuevo suelo) y sube despacio."""

    freeze_over_db: float = 6.0
    """Si la ventana sobresale más de esto sobre el suelo, se considera que hay
    un evento en curso y el suelo NO sube. Sin esta condición, un dron sostenido
    acaba convirtiéndose en el ambiente al cabo de medio minuto y deja de
    detectarse: el clásico que se muerde la cola de los AGC."""

    smooth: float = 0.35
    """Suavizado de la ganancia entre ventanas (0 = congelada, 1 = instantánea)."""


class AutoGain:
    """
    Uso, una vez por ventana de 1 s:

        agc = AutoGain()
        gain_lineal, info = agc.update(dbfs_de_la_ventana)

    `info` trae el suelo estimado y cuántos dB sobresale la ventana sobre él,
    que es la magnitud con la que conviene decidir si hay algo o no.
    """

    def __init__(self, config: AutoGainConfig | None = None) -> None:
        self.cfg = config or AutoGainConfig()
        self.floor_db: float | None = None
        self.gain_db: float = 0.0

    def reset(self) -> None:
        self.floor_db = None
        self.gain_db = 0.0

    def update(self, window_dbfs: float, window_seconds: float = 1.0) -> tuple[float, dict]:
        cfg = self.cfg

        # -- seguimiento del suelo de ruido --
        if self.floor_db is None:
            self.floor_db = window_dbfs
        elif window_dbfs < self.floor_db:
            self.floor_db = window_dbfs                       # baja al instante
        elif window_dbfs - self.floor_db < cfg.freeze_over_db:
            subida = cfg.floor_rise_db_per_s * window_seconds
            self.floor_db = min(self.floor_db + subida, window_dbfs)
        # else: hay algo sonando por encima del fondo -> el suelo se congela

        # -- ganancia que lleva ese suelo al objetivo --
        objetivo = cfg.target_floor_dbfs - self.floor_db
        objetivo = max(cfg.min_gain_db, min(cfg.max_gain_db, objetivo))
        self.gain_db += cfg.smooth * (objetivo - self.gain_db)

        sobre_fondo = window_dbfs - self.floor_db
        return 10.0 ** (self.gain_db / 20.0), {
            "floor_db":    self.floor_db,
            "gain_db":     self.gain_db,
            "over_floor_db": sobre_fondo,
        }
