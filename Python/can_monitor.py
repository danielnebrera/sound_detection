"""
can_monitor.py
Monitor CAN para sistema de detección de drones — Portenta H7
Protocolo: SLCAN (RH02-PLUS USB CAN FD)
Baudrate: 500 kbps

Tramas recibidas:
  ID 0x100 — Probabilidades de los 4 micrófonos
  ID 0x101 — Estado general (EMA, alerta, dBFS)

Uso:
  python can_monitor.py                    # autodetecta puerto
  python can_monitor.py --port COM5        # puerto específico
  python can_monitor.py --log              # guarda CSV
  python can_monitor.py --port COM5 --log  # ambos
"""

import can
import time
import csv
import argparse
import struct
import serial.tools.list_ports
from datetime import datetime


# ── Configuración ─────────────────────────────────────────────
BITRATE     = 500_000
CAN_ID_PROB = 0x100   # Probabilidades mic1-mic4
CAN_ID_STAT = 0x101   # Estado: EMA, alerta, dBFS

ALERTA_NOMBRES = {
    0: "SIN DRON",
    1: "RASTREANDO",
    2: "ALERTA NARANJA",
    3: "ALERTA ROJA",
}

ALERTA_COLORES = {
    0: "\033[32m",   # verde
    1: "\033[33m",   # amarillo
    2: "\033[33m",   # amarillo
    3: "\033[31m",   # rojo
}
RESET = "\033[0m"
BOLD  = "\033[1m"


# ── Autodetección del puerto RH02-PLUS ────────────────────────
def detectar_puerto_can():
    """Busca automáticamente el puerto del RH02-PLUS / CANable."""
    keywords = ["canable", "can", "rh02", "stm32", "slcan", "usb serial"]
    puertos = serial.tools.list_ports.comports()

    for p in puertos:
        desc = (p.description or "").lower()
        mfr  = (p.manufacturer or "").lower()
        if any(k in desc or k in mfr for k in keywords):
            print(f"[AUTO] Puerto CAN detectado: {p.device} — {p.description}")
            return p.device

    # Si no detecta por nombre, mostrar lista y pedir al usuario
    if puertos:
        print("\n[INFO] Puertos disponibles:")
        for i, p in enumerate(puertos):
            print(f"  [{i}] {p.device} — {p.description}")
        try:
            idx = int(input("\nSelecciona el número del puerto CAN: "))
            return puertos[idx].device
        except (ValueError, IndexError):
            pass

    return None


# ── Decodificadores de tramas ─────────────────────────────────
def decode_0x100(data: bytes) -> dict:
    """
    Trama 0x100: probabilidades de los 4 micrófonos
    4 × uint16 big-endian, escalado ÷ 1000 → [0.0 - 1.0]
    """
    if len(data) < 8:
        return {}
    p = {}
    for i in range(4):
        raw = struct.unpack_from(">H", data, i * 2)[0]
        p[f"mic{i+1}"] = raw / 1000.0
    return p


def decode_0x101(data: bytes) -> dict:
    """
    Trama 0x101: estado general
    byte[0-1]: EMA × 1000 (uint16 big-endian)
    byte[2]:   alerta (0-3)
    byte[3]:   reservado
    byte[4-5]: dBFS mic1 × 10 (int16 big-endian)
    byte[6-7]: dBFS mic2 × 10 (int16 big-endian)
    """
    if len(data) < 8:
        return {}
    ema_raw    = struct.unpack_from(">H", data, 0)[0]
    alerta     = data[2]
    db1_raw    = struct.unpack_from(">h", data, 4)[0]
    db2_raw    = struct.unpack_from(">h", data, 6)[0]
    return {
        "ema":    ema_raw / 1000.0,
        "alerta": alerta,
        "db_mic1": db1_raw / 10.0,
        "db_mic2": db2_raw / 10.0,
    }


# ── Visualización en consola ──────────────────────────────────
def imprimir_estado(probs: dict, stat: dict, timestamp: str):
    """Imprime el estado actual del sistema de forma legible."""
    alerta  = stat.get("alerta", 0)
    ema     = stat.get("ema", 0.0)
    color   = ALERTA_COLORES.get(alerta, "")
    nombre  = ALERTA_NOMBRES.get(alerta, "DESCONOCIDO")

    print(f"\n{'─'*60}")
    print(f"  {BOLD}[{timestamp}]{RESET}")
    print(f"{'─'*60}")

    # Probabilidades de los 4 micrófonos con barra visual
    for i in range(1, 5):
        key = f"mic{i}"
        p   = probs.get(key, 0.0)
        bar_len = int(p * 20)
        bar = "█" * bar_len + "░" * (20 - bar_len)
        print(f"  Mic{i}: [{bar}] {p*100:5.1f}%")

    # dBFS disponibles de la trama 0x101
    db1 = stat.get("db_mic1", -120.0)
    db2 = stat.get("db_mic2", -120.0)
    print(f"\n  dBFS  Mic1: {db1:6.1f} dB  |  Mic2: {db2:6.1f} dB")

    # EMA y alerta
    ema_bar_len = int(ema * 20)
    ema_bar = "█" * ema_bar_len + "░" * (20 - ema_bar_len)
    print(f"\n  EMA:  [{ema_bar}] {ema*100:5.1f}%")
    print(f"\n  {color}{BOLD}Estado: {nombre}{RESET}")
    print(f"{'─'*60}")


