#!/usr/bin/env python3
"""
json_to_wav.py

Convierte a WAV los JSON de audio generados por grabar_sesion.py.

Estructura esperada:
    json_test/
    |-- audio_metadata_s<ID>.json
    `-- sound/
        |-- sound_s<ID>_ord<ORDER>_right.json
        |-- sound_s<ID>_ord<ORDER>_left.json
        |-- sound_s<ID>_ord<ORDER>_back.json
        `-- sound_s<ID>_ord<ORDER>_top.json

Cada JSON de sonido debe contener exactamente 44100 valores numericos
normalizados en el intervalo [-1, 1], equivalentes a un segundo mono a
44100 Hz.

Uso:
    python json_to_wav.py --session 1
    python json_to_wav.py --session 1 --mic 2
    python json_to_wav.py --session 1 --input json_test
    python json_to_wav.py --session 1 --input json_test/sound
    python json_to_wav.py --session 1 --output wavs/session_1
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import re
import sys
import wave
from dataclasses import dataclass
from typing import Any


SAMPLE_RATE = 44100
EXPECTED_SAMPLES = 44100
SAMPLE_WIDTH = 2
N_CHANNELS = 1

MIC_ID_TO_NAME = {
    1: "right",
    2: "left",
    3: "back",
    4: "top",
}
MIC_NAME_TO_ID = {name: mic_id for mic_id, name in MIC_ID_TO_NAME.items()}

AUDIO_FILENAME_RE = re.compile(
    r"^sound_s(?P<session>\d+)_ord(?P<order>\d+)_"
    r"(?P<mic_name>right|left|back|top)\.json$"
)


@dataclass(frozen=True)
class AudioFile:
    path: str
    session_id: int
    order_number: int
    mic_id: int
    mic_name: str
    rel_timestamp: int | None = None


def resolve_layout(input_path: str, session_id: int, metadata_path: str | None):
    """
    --input puede apuntar a json_test/ o directamente a json_test/sound/.
    """
    normalized = os.path.abspath(input_path)

    if os.path.basename(os.path.normpath(normalized)).lower() == "sound":
        sound_dir = normalized
        root_dir = os.path.dirname(normalized)
    else:
        root_dir = normalized
        sound_dir = os.path.join(root_dir, "sound")

    metadata = (
        os.path.abspath(metadata_path)
        if metadata_path
        else os.path.join(root_dir, f"audio_metadata_s{session_id}.json")
    )
    return root_dir, sound_dir, metadata


def load_json(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def discover_from_metadata(
    metadata_path: str,
    root_dir: str,
    session_id: int,
    mic_filter: int | None,
) -> list[AudioFile]:
    data = load_json(metadata_path)

    if not isinstance(data, dict):
        raise ValueError("El metadata debe ser un objeto JSON.")

    metadata_session = data.get("session_id")
    if metadata_session != session_id:
        raise ValueError(
            f"session_id del metadata={metadata_session}; "
            f"se esperaba {session_id}."
        )

    mics = data.get("mics")
    if not isinstance(mics, list):
        raise ValueError("El campo 'mics' debe ser una lista.")

    discovered: list[AudioFile] = []
    seen: set[tuple[int, int]] = set()

    for mic_obj in mics:
        if not isinstance(mic_obj, dict):
            raise ValueError("Cada elemento de 'mics' debe ser un objeto.")

        mic_id = mic_obj.get("mic_id")
        mic_name = mic_obj.get("mic_name")

        if mic_id not in MIC_ID_TO_NAME:
            raise ValueError(f"mic_id invalido en metadata: {mic_id!r}.")

        expected_name = MIC_ID_TO_NAME[mic_id]
        if mic_name != expected_name:
            raise ValueError(
                f"Mapeo invalido para mic_id={mic_id}: "
                f"mic_name={mic_name!r}; se esperaba {expected_name!r}."
            )

        if mic_filter is not None and mic_id != mic_filter:
            continue

        sounds = mic_obj.get("sound")
        if not isinstance(sounds, list):
            raise ValueError(
                f"El campo sound de mic_id={mic_id} debe ser una lista."
            )

        for sound in sounds:
            if not isinstance(sound, dict):
                raise ValueError(
                    f"Registro sound invalido para mic_id={mic_id}."
                )

            order = sound.get("order_number")
            sound_path = sound.get("sound_path")
            rel_timestamp = sound.get("rel_timestamp")

            if not isinstance(order, int) or order < 1:
                raise ValueError(
                    f"order_number invalido para mic_id={mic_id}: {order!r}."
                )
            if not isinstance(sound_path, str) or not sound_path:
                raise ValueError(
                    f"sound_path invalido para mic_id={mic_id}, orden={order}."
                )
            if rel_timestamp is not None and not isinstance(rel_timestamp, int):
                raise ValueError(
                    f"rel_timestamp invalido para mic_id={mic_id}, orden={order}."
                )

            expected_timestamp = (order - 1) * 1000
            if rel_timestamp is not None and rel_timestamp != expected_timestamp:
                print(
                    f"[WARN] mic{mic_id} orden {order}: rel_timestamp="
                    f"{rel_timestamp}; para chunks de 1 s se esperaba "
                    f"{expected_timestamp}."
                )

            expected_basename = (
                f"sound_s{session_id}_ord{order}_{mic_name}.json"
            )
            actual_basename = os.path.basename(sound_path.replace("\\", "/"))
            if actual_basename != expected_basename:
                raise ValueError(
                    f"Nombre inconsistente en metadata para mic{mic_id}, "
                    f"orden {order}: {actual_basename!r}; se esperaba "
                    f"{expected_basename!r}."
                )

            key = (mic_id, order)
            if key in seen:
                raise ValueError(
                    f"Registro duplicado para mic_id={mic_id}, orden={order}."
                )
            seen.add(key)

            normalized_relative = sound_path.replace("/", os.sep).replace(
                "\\", os.sep
            )
            full_path = os.path.abspath(os.path.join(root_dir, normalized_relative))

            # Evita que un sound_path malicioso salga de json_test/.
            if os.path.commonpath([root_dir, full_path]) != root_dir:
                raise ValueError(
                    f"sound_path fuera de la carpeta raiz: {sound_path!r}."
                )

            discovered.append(
                AudioFile(
                    path=full_path,
                    session_id=session_id,
                    order_number=order,
                    mic_id=mic_id,
                    mic_name=mic_name,
                    rel_timestamp=rel_timestamp,
                )
            )

    return sorted(
        discovered,
        key=lambda item: (item.order_number, item.mic_id),
    )


def discover_by_filename(
    sound_dir: str,
    session_id: int,
    mic_filter: int | None,
) -> list[AudioFile]:
    pattern = os.path.join(sound_dir, f"sound_s{session_id}_ord*_*.json")
    discovered: list[AudioFile] = []

    for path in glob.glob(pattern):
        basename = os.path.basename(path)
        match = AUDIO_FILENAME_RE.fullmatch(basename)
        if not match:
            print(f"[WARN] Nombre no reconocido; se ignora: {basename}")
            continue

        parsed_session = int(match.group("session"))
        order = int(match.group("order"))
        mic_name = match.group("mic_name")
        mic_id = MIC_NAME_TO_ID[mic_name]

        if parsed_session != session_id:
            continue
        if mic_filter is not None and mic_id != mic_filter:
            continue

        discovered.append(
            AudioFile(
                path=os.path.abspath(path),
                session_id=parsed_session,
                order_number=order,
                mic_id=mic_id,
                mic_name=mic_name,
            )
        )

    return sorted(
        discovered,
        key=lambda item: (item.order_number, item.mic_id),
    )


def validate_complete_orders(files: list[AudioFile], mic_filter: int | None) -> int:
    if mic_filter is not None:
        return 0

    by_order: dict[int, set[int]] = {}
    for item in files:
        by_order.setdefault(item.order_number, set()).add(item.mic_id)

    warnings = 0
    expected = set(MIC_ID_TO_NAME)
    for order, mic_ids in sorted(by_order.items()):
        if mic_ids != expected:
            missing = sorted(expected - mic_ids)
            extra = sorted(mic_ids - expected)
            print(
                f"[WARN] Orden {order} incompleto en la estructura: "
                f"faltan={missing}, extras={extra}."
            )
            warnings += 1
    return warnings


def validate_samples(samples: Any, source_name: str) -> list[float]:
    if not isinstance(samples, list):
        raise ValueError("El JSON no contiene un array.")

    if len(samples) != EXPECTED_SAMPLES:
        raise ValueError(
            f"Cantidad incorrecta: {len(samples)} muestras; "
            f"se esperaban exactamente {EXPECTED_SAMPLES}."
        )

    validated: list[float] = []
    for index, value in enumerate(samples):
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError(
                f"Muestra {index} no numerica en {source_name}: {value!r}."
            )

        numeric = float(value)
        if not math.isfinite(numeric):
            raise ValueError(
                f"Muestra {index} no finita en {source_name}: {numeric!r}."
            )
        if numeric < -1.0 or numeric > 1.0:
            raise ValueError(
                f"Muestra {index} fuera de [-1, 1] en {source_name}: "
                f"{numeric}."
            )
        validated.append(numeric)

    return validated


def floats_to_wav(
    samples: list[float],
    filepath: str,
    sample_rate: int = SAMPLE_RATE,
) -> None:
    pcm_values = []
    for sample in samples:
        # -1.0 se representa como -32768; el extremo positivo como 32767.
        if sample <= -1.0:
            pcm_values.append(-32768)
        else:
            pcm_values.append(min(32767, int(round(sample * 32767.0))))

    import struct

    payload = struct.pack(f"<{len(pcm_values)}h", *pcm_values)

    temp_path = filepath + ".tmp"
    try:
        with wave.open(temp_path, "wb") as wav_file:
            wav_file.setnchannels(N_CHANNELS)
            wav_file.setsampwidth(SAMPLE_WIDTH)
            wav_file.setframerate(sample_rate)
            wav_file.writeframes(payload)

        # Verificacion posterior del encabezado y del numero de frames.
        with wave.open(temp_path, "rb") as wav_file:
            if wav_file.getnchannels() != N_CHANNELS:
                raise ValueError("El WAV generado no es mono.")
            if wav_file.getframerate() != sample_rate:
                raise ValueError("El WAV generado tiene sample rate incorrecto.")
            if wav_file.getsampwidth() != SAMPLE_WIDTH:
                raise ValueError("El WAV generado no es PCM16.")
            if wav_file.getnframes() != EXPECTED_SAMPLES:
                raise ValueError("El WAV generado tiene numero de frames incorrecto.")

        os.replace(temp_path, filepath)
    except Exception:
        try:
            if os.path.exists(temp_path):
                os.remove(temp_path)
        except OSError:
            pass
        raise


def convert_session(
    session_id: int,
    input_path: str,
    output_dir: str,
    mic_filter: int | None = None,
    metadata_path: str | None = None,
) -> int:
    root_dir, sound_dir, resolved_metadata = resolve_layout(
        input_path, session_id, metadata_path
    )
    os.makedirs(output_dir, exist_ok=True)

    if os.path.exists(resolved_metadata):
        print(f"Metadata: {resolved_metadata}")
        files = discover_from_metadata(
            resolved_metadata,
            root_dir,
            session_id,
            mic_filter,
        )
        discovery_mode = "metadata"
    else:
        print(
            f"[WARN] No existe {resolved_metadata}. "
            "Se usara busqueda por nombre de archivo."
        )
        files = discover_by_filename(sound_dir, session_id, mic_filter)
        discovery_mode = "nombres"

    if not files:
        print(
            f"[ERROR] No se encontraron JSON de audio para la sesion "
            f"{session_id} en {sound_dir}."
        )
        return 1

    structure_warnings = validate_complete_orders(files, mic_filter)

    print(
        f"\nSesion {session_id}: {len(files)} archivo(s) "
        f"descubiertos mediante {discovery_mode}."
    )
    if mic_filter is not None:
        print(
            f"Filtro: mic{mic_filter} "
            f"({MIC_ID_TO_NAME[mic_filter]})."
        )
    print(f"Entrada audio: {sound_dir}")
    print(f"Salida WAV: {os.path.abspath(output_dir)}\n")

    converted = 0
    errors = 0

    for item in files:
        basename = os.path.basename(item.path)
        wav_name = os.path.splitext(basename)[0] + ".wav"
        wav_path = os.path.join(output_dir, wav_name)

        try:
            if not os.path.isfile(item.path):
                raise FileNotFoundError(
                    f"El archivo referenciado no existe: {item.path}"
                )

            raw_samples = load_json(item.path)
            samples = validate_samples(raw_samples, basename)
            floats_to_wav(samples, wav_path)

            duration_s = len(samples) / SAMPLE_RATE
            print(
                f"  OK ord={item.order_number} mic{item.mic_id} "
                f"({item.mic_name}): {wav_name} "
                f"({len(samples)} muestras, {duration_s:.3f}s)"
            )
            converted += 1

        except json.JSONDecodeError as exc:
            print(f"  [ERROR] {basename}: JSON invalido: {exc}")
            errors += 1
        except Exception as exc:
            print(f"  [ERROR] {basename}: {exc}")
            errors += 1

    print(f"\n{'=' * 58}")
    print(f"  Convertidos             : {converted}")
    print(f"  Errores                 : {errors}")
    print(f"  Advertencias estructura : {structure_warnings}")
    print(f"  Carpeta WAV             : {os.path.abspath(output_dir)}")
    print(f"{'=' * 58}\n")

    return 0 if errors == 0 else 2


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Convierte sound_s<ID>_ord<ORDER>_<posicion>.json "
            "a WAV mono PCM16 de 44100 Hz."
        )
    )
    parser.add_argument(
        "--session",
        type=int,
        required=True,
        help="ID de sesion, por ejemplo 1.",
    )
    parser.add_argument(
        "--mic",
        type=int,
        choices=sorted(MIC_ID_TO_NAME),
        default=None,
        help="Convertir solamente mic 1-4. Omitir para todos.",
    )
    parser.add_argument(
        "--input",
        default="json_test",
        help=(
            "Carpeta json_test o json_test/sound. "
            "Default: json_test."
        ),
    )
    parser.add_argument(
        "--metadata",
        default=None,
        help=(
            "Ruta explicita a audio_metadata_s<ID>.json. "
            "Por defecto se infiere desde --input."
        ),
    )
    parser.add_argument(
        "--output",
        default=None,
        help="Carpeta de salida. Default: wavs/session_<ID>.",
    )
    args = parser.parse_args()

    output = args.output or os.path.join(
        "wavs", f"session_{args.session}"
    )

    return convert_session(
        session_id=args.session,
        input_path=args.input,
        output_dir=output,
        mic_filter=args.mic,
        metadata_path=args.metadata,
    )


if __name__ == "__main__":
    sys.exit(main())