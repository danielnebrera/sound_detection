#!/usr/bin/env python3
"""
grabar_sesion.py — transmisión BINARIA con CRC32

Protocolo por canal:
  [CHx_BIN]\r\n              header ASCII
  <88200 bytes int16 LE>     payload binario (44100 x 2)
  <4 bytes CRC32 LE>         checksum
  [CHx_END] sent=44100 errors=0\r\n  confirmacion ASCII
  PC responde A (ACK) o N (retransmitir canal)
Al guardar el chunk completo, PC responde K.

Mapeo CH → microfono fisico:
  CH0 → Mic2 (B_odd,  SEL=VCC)
  CH1 → Mic4 (A_odd,  SEL=VCC)
  CH2 → Mic3 (A_even, SEL=GND)
  CH3 → Mic1 (B_even, SEL=GND)

Uso:
    python grabar_sesion.py --port COM6 --session 1 --baud 921600
    python grabar_sesion.py --port COM6 --session 2 --start-order 4 --baud 921600
    python grabar_sesion.py --session 1   # simulacion
"""

import argparse
import json
import os
import sys
import time
import signal
import struct
import zlib
import random

try:
    import serial
except ImportError:
    print("[ERROR] Instala pyserial:  pip install pyserial")
    sys.exit(1)

# ── Configuracion ─────────────────────────────────────────────────────────────
SAMPLE_RATE    = 44100
OUTPUT_ROOT    = "json_test"
OUTPUT_DIR     = os.path.join(OUTPUT_ROOT, "sound")
METADATA_DIR   = OUTPUT_ROOT
CHUNK_SECS     = 1
CHUNK_MS       = int(round(CHUNK_SECS * 1000))
SAMPLES_CHUNK  = SAMPLE_RATE * CHUNK_SECS        # 44100
PAYLOAD_BYTES  = SAMPLES_CHUNK * 2               # 88200 bytes int16
CRC_BYTES      = 4
CHANNEL_BYTES  = PAYLOAD_BYTES + CRC_BYTES       # 88204

# Mapeo validado:
# CH0 -> Mic2 -> left
# CH1 -> Mic4 -> top
# CH2 -> Mic3 -> back
# CH3 -> Mic1 -> right
MICS = {
    0: {"mic_id": 2, "mic_name": "left"},
    1: {"mic_id": 4, "mic_name": "top"},
    2: {"mic_id": 3, "mic_name": "back"},
    3: {"mic_id": 1, "mic_name": "right"},
}

# Valores provisionales hasta integrar la deteccion real.
PLACEHOLDER_SOUND_CLASS_ID = 0
PLACEHOLDER_PROBABILITY    = 0

CH_BIN_TAGS = ["[CH0_BIN]", "[CH1_BIN]", "[CH2_BIN]", "[CH3_BIN]"]
CH_END_TAGS = ["[CH0_END]", "[CH1_END]", "[CH2_END]", "[CH3_END]"]

CHANNEL_MAX_RETRIES = 3
CHANNEL_READ_TIMEOUT = 15.0
ASCII_LINE_TIMEOUT = 5.0
CHANNEL_ACK = b"A"
CHANNEL_NACK = b"N"
CHUNK_SAVED_ACK = b"K"
STOP_COMMAND = b"S"

# ── Utilidades ────────────────────────────────────────────────────────────────

def crc32_check(data: bytes, expected: int) -> bool:
    calc = zlib.crc32(data) & 0xFFFFFFFF
    return calc == expected

def ensure_dir(path: str):
    os.makedirs(path, exist_ok=True)

def filename(session_id: int, order: int, mic_name: str) -> str:
    """Nombre exacto usado por sound_path en audio_metadata."""
    return f"sound_s{session_id}_ord{order}_{mic_name}.json"


def metadata_filename(session_id: int) -> str:
    return f"audio_metadata_s{session_id}.json"


def _placeholder_intensity_db(session_id: int, order: int, mic_id: int) -> float:
    """Valor provisional determinista; reemplazar por la deteccion real."""
    seed = (session_id * 1_000_003) + (order * 101) + mic_id
    rng = random.Random(seed)
    return round(rng.uniform(35.0, 85.0), 2)


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

    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"No se pudo leer metadata existente: {exc}") from exc

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
        normalized_mics.append({
            "mic_name": mic["mic_name"],
            "mic_id": mic["mic_id"],
            "sound": list(item.get("sound", [])),
        })

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


