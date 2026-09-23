#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from dataclasses import dataclass

try:
    import serial
except ImportError:
    print("[ERROR] Instala pyserial: pip install pyserial")
    sys.exit(1)

SAMPLE_RATE = 44100
SAMPLES_PER_CHUNK = SAMPLE_RATE
PCM16_BYTES = SAMPLES_PER_CHUNK * 2
EXPECTED_BAUD = 1000000
START_COMMAND = b"R"

OUTPUT_ROOT = "json_test"
OUTPUT_DIR = os.path.join(OUTPUT_ROOT, "sound")
METADATA_DIR = OUTPUT_ROOT

SLOTS = (0, 1)


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def sound_filename(session_id: int, order: int, slot: int) -> str:
    return f"sound_s{session_id}_ord{order}_slot{slot}.json"


def metadata_filename(session_id: int) -> str:
    return f"audio_metadata_s{session_id}.json"


def read_exact(ser: serial.Serial, count: int, timeout_s: float) -> bytes:
    data = bytearray(count)
    view = memoryview(data)
    received = 0
    deadline = time.monotonic() + timeout_s

    while received < count:
        if time.monotonic() >= deadline:
            raise TimeoutError(
                f"Timeout UART: {received}/{count} bytes recibidos"
            )

        amount = ser.readinto(view[received:])
        if amount:
            received += int(amount)

    return bytes(data)


def read_ascii_line(ser: serial.Serial, timeout_s: float = 10.0) -> str:
    deadline = time.monotonic() + timeout_s

    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue

        line = raw.decode("ascii", errors="ignore").strip()
        if line:
            print(f"[STM32] {line}")
            return line

    raise TimeoutError("Timeout esperando una linea ASCII del STM32")


def wait_for_prefix(
    ser: serial.Serial,
    prefix: str,
    timeout_s: float = 10.0,
) -> str:
    deadline = time.monotonic() + timeout_s

    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue

        line = raw.decode("ascii", errors="ignore").strip()
        if line:
            print(f"[STM32] {line}")

        if line.startswith(prefix):
            return line

    raise TimeoutError(f"Timeout esperando {prefix}")


def parse_bytes_field(line: str) -> int:
    for token in line.split():
        if token.startswith("bytes="):
            return int(token.split("=", 1)[1], 0)
    raise ValueError(f"No se encontro bytes= en: {line!r}")


def pcm16_bytes_to_normalized(payload: bytes) -> list[float]:
    if len(payload) != PCM16_BYTES:
        raise ValueError(
            f"PCM16 incompleto: {len(payload)} bytes; esperados {PCM16_BYTES}"
        )

    samples = struct.unpack(f"<{SAMPLES_PER_CHUNK}h", payload)
    return [sample / 32768.0 for sample in samples]


def save_json_atomic(data: object, path: str, *, indent: int | None = None) -> None:
    temp_path = path + ".tmp"
    with open(temp_path, "w", encoding="utf-8") as handle:
        json.dump(
            data,
            handle,
            indent=indent,
            separators=None if indent is not None else (",", ":"),
            allow_nan=False,
        )
        if indent is not None:
            handle.write("\n")
    os.replace(temp_path, path)


@dataclass
class SlotCapture:
    slot: int
    payload: bytes


def new_metadata(session_id: int) -> dict:
    return {
        "session_id": session_id,
        "mode": "BasicSetupMics",
        "physical_mics": 1,
        "sample_rate": SAMPLE_RATE,
        "samples_per_chunk": SAMPLES_PER_CHUNK,
        "sample_format": "pcm16le_normalized_json",
        "sai": "SAI2_Block_A",
        "pins": {
            "bclk": "PI5",
            "ws": "PI7",
            "data": "PI6",
        },
        "slots": [
            {"slot": 0, "sound": []},
            {"slot": 1, "sound": []},
        ],
    }


def load_metadata(path: str, session_id: int) -> dict:
    if not os.path.exists(path):
        return new_metadata(session_id)

    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    if data.get("session_id") != session_id:
        raise RuntimeError(
            f"Metadata session_id={data.get('session_id')}; esperado {session_id}"
        )

    return data


