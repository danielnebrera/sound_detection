#!/usr/bin/env python3
"""
grabar_sesion.py

Graba audio del STM32 (4 mics) en chunks exactos de 1 segundo.
Chunks incompletos se descartan automaticamente.

Mapeo validado por desconexion fisica DOUT:
  CH0 -> dma_buf_b[idx+1] -> Mic2 (B_odd,  SEL=VCC) OK
  CH1 -> dma_buf_a[idx+1] -> Mic4 (A_odd,  SEL=VCC) OK
  CH2 -> dma_buf_a[idx]   -> Mic3 (A_even, SEL=GND) interferencia
  CH3 -> dma_buf_b[idx]   -> Mic1 (B_even, SEL=GND) interferencia

Nombre de archivo:
    json_test/session_<S>_ord<O>_mic<M>.json

Uso:
    python grabar_sesion.py --port COM6 --session 1 --baud 921600
    python grabar_sesion.py --port COM6 --session 2 --start-order 4 --baud 921600
    python grabar_sesion.py --session 1   # simulacion sin STM32
"""

import argparse
import json
import os
import sys
import time
import signal

try:
    import serial
except ImportError:
    print("[ERROR] Instala pyserial:  pip install pyserial")
    sys.exit(1)

# ── Configuracion ─────────────────────────────────────────────────────────────
SAMPLE_RATE   = 44100
OUTPUT_DIR    = "json_test/sound"
CHUNK_SECS    = 1
SAMPLES_CHUNK = SAMPLE_RATE * CHUNK_SECS   # 44100 muestras por canal

# Mapeo CH → microfono fisico real
MICS = {
    0: {"mic_id": 2, "mic_name": "Mic2_B_odd_SEL_VCC"},   # CH0 = Mic2 fisico
    1: {"mic_id": 4, "mic_name": "Mic4_A_odd_SEL_VCC"},   # CH1 = Mic4 fisico
    2: {"mic_id": 3, "mic_name": "Mic3_A_even_SEL_GND"},  # CH2 = Mic3 fisico
    3: {"mic_id": 1, "mic_name": "Mic1_B_even_SEL_GND"},  # CH3 = Mic1 fisico
}

CH_TAGS = ["CH0", "CH1", "CH2", "CH3"]

# ── Utilidades ────────────────────────────────────────────────────────────────

def sai24_to_int(word: int) -> int:
    s = word & 0x00FFFFFF
    if s & 0x00800000:
        s |= 0xFF000000
    if s > 0x7FFFFFFF:
        s -= 0x100000000
    return s

def ensure_dir(path: str):
    os.makedirs(path, exist_ok=True)

def filename(session_id: int, order: int, mic_id: int) -> str:
    return f"session_{session_id}_ord{order:04d}_mic{mic_id}.json"

def save_chunk(channels: dict, session_id: int, order: int, out_dir: str):
    """Guarda 4 JSONs (uno por mic) para un chunk completo."""
    for ch_idx, mic in MICS.items():
        samples = channels[ch_idx]
        n = len(samples)

        if n < SAMPLES_CHUNK - 100:
            print(f"\n[WARN] ch{ch_idx} muy corto: {n}/{SAMPLES_CHUNK} — descartado")
            return False

        # Truncar o rellenar hasta exactamente SAMPLES_CHUNK
        if n > SAMPLES_CHUNK:
            samples = samples[:SAMPLES_CHUNK]
        elif n < SAMPLES_CHUNK:
            samples = samples + [0] * (SAMPLES_CHUNK - n)

        norm  = [s / 8388608.0 for s in samples]
        fname = filename(session_id, order, mic["mic_id"])
        fpath = os.path.join(out_dir, fname)
        with open(fpath, "w") as f:
            json.dump(norm, f, separators=(",", ":"))

    return True


# ── Sesion de grabacion ───────────────────────────────────────────────────────

