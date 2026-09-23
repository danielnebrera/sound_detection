#!/usr/bin/env python3
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import re
import struct
import sys
import wave
from dataclasses import dataclass
from typing import Any

SAMPLE_RATE = 44100
EXPECTED_SAMPLES = 44100
SAMPLE_WIDTH = 2
N_CHANNELS = 1

AUDIO_FILENAME_RE = re.compile(
    r"^sound_s(?P<session>\d+)_ord(?P<order>\d+)_slot(?P<slot>[01])\.json$"
)


@dataclass(frozen=True)
class AudioFile:
    path: str
    session_id: int
    order_number: int
    slot: int
    rel_timestamp: int | None = None


def resolve_layout(input_path: str, session_id: int, metadata_path: str | None):
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
    slot_filter: int | None,
) -> list[AudioFile]:
    data = load_json(metadata_path)

    if not isinstance(data, dict):
        raise ValueError("El metadata debe ser un objeto JSON.")

    if data.get("session_id") != session_id:
        raise ValueError(
            f"session_id del metadata={data.get('session_id')}; "
            f"se esperaba {session_id}."
        )

    slots = data.get("slots")
    if not isinstance(slots, list):
        raise ValueError("El campo 'slots' debe ser una lista.")

    discovered: list[AudioFile] = []
    seen: set[tuple[int, int]] = set()

    for slot_obj in slots:
        if not isinstance(slot_obj, dict):
            raise ValueError("Cada elemento de 'slots' debe ser un objeto.")

        slot = slot_obj.get("slot")
        if slot not in (0, 1):
            raise ValueError(f"slot invalido: {slot!r}")

        if slot_filter is not None and slot != slot_filter:
            continue

        sounds = slot_obj.get("sound")
        if not isinstance(sounds, list):
            raise ValueError(f"El campo sound del slot{slot} debe ser una lista.")

        for sound in sounds:
            if not isinstance(sound, dict):
                raise ValueError(f"Registro sound invalido para slot{slot}.")

            order = sound.get("order_number")
            sound_path = sound.get("sound_path")
            rel_timestamp = sound.get("rel_timestamp")

            if not isinstance(order, int) or order < 1:
                raise ValueError(f"order_number invalido para slot{slot}: {order!r}")

            if not isinstance(sound_path, str) or not sound_path:
                raise ValueError(
                    f"sound_path invalido para slot{slot}, orden={order}."
                )

            expected_basename = (
                f"sound_s{session_id}_ord{order}_slot{slot}.json"
            )
            actual_basename = os.path.basename(sound_path.replace("\\", "/"))

            if actual_basename != expected_basename:
                raise ValueError(
                    f"Nombre inconsistente para slot{slot}, orden={order}: "
                    f"{actual_basename!r}; esperado {expected_basename!r}."
                )

            key = (slot, order)
            if key in seen:
                raise ValueError(
                    f"Registro duplicado para slot={slot}, orden={order}."
                )
            seen.add(key)

            normalized_relative = sound_path.replace("/", os.sep).replace(
                "\\", os.sep
            )
            full_path = os.path.abspath(os.path.join(root_dir, normalized_relative))

            if os.path.commonpath([root_dir, full_path]) != root_dir:
                raise ValueError(f"sound_path fuera de json_test: {sound_path!r}")

            discovered.append(
                AudioFile(
                    path=full_path,
                    session_id=session_id,
                    order_number=order,
                    slot=slot,
                    rel_timestamp=rel_timestamp,
                )
            )

    return sorted(discovered, key=lambda item: (item.order_number, item.slot))


def discover_by_filename(
    sound_dir: str,
    session_id: int,
    slot_filter: int | None,
) -> list[AudioFile]:
    pattern = os.path.join(sound_dir, f"sound_s{session_id}_ord*_slot*.json")
    discovered: list[AudioFile] = []

    for path in glob.glob(pattern):
        basename = os.path.basename(path)
        match = AUDIO_FILENAME_RE.fullmatch(basename)
        if not match:
            continue

        parsed_session = int(match.group("session"))
        order = int(match.group("order"))
        slot = int(match.group("slot"))

        if parsed_session != session_id:
            continue
        if slot_filter is not None and slot != slot_filter:
            continue

        discovered.append(
            AudioFile(
                path=os.path.abspath(path),
                session_id=parsed_session,
                order_number=order,
                slot=slot,
            )
        )

    return sorted(discovered, key=lambda item: (item.order_number, item.slot))


