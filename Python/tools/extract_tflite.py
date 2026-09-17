"""
Extrae drone_mfcc_model.tflite desde CM7/X-CUBE-AI/App/network.c

network.c contiene el modelo como C array:
    const uint8_t g_tflm_network_model_data[] DATA_ALIGN_ATTRIBUTE = {
        0x1c, 0x00, 0x00, 0x00, ...
    };

Este script lee ese array y lo escribe como binario TFLite.

Uso:
    python tools/extract_tflite.py
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC  = ROOT / "CM7" / "X-CUBE-AI" / "App" / "network.c"
OUT  = Path(__file__).resolve().parent.parent / "drone_mfcc_model.tflite"

BYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})")


def main() -> None:
    if not SRC.exists():
        raise SystemExit(f"No encuentro {SRC}")

    in_array = False
    bytes_out = bytearray()

    with SRC.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if not in_array:
                if "g_tflm_network_model_data[]" in line and "{" in line:
                    in_array = True
                    remainder = line.split("{", 1)[1]
                    bytes_out.extend(int(m.group(1), 16) for m in BYTE_RE.finditer(remainder))
                continue

            if "};" in line:
                head = line.split("};", 1)[0]
                bytes_out.extend(int(m.group(1), 16) for m in BYTE_RE.finditer(head))
                break

            bytes_out.extend(int(m.group(1), 16) for m in BYTE_RE.finditer(line))

    if not bytes_out:
        raise SystemExit("No se extrajeron bytes. ¿Cambió el formato de network.c?")

    if bytes_out[4:8] != b"TFL3":
        print(f"AVISO: cabecera inesperada {bytes(bytes_out[:8]).hex()} — se guarda igualmente.")

    OUT.write_bytes(bytes_out)
    print(f"OK: {OUT}  ({len(bytes_out):,} bytes)")


if __name__ == "__main__":
    main()
