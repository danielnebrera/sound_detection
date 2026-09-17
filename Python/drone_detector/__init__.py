"""Detector de drones — versión Windows del firmware Portenta H7."""

from .mfcc import (
    MFCCExtractor,
    MFCCParams,
    PRESETS,
    MFCC_PARAMS,
    MFCC_PARAMS_HEADER,
    MFCC_PARAMS_LIBROSA,
    MFCC_PARAMS_FIRMWARE,
    preprocess_pcm,
    dbfs,
)
from .detector import DroneDetector, DetectorConfig, DetectionResult, ALERT_NAMES

__all__ = [
    "MFCCExtractor",
    "MFCCParams",
    "PRESETS",
    "MFCC_PARAMS",
    "MFCC_PARAMS_HEADER",
    "MFCC_PARAMS_LIBROSA",
    "MFCC_PARAMS_FIRMWARE",
    "preprocess_pcm",
    "dbfs",
    "DroneDetector",
    "DetectorConfig",
    "DetectionResult",
    "ALERT_NAMES",
]