# ── Logger CSV ────────────────────────────────────────────────
def crear_csv(filename: str):
    """Crea el archivo CSV con cabecera."""
    f = open(filename, "w", newline="", encoding="utf-8")
    writer = csv.writer(f)
    writer.writerow([
        "timestamp", "unix_time",
        "p_mic1", "p_mic2", "p_mic3", "p_mic4",
        "ema", "alerta", "alerta_nombre",
        "db_mic1", "db_mic2"
    ])
    return f, writer


# ── Main ──────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Monitor CAN — Sistema de detección de drones Portenta H7"
    )
    parser.add_argument("--port", type=str, default=None,
                        help="Puerto COM del RH02-PLUS (ej: COM5 o /dev/ttyUSB0)")
    parser.add_argument("--log", action="store_true",
                        help="Guardar datos en archivo CSV")
    args = parser.parse_args()

    # Determinar puerto
    port = args.port or detectar_puerto_can()
    if port is None:
        print("[ERROR] No se encontró ningún puerto CAN. Usa --port COMx")
        return

    # Archivo de log
    csv_file = csv_writer = None
    if args.log:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"drone_log_{ts}.csv"
        csv_file, csv_writer = crear_csv(filename)
        print(f"[LOG] Guardando en: {filename}")

    # Conectar al bus CAN
    print(f"\n[CAN] Conectando a {port} @ {BITRATE//1000} kbps...")
    try:
        bus = can.interface.Bus(
            interface="slcan",
            channel=port,
            tty_baudrate=115200,      # velocidad del puerto serie USB
            bitrate=BITRATE,          # velocidad CAN = 500000
            sleep_after_open=2.0,
            rtscts=False
        )
    except Exception as e:
        print(f"[ERROR] No se pudo abrir el bus CAN: {e}")
        return

    print("[CAN] Conectado. Esperando tramas...\n")
    print("  Ctrl+C para salir\n")

    # Estado acumulado (esperamos recibir ambas tramas antes de imprimir)
    probs = {}
    stat  = {}
    last_print = 0.0

    try:
        while True:
            msg = bus.recv(timeout=5.0)

            if msg is None:
                print("[WAIT] Sin datos en 5s — verificar conexión y firmware...")
                continue

            now = time.time()
            ts  = datetime.now().strftime("%H:%M:%S.%f")[:-3]

            if msg.arbitration_id == CAN_ID_PROB:
                probs = decode_0x100(msg.data)

            elif msg.arbitration_id == CAN_ID_STAT:
                stat = decode_0x101(msg.data)

                # Imprimir cuando llega 0x101 (llega después de 0x100)
                if probs:
                    imprimir_estado(probs, stat, ts)

                    # Log CSV
                    if csv_writer and probs and stat:
                        alerta = stat.get("alerta", 0)
                        csv_writer.writerow([
                            ts, f"{now:.3f}",
                            f"{probs.get('mic1', 0):.4f}",
                            f"{probs.get('mic2', 0):.4f}",
                            f"{probs.get('mic3', 0):.4f}",
                            f"{probs.get('mic4', 0):.4f}",
                            f"{stat.get('ema', 0):.4f}",
                            alerta,
                            ALERTA_NOMBRES.get(alerta, ""),
                            f"{stat.get('db_mic1', -120):.1f}",
                            f"{stat.get('db_mic2', -120):.1f}",
                        ])
                        if csv_file:
                            csv_file.flush()

            else:
                # Mostrar tramas desconocidas en hex para debug
                hex_data = " ".join(f"{b:02X}" for b in msg.data)
                print(f"  [0x{msg.arbitration_id:03X}] {hex_data}")

    except KeyboardInterrupt:
        print("\n\n[INFO] Monitor detenido por el usuario.")

    finally:
        bus.shutdown()
        if csv_file:
            csv_file.close()
            print(f"[LOG] Archivo guardado.")


if __name__ == "__main__":
    main()