def validate_samples(samples: Any, source_name: str) -> list[float]:
    if not isinstance(samples, list):
        raise ValueError("El JSON no contiene un array.")

    if len(samples) != EXPECTED_SAMPLES:
        raise ValueError(
            f"Cantidad incorrecta: {len(samples)} muestras; "
            f"se esperaban {EXPECTED_SAMPLES}."
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
                f"Muestra {index} fuera de [-1, 1] en {source_name}: {numeric}."
            )

        validated.append(numeric)

    return validated


def floats_to_wav(samples: list[float], filepath: str) -> None:
    pcm_values: list[int] = []

    for sample in samples:
        if sample <= -1.0:
            pcm_values.append(-32768)
        else:
            pcm_values.append(min(32767, int(round(sample * 32767.0))))

    payload = struct.pack(f"<{len(pcm_values)}h", *pcm_values)
    temp_path = filepath + ".tmp"

    try:
        with wave.open(temp_path, "wb") as wav_file:
            wav_file.setnchannels(N_CHANNELS)
            wav_file.setsampwidth(SAMPLE_WIDTH)
            wav_file.setframerate(SAMPLE_RATE)
            wav_file.writeframes(payload)

        with wave.open(temp_path, "rb") as wav_file:
            if wav_file.getnchannels() != N_CHANNELS:
                raise ValueError("El WAV generado no es mono.")
            if wav_file.getframerate() != SAMPLE_RATE:
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Convierte JSON BasicSetupMics slot0/slot1 a WAV mono PCM16 44.1 kHz."
        )
    )
    parser.add_argument("--session", type=int, required=True)
    parser.add_argument("--slot", type=int, choices=(0, 1), default=None)
    parser.add_argument("--input", default="json_test")
    parser.add_argument("--metadata", default=None)
    parser.add_argument("--output", default=None)
    args = parser.parse_args()

    output_dir = args.output or os.path.join(
        "wavs", f"session_{args.session}"
    )

    root_dir, sound_dir, metadata_path = resolve_layout(
        args.input,
        args.session,
        args.metadata,
    )
    os.makedirs(output_dir, exist_ok=True)

    try:
        if os.path.exists(metadata_path):
            print(f"Metadata: {metadata_path}")
            files = discover_from_metadata(
                metadata_path,
                root_dir,
                args.session,
                args.slot,
            )
            mode = "metadata"
        else:
            print(f"[WARN] No existe {metadata_path}; buscando por nombre.")
            files = discover_by_filename(
                sound_dir,
                args.session,
                args.slot,
            )
            mode = "nombres"

        if not files:
            print(
                f"[ERROR] No se encontraron JSON para la sesion "
                f"{args.session} en {sound_dir}."
            )
            return 1

        print(
            f"\nSesion {args.session}: {len(files)} archivo(s) "
            f"mediante {mode}."
        )
        print(f"Entrada: {sound_dir}")
        print(f"Salida : {os.path.abspath(output_dir)}\n")

        converted = 0
        errors = 0

        for item in files:
            basename = os.path.basename(item.path)
            wav_name = os.path.splitext(basename)[0] + ".wav"
            wav_path = os.path.join(output_dir, wav_name)

            try:
                samples = validate_samples(load_json(item.path), basename)
                floats_to_wav(samples, wav_path)
                print(
                    f"  OK ord={item.order_number} slot={item.slot}: "
                    f"{wav_name} ({len(samples)} muestras, 1.000s)"
                )
                converted += 1
            except Exception as exc:
                print(f"  [ERROR] {basename}: {exc}")
                errors += 1

        print("\n" + "=" * 58)
        print(f"Convertidos : {converted}")
        print(f"Errores     : {errors}")
        print(f"WAV         : {os.path.abspath(output_dir)}")
        print("=" * 58)
        return 0 if errors == 0 else 2

    except Exception as exc:
        print(f"[ERROR] {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