class GrabacionSession:

    def __init__(self, port, baud, session_id, start_order, out_dir):
        self.session_id  = session_id
        self.start_order = start_order
        self.out_dir     = out_dir
        self.order       = start_order
        self.chunks_ok   = 0
        self.stop        = False

        ensure_dir(out_dir)

        print(f"\n{'='*55}")
        print(f"  Sesion {session_id}  |  Orden inicial: {start_order}")
        print(f"  Chunk : {CHUNK_SECS}s = {SAMPLES_CHUNK} muestras/canal")
        print(f"  Salida: {out_dir}")
        print(f"  Ctrl+C para detener (chunk en curso se descarta)")
        print(f"{'='*55}\n")

        if port:
            try:
                self.ser = serial.Serial(port, baud, timeout=2.0)
                print(f"[UART] Conectado a {port} @ {baud} baud")
            except Exception as e:
                print(f"[ERROR] {e}")
                sys.exit(1)
            self.simulation = False

            # Esperar a que el STM32 este listo
            print("[UART] Esperando que el STM32 este listo...")
            t0   = time.time()
            sent = False
            while time.time() - t0 < 60 and not sent:
                try:
                    raw = self.ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("ascii", errors="ignore").strip()
                    if line:
                        print(f"  [STM32] {line}")
                    if ("esperando comando RECORD" in line or
                            "RECORD_READY" in line or
                            "esperando byte" in line or
                            "UART_RX" in line):
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
            self.ser = None
            self.simulation = True
            print("[SIM] Modo simulacion activo (sin --port)")

        signal.signal(signal.SIGINT, self._handle_stop)

    def _handle_stop(self, sig, frame):
        print(f"\n\n[STOP] Ctrl+C recibido — deteniendo sesion...")
        self.stop = True
        if self.ser:
            try:
                self.ser.write(b"STOP\n")
            except Exception:
                pass

    def _reset_chunk(self):
        return {ch: [] for ch in range(4)}

    def _receive_chunk(self):
        channels   = self._reset_chunk()
        current_ch = None
        total      = SAMPLES_CHUNK

        while not self.stop:
            try:
                raw = self.ser.readline()
            except serial.SerialException as e:
                print(f"\n[ERROR] UART: {e}")
                return None

            if not raw:
                continue

            try:
                line = raw.decode("ascii", errors="ignore").strip()
            except Exception:
                continue

            if not line:
                continue

            # Inicio de chunk
            if line.startswith("[CHUNK_START]"):
                parts      = line.split()
                total      = int(parts[1]) if len(parts) > 1 else SAMPLES_CHUNK
                channels   = self._reset_chunk()
                current_ch = None
                print(f"  → Chunk {self.order} recibiendo "
                      f"({total} muestras/canal)...", end="", flush=True)
                continue

            # Fin de chunk — retornar channels directamente
            if line.startswith("[CHUNK_END]"):
                sizes = [len(channels[ch]) for ch in range(4)]
                # Ignorar CHUNK_END vacío (llega del chunk anterior ya procesado)
                if all(s == 0 for s in sizes):
                    in_chunk = False
                    continue
                print(f"\n  ← CHUNK_END; tamaños={sizes}")
                in_chunk = False
                return channels

            # CHUNK_GUARDADO — cerrar solo si tenemos suficientes muestras
            if line.startswith("[CHUNK_GUARDADO]"):
                sizes  = [len(channels[ch]) for ch in range(4)]
                filled = sum(1 for s in sizes if s >= total - 200)
                if filled >= 4 and in_chunk:
                    print(f"\n  ← CHUNK_GUARDADO como cierre; tamaños={sizes}")
                    in_chunk = False
                    return channels
                else:
                    print(f"\n  [STM32] {line} (esperando mas muestras {sizes})")
                    continue

            # Inicio de canal
            if line.startswith("[CH") and "_START]" in line:
                try:
                    current_ch = CH_TAGS.index(line[1:4])
                except ValueError:
                    pass
                continue

            # Fin de canal
            if line.startswith("[CH") and "_END]" in line:
                current_ch = None
                continue

            # Muestra hex
            if current_ch is not None and 6 <= len(line) <= 8:
                try:
                    sample = sai24_to_int(int(line, 16))
                    if len(channels[current_ch]) < total:
                        channels[current_ch].append(sample)
                except ValueError:
                    pass

                # Imprimir progreso cada 10000 muestras del CH0
                if current_ch == 0:
                    n = len(channels[0])
                    if n % 10000 == 0 and n > 0:
                        sizes = [len(channels[ch]) for ch in range(4)]
                        print(f"\n    [PROG] {sizes}", end="", flush=True)

                # Retornar cuando TODOS los canales tienen suficientes muestras
                if all(len(channels[ch]) >= total for ch in range(4)):
                    sizes = [len(channels[ch]) for ch in range(4)]
                    print(f" completo [{', '.join(str(s) for s in sizes)}]")
                    return channels
                continue

            # Fin de grabacion
            if "RECORD_DONE" in line:
                print(f"\n[STM32] {line}")
                self.stop = True
                return None

            # Mensajes de progreso
            if current_ch is None:
                if "[GRABANDO]" in line:
                    print(f"\n  \U0001f399\ufe0f  {line}")
                elif "[CHUNK_GUARDADO]" in line:
                    print(f"\n  \u2705 {line}")
                elif "RECORD_READY" in line:
                    print(f"\n  \u25b6\ufe0f  Grabacion iniciada — acerca el audio al microfono")
                elif line:
                    print(f"  [STM32] {line}")

        return None

    def _simulate_chunk(self):
        import random
        return {
            0: [random.randint(-80000, 80000) for _ in range(SAMPLES_CHUNK)],
            1: [random.randint(-80000, 80000) for _ in range(SAMPLES_CHUNK)],
            2: [random.randint(-500,   500)   for _ in range(SAMPLES_CHUNK)],
            3: [random.randint(-500,   500)   for _ in range(SAMPLES_CHUNK)],
        }

    def run(self):
        try:
            while not self.stop:
                if self.simulation:
                    time.sleep(CHUNK_SECS)
                    if self.stop:
                        break
                    channels = self._simulate_chunk()
                else:
                    channels = self._receive_chunk()
                    if channels is None:
                        break

                sizes = [len(channels[ch]) for ch in range(4)]
                if any(s < SAMPLES_CHUNK - 100 for s in sizes):
                    print(f"\n[WARN] Chunk {self.order} muy corto {sizes} — descartado")
                    continue

                ok = save_chunk(channels, self.session_id, self.order, self.out_dir)
                if ok:
                    self.chunks_ok += 1
                    print(f"\n{'='*55}")
                    print(f"  \u2705 CHUNK {self.chunks_ok} GUARDADO")
                    print(f"     session_{self.session_id}_ord{self.order:04d}_mic2.json")
                    print(f"     session_{self.session_id}_ord{self.order:04d}_mic4.json")
                    print(f"     session_{self.session_id}_ord{self.order:04d}_mic3.json")
                    print(f"     session_{self.session_id}_ord{self.order:04d}_mic1.json")
                    print(f"     Total chunks: {self.chunks_ok}")
                    print(f"{'='*55}")
                    print(f"  Grabando siguiente chunk... (Ctrl+C para terminar)")
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
        print(f"  Carpeta          : {os.path.abspath(self.out_dir)}")
        print(f"{'='*55}\n")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description=f"Graba audio del STM32 en chunks exactos de {CHUNK_SECS}s."
    )
    parser.add_argument("--port",        default=None,
                        help="Puerto COM (ej. COM6). Omitir para simular.")
    parser.add_argument("--baud",        type=int, default=921600)
    parser.add_argument("--session",     type=int, default=1)
    parser.add_argument("--start-order", type=int, default=1)
    parser.add_argument("--output",      default=OUTPUT_DIR)
    args = parser.parse_args()

    session = GrabacionSession(
        port        = args.port,
        baud        = args.baud,
        session_id  = args.session,
        start_order = args.start_order,
        out_dir     = args.output,
    )
    session.run()


if __name__ == "__main__":
    main()