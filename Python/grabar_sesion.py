#!/usr/bin/env python3
"""
grabar_sesion.py - receptor SDRAM v2.1.7 con timeout adaptativo y fallback inmediato.

Flujo:
  1. Espera [RECORD_READY] y envia R.
  2. El STM32 captura 3 s de calibracion + audio continuo.
  3. Ctrl+C envia S, pero Python continua escuchando.
  4. El STM32 cierra en el siguiente segundo exacto, procesa MFCC/modelo
     y transmite cada chunk con CRC32, ACK por bloque y fallback adaptativo.
  5. Python recibe UART en primer plano y genera JSON en procesos separados.
  6. El metadata se escribe una sola vez al finalizar.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import signal
import struct
import sys
import time
import zlib
from concurrent.futures import Future, ProcessPoolExecutor, as_completed
from dataclasses import dataclass

try:
    import serial
except ImportError:
    print("[ERROR] Instala pyserial: pip install pyserial")
    sys.exit(1)

SAMPLE_RATE = 44100
CHUNK_SECS = 1
CHUNK_MS = 1000
SAMPLES_CHUNK = SAMPLE_RATE
PAYLOAD_BYTES = SAMPLES_CHUNK * 2
CRC_BYTES = 4
CHANNEL_BYTES = PAYLOAD_BYTES + CRC_BYTES

OUTPUT_ROOT = "json_test"
OUTPUT_DIR = os.path.join(OUTPUT_ROOT, "sound")
METADATA_DIR = OUTPUT_ROOT

MICS = {
    0: {"mic_id": 2, "mic_name": "left"},
    1: {"mic_id": 4, "mic_name": "top"},
    2: {"mic_id": 3, "mic_name": "back"},
    3: {"mic_id": 1, "mic_name": "right"},
}

CH_BIN_TAGS = ["[CH0_BIN]", "[CH1_BIN]", "[CH2_BIN]", "[CH3_BIN]"]
BLOCK_PAYLOAD_BYTES = 8192
BLOCK_FALLBACK_BYTES = 4096
BLOCK_FINAL_FALLBACK_BYTES = 1024
EXPECTED_BAUD = 1000000
BLOCK_MAX_RETRIES = 12
BLOCK_TIMEOUT_MIN = 0.18
BLOCK_TIMEOUT_WIRE_MULTIPLIER = 4.0
BLOCK_TIMEOUT_MARGIN = 0.08
BLOCK_HEADER_TIMEOUT = 1.50
CHANNEL_READ_TIMEOUT = 2.0
ASCII_LINE_TIMEOUT = 4.0
CHANNEL_ACK = b"A"
CHANNEL_NACK = b"N"
CHUNK_SAVED_ACK = b"K"
STOP_COMMAND = b"S"
START_COMMAND = b"R"

DET_RESULT_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def filename(session_id: int, order: int, mic_name: str) -> str:
    return f"sound_s{session_id}_ord{order}_{mic_name}.json"


def metadata_filename(session_id: int) -> str:
    return f"audio_metadata_s{session_id}.json"


def _new_metadata(session_id: int) -> dict:
    return {
        "session_id": session_id,
        "mics": [
            {
                "mic_name": mic["mic_name"],
                "mic_id": mic["mic_id"],
                "sound": [],
            }
            for mic in sorted(MICS.values(), key=lambda item: item["mic_id"])
        ],
    }


def load_metadata(path: str, session_id: int) -> dict:
    if not os.path.exists(path):
        return _new_metadata(session_id)

    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    if data.get("session_id") != session_id:
        raise RuntimeError(
            f"El metadata contiene session_id={data.get('session_id')} "
            f"pero se solicito session_id={session_id}"
        )

    existing_by_id = {
        int(item.get("mic_id")): item
        for item in data.get("mics", [])
        if isinstance(item, dict) and "mic_id" in item
    }

    normalized_mics = []
    for mic in sorted(MICS.values(), key=lambda item: item["mic_id"]):
        item = existing_by_id.get(mic["mic_id"], {})
        normalized_mics.append(
            {
                "mic_name": mic["mic_name"],
                "mic_id": mic["mic_id"],
                "sound": list(item.get("sound", [])),
            }
        )

    data["mics"] = normalized_mics
    return data


def next_metadata_id(metadata: dict) -> int:
    ids = [
        int(sound["id"])
        for mic in metadata.get("mics", [])
        for sound in mic.get("sound", [])
        if isinstance(sound, dict) and "id" in sound
    ]
    return max(ids, default=0) + 1


def save_metadata_atomic(metadata: dict, path: str) -> None:
    temp_path = path + ".tmp"
    with open(temp_path, "w", encoding="utf-8") as handle:
        json.dump(metadata, handle, indent=4, allow_nan=False)
        handle.write("\n")
    os.replace(temp_path, path)


def save_chunk(channels: dict[int, list[int]], session_id: int, order: int, out_dir: str) -> bool:
    sizes = [len(channels[ch]) for ch in range(4)]
    if any(size != SAMPLES_CHUNK for size in sizes):
        print(f"[WARN] Chunk {order} incompleto {sizes}; descartado")
        return False

    pending: list[tuple[str, str]] = []
    try:
        for ch_idx, mic in MICS.items():
            normalized = [sample / 32768.0 for sample in channels[ch_idx]]
            name = filename(session_id, order, mic["mic_name"])
            final_path = os.path.join(out_dir, name)
            temp_path = final_path + ".tmp"

            with open(temp_path, "w", encoding="utf-8") as handle:
                json.dump(normalized, handle, separators=(",", ":"), allow_nan=False)

            pending.append((temp_path, final_path))

        for temp_path, final_path in pending:
            os.replace(temp_path, final_path)
        return True
    except Exception as exc:
        print(f"[ERROR] save_chunk: {exc}")
        for temp_path, _ in pending:
            try:
                if os.path.exists(temp_path):
                    os.remove(temp_path)
            except OSError:
                pass
        return False


def save_chunk_pcm16_bytes(
    channels: dict[int, bytes],
    session_id: int,
    order: int,
    out_dir: str,
) -> int:
    """Worker multiproceso: PCM16 little-endian -> cuatro JSON atomicos."""
    sizes = [len(channels[ch]) for ch in range(4)]
    if any(size != PAYLOAD_BYTES for size in sizes):
        raise ValueError(f"Chunk {order} PCM16 incompleto: {sizes}")

    pending: list[tuple[str, str]] = []
    try:
        for ch_idx, mic in MICS.items():
            samples = struct.unpack(f"<{SAMPLES_CHUNK}h", channels[ch_idx])
            normalized = [sample / 32768.0 for sample in samples]
            name = filename(session_id, order, mic["mic_name"])
            final_path = os.path.join(out_dir, name)
            temp_path = final_path + ".tmp"

            with open(temp_path, "w", encoding="utf-8") as handle:
                json.dump(
                    normalized,
                    handle,
                    separators=(",", ":"),
                    allow_nan=False,
                )

            pending.append((temp_path, final_path))

        for temp_path, final_path in pending:
            os.replace(temp_path, final_path)

        return order
    except Exception:
        for temp_path, _ in pending:
            try:
                if os.path.exists(temp_path):
                    os.remove(temp_path)
            except OSError:
                pass
        raise


@dataclass
class DetectionResult:
    alert: int = 0
    baseline: int = 0
    fused: float = 0.0
    ema: float = 0.0
    valid_mask: int = 0
    active_mask: int = 0
    probability: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    dbfs: tuple[float, float, float, float] = (-120.0, -120.0, -120.0, -120.0)
    delta_dbfs: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)


def parse_detection_line(line: str) -> DetectionResult:
    values = {key: value for key, value in DET_RESULT_RE.findall(line)}

    def get_float(name: str, default: float = 0.0) -> float:
        try:
            return float(values.get(name, default))
        except (TypeError, ValueError):
            return default

    def get_int(name: str, default: int = 0) -> int:
        raw = values.get(name)
        if raw is None:
            return default
        try:
            return int(raw, 0)
        except ValueError:
            return default

    return DetectionResult(
        alert=get_int("alert"),
        baseline=get_int("baseline"),
        fused=get_float("fused"),
        ema=get_float("ema"),
        valid_mask=get_int("valid"),
        active_mask=get_int("active"),
        probability=tuple(get_float(f"p{i}") for i in range(4)),
        dbfs=tuple(get_float(f"db{i}", -120.0) for i in range(4)),
        delta_dbfs=tuple(get_float(f"delta{i}") for i in range(4)),
    )


def upsert_metadata_chunk(
    metadata: dict,
    session_id: int,
    order: int,
    next_id: int,
    detection: DetectionResult,
) -> int:
    mic_objects = {int(item["mic_id"]): item for item in metadata["mics"]}
    rel_timestamp = (order - 1) * CHUNK_MS

    for ch_idx, mic in MICS.items():
        mic_id = mic["mic_id"]
        mic_name = mic["mic_name"]
        mic_obj = mic_objects[mic_id]

        existing = next(
            (
                item
                for item in mic_obj["sound"]
                if int(item.get("order_number", -1)) == order
            ),
            None,
        )

        if existing is None:
            sound_id = next_id
            next_id += 1
            existing = {}
            mic_obj["sound"].append(existing)
        else:
            sound_id = int(existing.get("id", next_id))
            if "id" not in existing:
                next_id += 1

        probability_percent = round(
            max(0.0, min(1.0, detection.probability[ch_idx])) * 100.0,
            2,
        )

        existing.clear()
        existing.update(
            {
                "id": sound_id,
                "sound_path": f"sound/{filename(session_id, order, mic_name)}",
                "order_number": order,
                "rel_timestamp": rel_timestamp,
                "sound_class_id": 0,
                "intensity_db": round(detection.dbfs[ch_idx], 2),
                "probability_percent": probability_percent,
            }
        )
        mic_obj["sound"].sort(key=lambda item: int(item.get("order_number", 0)))

    return next_id


class GrabacionSession:
    def __init__(
        self,
        port: str | None,
        baud: int,
        session_id: int,
        start_order: int,
        out_dir: str,
        metadata_dir: str,
        json_workers: int,
        verbose: bool,
    ) -> None:
        self.session_id = session_id
        self.start_order = start_order
        self.order = start_order
        self.out_dir = out_dir
        self.metadata_dir = metadata_dir
        self.chunks_ok = 0
        self.received_chunks = 0
        self.json_workers = max(1, int(json_workers))
        self.verbose = bool(verbose)
        self.chunk_started_at = 0.0
        self.persist_executor: ProcessPoolExecutor | None = None
        self.pending_persists: dict[Future, tuple[int, DetectionResult]] = {}
        self.persisted_detections: dict[int, DetectionResult] = {}

        self.capture_stop_requested = False
        self.abort = False
        self.finished = False
        self.phase = "startup"

        ensure_dir(out_dir)
        ensure_dir(metadata_dir)

        self.metadata_path = os.path.join(metadata_dir, metadata_filename(session_id))
        self.metadata = load_metadata(self.metadata_path, session_id)
        self.next_sound_id = next_metadata_id(self.metadata)

        signal.signal(signal.SIGINT, self._handle_sigint)

        if port is None:
            self.ser = None
            self.simulation = True
            print("[SIM] Sin --port: se generaran 5 chunks simulados.")
        else:
            self.simulation = False
            self.persist_executor = ProcessPoolExecutor(
                max_workers=self.json_workers
            )
            self.ser = serial.Serial(
                port,
                baud,
                timeout=0.02,
                write_timeout=1.0,
            )
            try:
                self.ser.set_buffer_size(rx_size=1024 * 1024, tx_size=64 * 1024)
            except (AttributeError, OSError):
                pass
            print(f"[UART] Conectado a {port} @ {baud} baud")
            print(
                f"[JSON] workers={self.json_workers} mode=process_pool "
                f"ack=queued metadata_write=end verbose={int(self.verbose)}"
            )
            print(
                "[UART_RX_CFG] adaptive_timeout=1 "
                f"min={BLOCK_TIMEOUT_MIN:.2f}s expected_baud={EXPECTED_BAUD}"
            )
            if baud != EXPECTED_BAUD:
                print(
                    f"[WARN] Esta version espera {EXPECTED_BAUD} baud. "
                    "El firmware y Python deben usar el mismo valor."
                )
            self._wait_ready_and_start()

    def _handle_sigint(self, sig, frame) -> None:
        del sig, frame

        if self.simulation:
            self.abort = True
            return

        if self.phase == "capture" and not self.capture_stop_requested:
            self.capture_stop_requested = True
            print("\n[STOP] Ctrl+C: solicitando cierre en el proximo segundo exacto...")
            self._send_control(STOP_COMMAND)
            return

        print("\n[ABORT] Segundo Ctrl+C: abortando transferencia.")
        self.abort = True
        self._send_control(STOP_COMMAND)

    def _wait_ready_and_start(self) -> None:
        print("[UART] Esperando [RECORD_READY]...")
        deadline = time.monotonic() + 60.0

        while time.monotonic() < deadline and not self.abort:
            raw = self.ser.readline()
            line = raw.decode("ascii", errors="ignore").strip()
            if line:
                print(f"  [STM32] {line}")

            if "RECORD_READY" in line:
                self.ser.write(START_COMMAND)
                self.ser.flush()
                self.phase = "capture"
                print("[UART] R enviado. Calibracion y captura iniciadas.")
                return

        raise RuntimeError("Timeout esperando RECORD_READY")

    def _send_control(self, value: bytes) -> bool:
        if self.ser is None:
            return True
        try:
            self.ser.write(value)
            self.ser.flush()
            return True
        except serial.SerialException as exc:
            print(f"[ERROR] UART write: {exc}")
            self.abort = True
            return False

    def _block_timeout(self, block_length: int) -> float:
        """Timeout corto basado en el tiempo fisico del bloque sobre UART 8N1."""
        baudrate = max(1, int(self.ser.baudrate))
        wire_seconds = (float(block_length) * 10.0) / float(baudrate)
        return max(
            BLOCK_TIMEOUT_MIN,
            (wire_seconds * BLOCK_TIMEOUT_WIRE_MULTIPLIER)
            + BLOCK_TIMEOUT_MARGIN,
        )

    def _read_exact(
        self,
        count: int,
        timeout_s: float = CHANNEL_READ_TIMEOUT,
        *,
        timeout_is_retry: bool = False,
    ) -> bytes | None:
        data = bytearray(count)
        view = memoryview(data)
        received = 0
        deadline = time.monotonic() + timeout_s

        while received < count and not self.abort:
            if time.monotonic() >= deadline:
                level = "RETRY" if timeout_is_retry else "ERROR"
                print(
                    f"[{level}] Timeout UART: {received}/{count} bytes "
                    f"limite={timeout_s:.3f}s"
                )
                return None

            try:
                readinto = getattr(self.ser, "readinto", None)
                if callable(readinto):
                    amount = readinto(view[received:])
                    if amount:
                        received += int(amount)
                else:
                    chunk = self.ser.read(count - received)
                    if chunk:
                        amount = len(chunk)
                        view[received:received + amount] = chunk
                        received += amount
            except serial.SerialException as exc:
                print(f"[ERROR] UART read: {exc}")
                return None

        return bytes(data) if received == count else None

    def _discard_pending_input(self, max_payload_bytes: int = BLOCK_PAYLOAD_BYTES) -> None:
        """Descarta residuos; el STM32 permanece detenido esperando ACK/NACK."""
        del max_payload_bytes
        if self.ser is None:
            return

        # El timeout adaptativo supera varias veces el tiempo fisico del bloque,
        # por lo que al llegar aqui el transmisor ya termino y espera respuesta.
        time.sleep(0.005)
        try:
            self.ser.reset_input_buffer()
        except (AttributeError, OSError, serial.SerialException):
            pass

    def _read_ascii_line(self, timeout_s: float = ASCII_LINE_TIMEOUT) -> str | None:
        data = bytearray()
        deadline = time.monotonic() + timeout_s

        while time.monotonic() < deadline and not self.abort:
            value = self.ser.read(1)
            if not value:
                continue
            data.extend(value)
            if value == b"\n":
                return data.decode("ascii", errors="ignore").strip()

        return None

    def _scan_prefixed_line(
        self,
        prefix: bytes,
        timeout_s: float = CHANNEL_READ_TIMEOUT,
    ) -> str | None:
        """Busca una línea ASCII por prefijo, descartando bytes residuales."""
        matched = 0
        line = bytearray()
        deadline = time.monotonic() + timeout_s

        while time.monotonic() < deadline and not self.abort:
            try:
                value = self.ser.read(1)
            except serial.SerialException as exc:
                print(f"[ERROR] UART read: {exc}")
                return None

            if not value:
                continue

            byte = value[0]

            if matched < len(prefix):
                if byte == prefix[matched]:
                    matched += 1
                    if matched == len(prefix):
                        line = bytearray(prefix)
                else:
                    matched = 1 if byte == prefix[0] else 0
                continue

            line.append(byte)
            if byte == 0x0A:
                return line.decode("ascii", errors="ignore").strip()

            if len(line) > 256:
                matched = 0
                line.clear()

        return None

    @staticmethod
    def _parse_channel_header(
        line: str,
        ch_idx: int,
    ) -> tuple[int, int, int, int] | None:
        expected = f"[CH{ch_idx}_BIN]"
        if not line.startswith(expected):
            return None

        values = {key: value for key, value in DET_RESULT_RE.findall(line)}
        try:
            payload_bytes = int(values["bytes"], 0)
            max_block = int(values["max_block"], 0)
            fallback = int(values["fallback"], 0)
            final_fallback = int(values["final"], 0)
            ack_mode = values["ack"]
        except (KeyError, ValueError):
            return None

        if (
            payload_bytes != PAYLOAD_BYTES
            or max_block != BLOCK_PAYLOAD_BYTES
            or fallback != BLOCK_FALLBACK_BYTES
            or final_fallback != BLOCK_FINAL_FALLBACK_BYTES
            or ack_mode != "block"
        ):
            return None

        return payload_bytes, max_block, fallback, final_fallback

    @staticmethod
    def _parse_block_header(
        line: str,
    ) -> tuple[int, int, int, int, int, int] | None:
        if not line.startswith("[BLK]"):
            return None

        values = {key: value for key, value in DET_RESULT_RE.findall(line)}
        try:
            return (
                int(values["ch"], 0),
                int(values["seq"], 0),
                int(values["off"], 0),
                int(values["len"], 0),
                int(values["crc"], 16),
                int(values["try"], 0),
            )
        except (KeyError, ValueError):
            return None

    def _receive_channel(
        self,
        ch_idx: int,
        payload_bytes: int,
        max_block: int,
        fallback: int,
        final_fallback: int,
    ) -> bytes | None:
        del fallback, final_fallback

        output = bytearray(payload_bytes)
        expected_offset = 0
        expected_sequence = 0
        failures_for_position = 0
        packets_received = 0

        while expected_offset < payload_bytes and not self.abort:
            block_line = self._scan_prefixed_line(
                b"[BLK]",
                BLOCK_HEADER_TIMEOUT,
            )
            parsed_block = self._parse_block_header(block_line or "")

            if parsed_block is None:
                failures_for_position += 1
                print(
                    f"\n[WARN] CH{ch_idx} off={expected_offset} "
                    f"cabecera de bloque invalida; NACK "
                    f"({failures_for_position}/{BLOCK_MAX_RETRIES})"
                )
                self._discard_pending_input(max_block)
                if not self._send_control(CHANNEL_NACK):
                    return None
                if failures_for_position >= BLOCK_MAX_RETRIES:
                    self._send_control(STOP_COMMAND)
                    self.abort = True
                    return None
                continue

            (
                block_ch,
                block_sequence,
                block_offset,
                block_len,
                block_crc,
                block_try,
            ) = parsed_block

            length_ok = (
                block_len > 0
                and block_len <= max_block
                and block_len <= (payload_bytes - expected_offset)
            )
            header_ok = (
                block_ch == ch_idx
                and block_sequence == expected_sequence
                and block_offset == expected_offset
                and block_try > 0
                and length_ok
            )

            if not header_ok:
                failures_for_position += 1
                print(
                    f"\n[WARN] CH{ch_idx} encabezado fuera de secuencia "
                    f"esperado(seq={expected_sequence},off={expected_offset}) "
                    f"recibido={parsed_block}; NACK"
                )

                if 0 < block_len <= max_block:
                    _ = self._read_exact(
                        block_len,
                        self._block_timeout(block_len),
                        timeout_is_retry=True,
                    )
                else:
                    self._discard_pending_input(max_block)

                if not self._send_control(CHANNEL_NACK):
                    return None
                if failures_for_position >= BLOCK_MAX_RETRIES:
                    self._send_control(STOP_COMMAND)
                    self.abort = True
                    return None
                continue

            block_data = self._read_exact(
                block_len,
                self._block_timeout(block_len),
                timeout_is_retry=True,
            )
            if block_data is None:
                failures_for_position += 1
                self._discard_pending_input(max_block)
                if not self._send_control(CHANNEL_NACK):
                    return None
                if failures_for_position >= BLOCK_MAX_RETRIES:
                    self._send_control(STOP_COMMAND)
                    self.abort = True
                    return None
                continue

            crc_calc = zlib.crc32(block_data) & 0xFFFFFFFF
            if crc_calc != block_crc:
                failures_for_position += 1
                print(
                    f"\n[WARN] CH{ch_idx} seq={block_sequence} "
                    f"off={block_offset} len={block_len} try={block_try} "
                    f"crc calc=0x{crc_calc:08X} recv=0x{block_crc:08X}; NACK"
                )
                if not self._send_control(CHANNEL_NACK):
                    return None
                if failures_for_position >= BLOCK_MAX_RETRIES:
                    self._send_control(STOP_COMMAND)
                    self.abort = True
                    return None
                continue

            output[expected_offset:expected_offset + block_len] = block_data

            if not self._send_control(CHANNEL_ACK):
                return None

            expected_offset += block_len
            expected_sequence += 1
            packets_received += 1
            failures_for_position = 0

        if expected_offset != payload_bytes:
            self._send_control(STOP_COMMAND)
            self.abort = True
            print(
                f"\n[ERROR] CH{ch_idx} bytes={expected_offset}/{payload_bytes}"
            )
            return None

        expected_end = f"[CH{ch_idx}_END]"
        end_line = self._scan_prefixed_line(
            expected_end.encode("ascii"),
            ASCII_LINE_TIMEOUT,
        )
        values = {key: value for key, value in DET_RESULT_RE.findall(end_line or "")}
        try:
            end_sent = int(values.get("sent", "-1"), 0)
            end_bytes = int(values.get("bytes", "-1"), 0)
            end_packets = int(values.get("packets", "-1"), 0)
            end_errors = int(values.get("errors", "-1"), 0)
        except ValueError:
            end_sent = end_bytes = end_packets = end_errors = -1

        report_ok = (
            end_line is not None
            and end_line.startswith(expected_end)
            and end_sent == SAMPLES_CHUNK
            and end_bytes == payload_bytes
            and end_packets == packets_received
            and end_errors == 0
        )

        if not report_ok:
            self._send_control(CHANNEL_NACK)
            self._send_control(STOP_COMMAND)
            self.abort = True
            print(f"\n[ERROR] Fin CH{ch_idx} invalido: {end_line!r}")
            return None

        self._send_control(CHANNEL_ACK)
        return bytes(output)

    def _receive_chunk(self) -> tuple[dict[int, bytes], DetectionResult] | None:
        channels: dict[int, bytes] = {}
        detection = DetectionResult()
        in_chunk = False

        while not self.abort:
            raw = self.ser.readline()
            line = raw.decode("ascii", errors="ignore").strip()
            if not line:
                continue

            if line.startswith("[CAPTURE_STARTED]"):
                self.phase = "capture"
                print(f"[STM32] {line}")
                continue

            if line.startswith("[CAPTURE_DONE]"):
                self.phase = "processing"
                print(f"[STM32] {line}")
                continue

            if line.startswith("[PROCESSING_START]") or line.startswith("[SESSION_START]"):
                self.phase = "transfer"
                print(f"[STM32] {line}")
                continue

            if line.startswith("[RECORD_DONE]"):
                self.finished = True
                print(f"[STM32] {line}")
                return None

            if line.startswith("[CHUNK_START]"):
                channels = {}
                detection = DetectionResult()
                in_chunk = True
                self.chunk_started_at = time.monotonic()
                if self.verbose:
                    print(f"  Recibiendo ord {self.order}...", end="", flush=True)
                continue

            if not in_chunk:
                print(f"[STM32] {line}")
                continue

            if line.startswith("[DET_RESULT]"):
                detection = parse_detection_line(line)
                continue

            matched_channel = False
            for ch_idx, tag in enumerate(CH_BIN_TAGS):
                if line.startswith(tag):
                    channel_cfg = self._parse_channel_header(line, ch_idx)
                    if channel_cfg is None:
                        print(f"\n[ERROR] Cabecera CH{ch_idx} invalida: {line!r}")
                        self._send_control(CHANNEL_NACK)
                        self._send_control(STOP_COMMAND)
                        self.abort = True
                        return None

                    payload_bytes, max_block, fallback, final_fallback = channel_cfg

                    # Handshake del encabezado de canal: el STM32 no envia
                    # bloques hasta recibir este ACK.
                    if not self._send_control(CHANNEL_ACK):
                        return None
                    started = time.monotonic()
                    samples = self._receive_channel(
                        ch_idx,
                        payload_bytes,
                        max_block,
                        fallback,
                        final_fallback,
                    )
                    if samples is None:
                        return None
                    channels[ch_idx] = samples
                    if self.verbose:
                        print(
                            f" CH{ch_idx}({time.monotonic() - started:.2f}s)",
                            end="",
                            flush=True,
                        )
                    matched_channel = True
                    break

            if matched_channel:
                continue

            if line.startswith("[CHUNK_END]"):
                if len(channels) != 4:
                    print(f"\n[ERROR] Chunk incompleto: canales={sorted(channels)}")
                    self.abort = True
                    return None
                if self.verbose:
                    print(" completo")
                return channels, detection

            if line.startswith("[CHUNK_ABORT]"):
                print(f"\n[ERROR] {line}")
                self.abort = True
                return None

            print(f"[STM32] {line}")

        return None

    def _simulate_chunk(self, index: int) -> tuple[dict[int, list[int]], DetectionResult]:
        rng = random.Random(1000 + index)
        channels = {
            0: [rng.randint(-8000, 8000) for _ in range(SAMPLES_CHUNK)],
            1: [rng.randint(-5000, 5000) for _ in range(SAMPLES_CHUNK)],
            2: [rng.randint(-500, 500) for _ in range(SAMPLES_CHUNK)],
            3: [rng.randint(-500, 500) for _ in range(SAMPLES_CHUNK)],
        }
        detection = DetectionResult(
            baseline=1,
            probability=(0.8, 0.6, 0.1, 0.05),
            dbfs=(-22.0, -28.0, -55.0, -60.0),
            delta_dbfs=(8.0, 6.0, 1.0, 0.5),
            fused=0.73,
            ema=0.65,
            alert=2,
        )
        return channels, detection

    def _persist_chunk_sync(
        self,
        channels: dict[int, list[int]],
        detection: DetectionResult,
    ) -> bool:
        if not save_chunk(channels, self.session_id, self.order, self.out_dir):
            return False

        self.next_sound_id = upsert_metadata_chunk(
            self.metadata,
            self.session_id,
            self.order,
            self.next_sound_id,
            detection,
        )
        save_metadata_atomic(self.metadata, self.metadata_path)

        self.chunks_ok += 1
        self.received_chunks += 1
        print(
            f"[OK] ord={self.order} alert={detection.alert} "
            f"fused={detection.fused:.3f} ema={detection.ema:.3f}"
        )
        self.order += 1
        return True

    def _queue_persist(
        self,
        channels: dict[int, bytes],
        detection: DetectionResult,
    ) -> bool:
        if self.persist_executor is None:
            return False

        order = self.order
        try:
            future = self.persist_executor.submit(
                save_chunk_pcm16_bytes,
                channels,
                self.session_id,
                order,
                self.out_dir,
            )
        except Exception as exc:
            print(f"[ERROR] No se pudo encolar JSON ord={order}: {exc}")
            return False

        self.pending_persists[future] = (order, detection)

        # K significa: chunk validado y retenido en RAM/cola del PC.
        # El STM32 puede comenzar el siguiente chunk sin esperar json.dump().
        if not self._send_control(CHUNK_SAVED_ACK):
            future.cancel()
            return False

        self.received_chunks += 1
        elapsed = (
            time.monotonic() - self.chunk_started_at
            if self.chunk_started_at > 0.0
            else 0.0
        )
        print(
            f"[ORD_OK] ord={order} rx={elapsed:.2f}s "
            f"alert={detection.alert} pending={len(self.pending_persists)}"
        )
        self.order += 1
        return True

    def _collect_completed_persists(self, wait_all: bool) -> bool:
        if not self.pending_persists:
            return True

        if wait_all:
            futures = list(as_completed(list(self.pending_persists)))
        else:
            futures = [future for future in self.pending_persists if future.done()]

        for future in futures:
            order, detection = self.pending_persists.pop(future)
            try:
                completed_order = int(future.result())
                if completed_order != order:
                    raise RuntimeError(
                        f"worker devolvio ord={completed_order}, esperado={order}"
                    )
            except Exception as exc:
                print(f"[ERROR] JSON ord={order}: {exc}")
                self.abort = True
                if not self.simulation:
                    self._send_control(STOP_COMMAND)
                return False

            self.persisted_detections[order] = detection
            self.chunks_ok += 1
            if self.verbose:
                print(
                    f"[JSON_OK] ord={order} "
                    f"pendientes={len(self.pending_persists)}"
                )

        return True

    def _finalize_persistence(self) -> bool:
        if self.pending_persists:
            print(
                f"[JSON] Esperando {len(self.pending_persists)} "
                "chunk(s) pendientes..."
            )

        if not self._collect_completed_persists(wait_all=True):
            return False

        for order in sorted(self.persisted_detections):
            self.next_sound_id = upsert_metadata_chunk(
                self.metadata,
                self.session_id,
                order,
                self.next_sound_id,
                self.persisted_detections[order],
            )

        save_metadata_atomic(self.metadata, self.metadata_path)
        print(
            f"[JSON] Completados={self.chunks_ok} "
            "metadata_guardado=1"
        )
        return True

    def run(self) -> None:
        try:
            if self.simulation:
                for index in range(5):
                    if self.abort:
                        break
                    channels, detection = self._simulate_chunk(index)
                    if not self._persist_chunk_sync(channels, detection):
                        self.abort = True
                        break
                self.finished = not self.abort
                return

            while not self.finished and not self.abort:
                received = self._receive_chunk()
                if received is None:
                    self._collect_completed_persists(wait_all=False)
                    continue

                channels, detection = received
                if not self._queue_persist(channels, detection):
                    self._send_control(STOP_COMMAND)
                    self.abort = True
                    break

                if not self._collect_completed_persists(wait_all=False):
                    break

            if self.finished and not self.abort:
                if not self._finalize_persistence():
                    self.abort = True
        finally:
            self._finish()

    def _finish(self) -> None:
        if self.persist_executor is not None:
            try:
                self.persist_executor.shutdown(wait=True, cancel_futures=False)
            except TypeError:
                self.persist_executor.shutdown(wait=True)

        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass

        completed_orders = sorted(self.persisted_detections)
        if self.simulation and self.chunks_ok > 0:
            completed_orders = list(
                range(self.start_order, self.start_order + self.chunks_ok)
            )

        order_text = (
            f"{completed_orders[0]} -> {completed_orders[-1]}"
            if completed_orders
            else "ninguna"
        )

        print("\n" + "=" * 58)
        print(f"Sesion {self.session_id} finalizada")
        print(f"Chunks recibidos: {self.received_chunks}")
        print(f"Chunks JSON completos: {self.chunks_ok}")
        print(f"JSON de audio: {self.chunks_ok * 4}")
        print(f"Ordenes: {order_text}")
        print(f"Audio: {os.path.abspath(self.out_dir)}")
        print(f"Metadata: {os.path.abspath(self.metadata_path)}")
        print(f"Estado: {'OK' if self.finished and not self.abort else 'ABORTADA'}")
        print("=" * 58)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Captura continua STM32/Portenta con SDRAM, MFCC y TFLite."
    )
    parser.add_argument("--port", default=None)
    parser.add_argument("--baud", type=int, default=460800)
    parser.add_argument("--session", type=int, default=1)
    parser.add_argument("--start-order", type=int, default=1)
    parser.add_argument("--output", default=OUTPUT_DIR)
    parser.add_argument("--metadata-output", default=METADATA_DIR)
    parser.add_argument("--json-workers", type=int, default=2)
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Muestra tiempos por canal y finalizacion de cada worker JSON.",
    )
    args = parser.parse_args()

    try:
        session = GrabacionSession(
            port=args.port,
            baud=args.baud,
            session_id=args.session,
            start_order=args.start_order,
            out_dir=args.output,
            metadata_dir=args.metadata_output,
            json_workers=args.json_workers,
            verbose=args.verbose,
        )
        session.run()
        return 0 if session.finished and not session.abort else 2
    except Exception as exc:
        print(f"[ERROR] {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())