def upsert_metadata_order(metadata: dict, session_id: int, order: int) -> None:
    rel_timestamp = (order - 1) * 1000
    slots_by_id = {
        int(item["slot"]): item
        for item in metadata.get("slots", [])
        if isinstance(item, dict) and "slot" in item
    }

    for slot in SLOTS:
        slot_obj = slots_by_id.setdefault(slot, {"slot": slot, "sound": []})
        sounds = slot_obj.setdefault("sound", [])
        sound_path = f"sound/{sound_filename(session_id, order, slot)}"

        existing = next(
            (
                item
                for item in sounds
                if int(item.get("order_number", -1)) == order
            ),
            None,
        )

        record = {
            "sound_path": sound_path,
            "order_number": order,
            "rel_timestamp": rel_timestamp,
        }

        if existing is None:
            sounds.append(record)
        else:
            existing.clear()
            existing.update(record)

        sounds.sort(key=lambda item: int(item.get("order_number", 0)))

    metadata["slots"] = [slots_by_id[0], slots_by_id[1]]


def receive_one_capture(ser: serial.Serial) -> dict[int, SlotCapture]:
    wait_for_prefix(ser, "[CAPTURE_STARTED]", timeout_s=5.0)
    wait_for_prefix(ser, "[CAPTURE_DONE]", timeout_s=5.0)

    captures: dict[int, SlotCapture] = {}

    for slot in SLOTS:
        header = wait_for_prefix(ser, f"[SLOT{slot}_BIN]", timeout_s=5.0)
        payload_bytes = parse_bytes_field(header)

        if payload_bytes != PCM16_BYTES:
            raise RuntimeError(
                f"SLOT{slot}: bytes={payload_bytes}; esperados {PCM16_BYTES}"
            )

        wire_seconds = (payload_bytes * 10.0) / float(ser.baudrate)
        payload = read_exact(
            ser,
            payload_bytes,
            timeout_s=max(2.0, wire_seconds * 3.0 + 0.5),
        )

        captures[slot] = SlotCapture(slot=slot, payload=payload)
        wait_for_prefix(ser, f"[SLOT{slot}_END]", timeout_s=5.0)

    wait_for_prefix(ser, "[BASIC_DONE]", timeout_s=5.0)
    return captures


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Receptor minimo BasicSetupMics: 1 mic fisico, "
            "SAI2A, dos slots I2S y JSON por segundo."
        )
    )
    parser.add_argument("--port", required=True, help="Ejemplo: COM7")
    parser.add_argument("--baud", type=int, default=EXPECTED_BAUD)
    parser.add_argument("--session", type=int, default=1)
    parser.add_argument("--order", type=int, default=1)
    parser.add_argument("--output", default=OUTPUT_DIR)
    parser.add_argument("--metadata-output", default=METADATA_DIR)
    args = parser.parse_args()

    ensure_dir(args.output)
    ensure_dir(args.metadata_output)

    metadata_path = os.path.join(
        args.metadata_output,
        metadata_filename(args.session),
    )
    metadata = load_metadata(metadata_path, args.session)

    if args.baud != EXPECTED_BAUD:
        print(
            f"[WARN] BasicSetupMics espera {EXPECTED_BAUD} baud; "
            f"se solicito {args.baud}."
        )

    try:
        with serial.Serial(
            args.port,
            args.baud,
            timeout=0.05,
            write_timeout=1.0,
        ) as ser:
            try:
                ser.set_buffer_size(rx_size=512 * 1024, tx_size=64 * 1024)
            except (AttributeError, OSError):
                pass

            time.sleep(0.20)
            ser.reset_input_buffer()

            print(f"[UART] Conectado a {args.port} @ {args.baud}")
            print("[UART] Esperando [BASIC_READY]...")
            wait_for_prefix(ser, "[BASIC_READY]", timeout_s=60.0)

            ser.write(START_COMMAND)
            ser.flush()
            print("[UART] R enviado. Capturando 1 segundo...")

            captures = receive_one_capture(ser)

        for slot in SLOTS:
            normalized = pcm16_bytes_to_normalized(captures[slot].payload)
            name = sound_filename(args.session, args.order, slot)
            path = os.path.join(args.output, name)
            save_json_atomic(normalized, path)
            print(
                f"[JSON_OK] slot={slot} order={args.order} "
                f"samples={len(normalized)} path={path}"
            )

        upsert_metadata_order(metadata, args.session, args.order)
        save_json_atomic(metadata, metadata_path, indent=4)

        print("\n" + "=" * 58)
        print(f"Sesion             : {args.session}")
        print(f"Orden              : {args.order}")
        print(f"Micros fisicos     : 1")
        print(f"Slots guardados    : 0 y 1")
        print(f"Muestras por slot  : {SAMPLES_PER_CHUNK}")
        print(f"Sample rate        : {SAMPLE_RATE} Hz")
        print(f"Audio JSON         : {os.path.abspath(args.output)}")
        print(f"Metadata           : {os.path.abspath(metadata_path)}")
        print("=" * 58)
        return 0

    except Exception as exc:
        print(f"[ERROR] {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