def upsert_metadata_chunk(metadata: dict, session_id: int, order: int, next_id: int) -> int:
    """Agrega o actualiza las cuatro entradas del chunk completo."""
    mic_objects = {int(item["mic_id"]): item for item in metadata["mics"]}
    rel_timestamp = (order - 1) * CHUNK_MS

    for mic in sorted(MICS.values(), key=lambda item: item["mic_id"]):
        mic_id = mic["mic_id"]
        mic_name = mic["mic_name"]
        mic_obj = mic_objects[mic_id]

        existing = next(
            (
                item for item in mic_obj["sound"]
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

        audio_name = filename(session_id, order, mic_name)
        existing.clear()
        existing.update({
            "id": sound_id,
            "sound_path": f"sound/{audio_name}",
            "order_number": order,
            "rel_timestamp": rel_timestamp,
            "sound_class_id": PLACEHOLDER_SOUND_CLASS_ID,
            "intensity_db": _placeholder_intensity_db(session_id, order, mic_id),
            "probability_percent": PLACEHOLDER_PROBABILITY,
        })
        mic_obj["sound"].sort(key=lambda item: int(item.get("order_number", 0)))

    return next_id


def save_metadata_atomic(metadata: dict, path: str) -> None:
    temp_path = path + ".tmp"
    with open(temp_path, "w", encoding="utf-8") as handle:
        json.dump(metadata, handle, indent=4, allow_nan=False)
        handle.write("\n")
    os.replace(temp_path, path)


def save_chunk(channels: dict, session_id: int, order: int, out_dir: str) -> bool:
    sizes = [len(channels[ch]) for ch in range(4)]
    if any(s != SAMPLES_CHUNK for s in sizes):
        print(f"\n[WARN] Chunk {order} incompleto {sizes} — descartado")
        return False

    pending = []
    try:
        for ch_idx, mic in MICS.items():
            samples     = channels[ch_idx]
            norm        = [s / 32768.0 for s in samples]   # int16 → float [-1, 1)
            fname       = filename(session_id, order, mic["mic_name"])
            final_path  = os.path.join(out_dir, fname)
            temp_path   = final_path + ".tmp"
            with open(temp_path, "w", encoding="utf-8") as f:
                json.dump(norm, f, separators=(",", ":"), allow_nan=False)
            pending.append((temp_path, final_path))

        for tmp, final in pending:
            os.replace(tmp, final)
        return True

    except Exception as exc:
        print(f"\n[ERROR] save_chunk: {exc}")
        for tmp, _ in pending:
            try:
                if os.path.exists(tmp): os.remove(tmp)
            except OSError:
                pass
        return False


# ── Sesion de grabacion ───────────────────────────────────────────────────────

class GrabacionSession:

    def __init__(self, port, baud, session_id, start_order, out_dir, metadata_dir):
        self.session_id  = session_id
        self.start_order = start_order
        self.out_dir       = out_dir
        self.metadata_dir  = metadata_dir
        self.order         = start_order
        self.chunks_ok     = 0
        self.stop          = False

        ensure_dir(out_dir)
        ensure_dir(metadata_dir)

        self.metadata_path = os.path.join(metadata_dir, metadata_filename(session_id))
        self.metadata = load_metadata(self.metadata_path, session_id)
        self.next_sound_id = next_metadata_id(self.metadata)

        print(f"\n{'='*55}")
        print(f"  Sesion {session_id}  |  Orden inicial: {start_order}")
        print(f"  Chunk : {CHUNK_SECS}s = {SAMPLES_CHUNK} muestras/canal")
        print(f"  Modo  : BINARIO + CRC32 ({CHANNEL_BYTES} bytes/canal)")
        print(f"  Audio : {os.path.abspath(out_dir)}")
        print(f"  Metadata: {os.path.abspath(self.metadata_path)}")
        print(f"  Ctrl+C para detener")
        print(f"{'='*55}\n")

        if port:
            try:
                self.ser = serial.Serial(port, baud, timeout=0.25)
                try:
                    self.ser.set_buffer_size(
                        rx_size=1024 * 1024,
                        tx_size=64 * 1024,
                    )
                except (AttributeError, OSError):
                    pass
                print(f"[UART] Conectado a {port} @ {baud} baud")
            except Exception as e:
                print(f"[ERROR] {e}")
                sys.exit(1)
            self.simulation = False

            print("[UART] Esperando que el STM32 este listo...")
            t0   = time.time()
            sent = False
            while time.time() - t0 < 60 and not sent:
                try:
                    raw  = self.ser.readline()
                    line = raw.decode("ascii", errors="ignore").strip()
                    if line:
                        print(f"  [STM32] {line}")
                    if ("UART_RX" in line or
                            "esperando byte" in line or
                            "RECORD_READY" in line or
                            "esperando comando RECORD" in line):
                        time.sleep(0.3)
                        self.ser.write(b"R")
                        self.ser.flush()
                        print("[UART] Comando enviado ✓")
                        sent = True
                except Exception:
                    pass
            if not sent:
                print("[WARN] Timeout — enviando R de todas formas")
                self.ser.write(b"R")
                self.ser.flush()
        else:
            self.ser        = None
            self.simulation = True
            print("[SIM] Modo simulacion activo (sin --port)")

        signal.signal(signal.SIGINT, self._handle_stop)

    def _handle_stop(self, sig, frame):
        print(f"\n\n[STOP] Ctrl+C recibido — deteniendo sesion...")
        self.stop = True
        if self.ser:
            try:
                self.ser.write(STOP_COMMAND)
            except Exception:
                pass

    def _send_control(self, value: bytes) -> bool:
        if self.ser is None:
            return True
        try:
            self.ser.write(value)
            self.ser.flush()
            return True
        except serial.SerialException as exc:
            print(f"\n[ERROR] UART write: {exc}")
            return False

    def _read_exact(
        self,
        n: int,
        timeout_s: float = CHANNEL_READ_TIMEOUT,
    ) -> bytes | None:
        """Lee exactamente n bytes con un tiempo limite absoluto."""
        data = bytearray()
        deadline = time.monotonic() + timeout_s

        while len(data) < n and not self.stop:
            if time.monotonic() >= deadline:
                print(
                    f"\n[ERROR] Timeout UART: "
                    f"{len(data)}/{n} bytes recibidos"
                )
                return None

            try:
                chunk = self.ser.read(n - len(data))
            except serial.SerialException as exc:
                print(f"\n[ERROR] UART read: {exc}")
                return None

            if chunk:
                data.extend(chunk)

        return bytes(data) if len(data) == n else None

    def _read_ascii_line(
        self,
        timeout_s: float = ASCII_LINE_TIMEOUT,
    ) -> str | None:
        data = bytearray()
        deadline = time.monotonic() + timeout_s

        while not self.stop and time.monotonic() < deadline:
            try:
                value = self.ser.read(1)
            except serial.SerialException as exc:
                print(f"\n[ERROR] UART read: {exc}")
                return None

            if not value:
                continue

            data.extend(value)
            if value == b"\n":
                return data.decode("ascii", errors="ignore").strip()

        return None

    def _wait_for_retry_header(
        self,
        ch_idx: int,
        timeout_s: float = CHANNEL_READ_TIMEOUT,
    ) -> bool:
        """Descarta bytes hasta encontrar el header exacto del reintento."""
        marker = f"[CH{ch_idx}_BIN]\r\n".encode("ascii")
        matched = 0
        deadline = time.monotonic() + timeout_s

        while not self.stop and time.monotonic() < deadline:
            try:
                value = self.ser.read(1)
            except serial.SerialException as exc:
                print(f"\n[ERROR] UART read: {exc}")
                return False

            if not value:
                continue

            byte = value[0]
            if byte == marker[matched]:
                matched += 1
                if matched == len(marker):
                    return True
            else:
                matched = 1 if byte == marker[0] else 0

        print(f"\n[ERROR] No llego el header de reintento CH{ch_idx}")
        return False

    def _receive_channel_binary(self, ch_idx: int) -> list | None:
        expected_end = f"[CH{ch_idx}_END]"

        for attempt in range(1, CHANNEL_MAX_RETRIES + 1):
            data = self._read_exact(CHANNEL_BYTES)

            crc_ok = False
            payload = b""
            crc_calc = 0
            crc_recv = 0

            if data is not None:
                payload = data[:PAYLOAD_BYTES]
                crc_recv = struct.unpack_from(
                    "<I", data, PAYLOAD_BYTES
                )[0]
                crc_calc = zlib.crc32(payload) & 0xFFFFFFFF
                crc_ok = crc_calc == crc_recv

            end_line = None
            report_ok = False

            if crc_ok:
                end_line = self._read_ascii_line()
                report_ok = (
                    end_line is not None
                    and end_line.startswith(expected_end)
                    and "sent=44100" in end_line
                    and "errors=0" in end_line
                )

            if crc_ok and report_ok:
                if not self._send_control(CHANNEL_ACK):
                    return None
                return list(
                    struct.unpack_from(
                        f"<{SAMPLES_CHUNK}h",
                        payload,
                    )
                )

            if data is not None and not crc_ok:
                print(
                    f"\n[WARN] CRC32 CH{ch_idx} intento {attempt}/"
                    f"{CHANNEL_MAX_RETRIES}: "
                    f"calculado=0x{crc_calc:08X} "
                    f"recibido=0x{crc_recv:08X}"
                )
            elif crc_ok and not report_ok:
                print(
                    f"\n[WARN] Fin CH{ch_idx} invalido en intento "
                    f"{attempt}: {end_line!r}"
                )

            if not self._send_control(CHANNEL_NACK):
                return None

            if attempt >= CHANNEL_MAX_RETRIES:
                break

            print(
                f"\n[UART] Solicitando retransmision CH{ch_idx} "
                f"({attempt + 1}/{CHANNEL_MAX_RETRIES})..."
            )

            if not self._wait_for_retry_header(ch_idx):
                break

        self._send_control(STOP_COMMAND)
        self.stop = True
        print(
            f"\n[ERROR] CH{ch_idx} fallo despues de "
            f"{CHANNEL_MAX_RETRIES} intentos"
        )
        return None

    def _receive_chunk(self) -> dict | None:
        """
        Recibe un chunk completo:
          Lee líneas ASCII hasta [CHUNK_START]
          Para cada canal: espera [CHx_BIN] y lee payload binario
          Termina con [CHUNK_END]
        """
        channels   = {}
        in_chunk   = False
        total      = SAMPLES_CHUNK

        while not self.stop:
            # Leer línea ASCII
            try:
                raw  = self.ser.readline()
                line = raw.decode("ascii", errors="ignore").strip()
            except serial.SerialException as e:
                print(f"\n[ERROR] UART: {e}")
                return None

            if not line:
                continue

            # Inicio de chunk
            if line.startswith("[CHUNK_START]"):
                parts    = line.split()
                total    = int(parts[1]) if len(parts) > 1 else SAMPLES_CHUNK
                channels = {}
                in_chunk = True
                print(f"  → Chunk {self.order} recibiendo "
                      f"({total} muestras/canal, binario)...",
                      end="", flush=True)
                continue

            if not in_chunk:
                self._print_stm32(line)
                continue

            # Header de canal binario: [CHx_BIN]
            for ch_idx, tag in enumerate(CH_BIN_TAGS):
                if line.startswith(tag.replace("\r\n", "").strip()):
                    t0      = time.time()
                    samples = self._receive_channel_binary(ch_idx)
                    elapsed = time.time() - t0
                    if samples is None:
                        print(f"\n[ERROR] Fallo recibiendo CH{ch_idx}")
                        return None
                    channels[ch_idx] = samples
                    print(f" CH{ch_idx}✓({elapsed:.2f}s)",
                          end="", flush=True)
                    break

            # Confirmacion ASCII de fin de canal
            for ch_idx, tag in enumerate(CH_END_TAGS):
                if line.startswith(tag.replace("\r\n", "").strip()):
                    # Extraer sent/errors del mensaje
                    if "errors=0" not in line:
                        print(f"\n[WARN] {line}")
                    break

            # Fin de chunk
            if line.startswith("[CHUNK_END]") or line.startswith("[CHUNK_GUARDADO]"):
                if len(channels) == 4:
                    sizes = [len(channels[ch]) for ch in range(4)]
                    print(f" completo {sizes}")
                    return channels
                # CHUNK_END vacío del chunk anterior — ignorar
                if len(channels) == 0:
                    in_chunk = False
                    continue

            # Mensajes de progreso
            if in_chunk and not any(line.startswith(t.strip()) for t in CH_BIN_TAGS):
                self._print_stm32(line)

            # Fin de grabacion
            if "RECORD_DONE" in line:
                print(f"\n[STM32] {line}")
                self.stop = True
                return None

        return None

    def _print_stm32(self, line: str):
        if "[GRABANDO]" in line:
            print(f"\n  \U0001f399\ufe0f  {line}")
        elif "[CHUNK_GUARDADO]" in line or "[CHUNK_LISTO]" in line:
            print(f"\n  \u2139\ufe0f  {line}")
        elif "RECORD_READY" in line:
            print(f"\n  \u25b6\ufe0f  Grabacion iniciada")
        elif line:
            print(f"  [STM32] {line}")

    def _simulate_chunk(self) -> dict:
        import random
        return {
            0: [random.randint(-8000,  8000) for _ in range(SAMPLES_CHUNK)],
            1: [random.randint(-8000,  8000) for _ in range(SAMPLES_CHUNK)],
            2: [random.randint(-500,    500) for _ in range(SAMPLES_CHUNK)],
            3: [random.randint(-500,    500) for _ in range(SAMPLES_CHUNK)],
        }

    def run(self):
        try:
            while not self.stop:
                if self.simulation:
                    time.sleep(0.5)
                    if self.stop: break
                    channels = self._simulate_chunk()
                else:
                    channels = self._receive_chunk()
                    if channels is None:
                        break

                sizes = [len(channels[ch]) for ch in range(4)]
                if any(s != SAMPLES_CHUNK for s in sizes):
                    print(f"\n[WARN] Chunk {self.order} incompleto {sizes} — sesion detenida")
                    if not self.simulation:
                        self._send_control(STOP_COMMAND)
                    self.stop = True
                    break

                ok = save_chunk(channels, self.session_id, self.order, self.out_dir)
                if not ok:
                    if not self.simulation:
                        self._send_control(STOP_COMMAND)
                    self.stop = True
                    break

                if ok:
                    try:
                        self.next_sound_id = upsert_metadata_chunk(
                            self.metadata,
                            self.session_id,
                            self.order,
                            self.next_sound_id,
                        )
                        save_metadata_atomic(self.metadata, self.metadata_path)
                    except Exception as exc:
                        print(f"\n[ERROR] No se pudo actualizar metadata: {exc}")
                        if not self.simulation:
                            self._send_control(STOP_COMMAND)
                        self.stop = True
                        break

                    if not self.simulation:
                        if not self._send_control(CHUNK_SAVED_ACK):
                            self.stop = True
                            break

                    self.chunks_ok += 1
                    print(f"\n{'='*55}")
                    print(f"  \u2705 CHUNK {self.chunks_ok} GUARDADO (ord {self.order})")
                    for mic in sorted(MICS.values(), key=lambda item: item["mic_id"]):
                        print(
                            f"     mic{mic['mic_id']} ({mic['mic_name']}): "
                            f"{filename(self.session_id, self.order, mic['mic_name'])}"
                        )
                    print(f"     metadata: {metadata_filename(self.session_id)}")
                    print(f"{'='*55}")
                    print(f"  \u23f3 Grabando siguiente chunk... (Ctrl+C para terminar)")
                    self.order += 1

        finally:
            self._finish()

    def _finish(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass

        total_files = self.chunks_ok * 4
        print(f"\n{'='*55}")
        print(f"  Sesion {self.session_id} finalizada")
        print(f"  Chunks completos : {self.chunks_ok}")
        print(f"  Archivos JSON    : {total_files}")
        print(f"  Ordenes          : {self.start_order} → {self.order - 1}")
        print(f"  Carpeta audio    : {os.path.abspath(self.out_dir)}")
        print(f"  Metadata         : {os.path.abspath(self.metadata_path)}")
        print(f"{'='*55}\n")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description=f"Graba audio STM32 en chunks de {CHUNK_SECS}s — modo binario."
    )
    parser.add_argument("--port",        default=None)
    parser.add_argument("--baud",        type=int, default=921600)
    parser.add_argument("--session",     type=int, default=1)
    parser.add_argument("--start-order", type=int, default=1)
    parser.add_argument("--output",      default=OUTPUT_DIR,
                        help="Carpeta para los JSON de audio")
    parser.add_argument("--metadata-output", default=METADATA_DIR,
                        help="Carpeta para audio_metadata_s<ID>.json")
    args = parser.parse_args()

    session = GrabacionSession(
        port        = args.port,
        baud        = args.baud,
        session_id  = args.session,
        start_order = args.start_order,
        out_dir       = args.output,
        metadata_dir  = args.metadata_output,
    )
    session.run()


if __name__ == "__main__":
    main()