#!/usr/bin/env python3
"""
json_to_wav.py

Convierte los JSONs de audio generados por grabar_sesion.py a archivos WAV.
Los JSONs contienen arrays de floats normalizados [-1, 1] a 44100 Hz.

Uso:
    # Convertir todos los JSONs de una sesión
    python json_to_wav.py --session 1

    # Convertir solo un mic específico
    python json_to_wav.py --session 1 --mic 2

    # Convertir todos los JSONs de una carpeta
    python json_to_wav.py --input json_test --session 1

    # Especificar carpeta de salida
    python json_to_wav.py --session 1 --output wavs/
"""

import argparse
import json
import os
import struct
import sys
import glob


SAMPLE_RATE = 44100
SAMPLE_WIDTH = 2       # int16 = 2 bytes
N_CHANNELS = 1         # mono


def floats_to_wav(samples: list, filepath: str, sample_rate: int = SAMPLE_RATE):
    """
    Guarda una lista de floats [-1, 1] como archivo WAV mono int16.
    Equivale exactamente a lo que hace el Web Audio API con los JSON.
    """
    # Convertir float [-1, 1] a int16 [-32767, 32767]
    pcm = []
    for s in samples:
        val = max(-1.0, min(1.0, float(s)))       # clamp
        pcm.append(int(val * 32767))

    n_samples   = len(pcm)
    data_size   = n_samples * SAMPLE_WIDTH
    chunk_size  = 36 + data_size

    with open(filepath, 'wb') as f:
        # RIFF header
        f.write(b'RIFF')
        f.write(struct.pack('<I', chunk_size))
        f.write(b'WAVE')

        # fmt chunk
        f.write(b'fmt ')
        f.write(struct.pack('<I', 16))                          # chunk size
        f.write(struct.pack('<H', 1))                           # PCM = 1
        f.write(struct.pack('<H', N_CHANNELS))                  # canales
        f.write(struct.pack('<I', sample_rate))                 # sample rate
        f.write(struct.pack('<I', sample_rate * SAMPLE_WIDTH))  # byte rate
        f.write(struct.pack('<H', SAMPLE_WIDTH))                # block align
        f.write(struct.pack('<H', 16))                          # bits per sample

        # data chunk
        f.write(b'data')
        f.write(struct.pack('<I', data_size))
        for v in pcm:
            f.write(struct.pack('<h', v))


def convert_session(session_id: int, input_dir: str, output_dir: str,
                    mic_filter: int = None):
    """
    Convierte todos los JSONs de una sesión a WAV.
    Agrupa por orden y crea un WAV por (orden, mic).
    """
    os.makedirs(output_dir, exist_ok=True)

    # Buscar todos los JSONs de la sesión
    pattern = os.path.join(input_dir, f"session_{session_id}_ord*_mic*.json")
    files   = sorted(glob.glob(pattern))

    if not files:
        print(f"[ERROR] No se encontraron archivos en: {pattern}")
        sys.exit(1)

    # Filtrar por mic si se especificó
    if mic_filter is not None:
        files = [f for f in files if f"_mic{mic_filter}.json" in f]
        if not files:
            print(f"[ERROR] No hay archivos para mic{mic_filter} en sesión {session_id}")
            sys.exit(1)

    print(f"\nSesión {session_id} — {len(files)} archivos encontrados")
    print(f"Salida: {os.path.abspath(output_dir)}\n")

    converted = 0
    errors    = 0

    for json_path in files:
        basename = os.path.basename(json_path)          # session_1_ord0001_mic2.json
        wav_name = basename.replace('.json', '.wav')
        wav_path = os.path.join(output_dir, wav_name)

        try:
            with open(json_path, 'r') as f:
                samples = json.load(f)

            if not isinstance(samples, list):
                print(f"  [WARN] {basename}: JSON no es un array — ignorado")
                errors += 1
                continue

            if len(samples) == 0:
                print(f"  [WARN] {basename}: array vacío — ignorado")
                errors += 1
                continue

            floats_to_wav(samples, wav_path)
            duration_s = len(samples) / SAMPLE_RATE
            print(f"  ✅ {wav_name}  ({len(samples)} muestras, {duration_s:.3f}s)")
            converted += 1

        except json.JSONDecodeError as e:
            print(f"  [ERROR] {basename}: JSON inválido — {e}")
            errors += 1
        except Exception as e:
            print(f"  [ERROR] {basename}: {e}")
            errors += 1

    print(f"\n{'='*50}")
    print(f"  Convertidos : {converted}")
    print(f"  Errores     : {errors}")
    print(f"  Carpeta WAV : {os.path.abspath(output_dir)}")
    print(f"{'='*50}\n")


def main():
    parser = argparse.ArgumentParser(
        description="Convierte JSONs de audio (floats [-1,1]) a WAV mono 44100Hz."
    )
    parser.add_argument("--session",  type=int, required=True,
                        help="ID de sesión (ej. 1)")
    parser.add_argument("--mic",      type=int, default=None,
                        help="Convertir solo este mic (1-4). Omitir = todos")
    parser.add_argument("--input",    default="json_test",
                        help="Carpeta de entrada con los JSON (default: json_test)")
    parser.add_argument("--output",   default=None,
                        help="Carpeta de salida WAV (default: wavs/session_<N>)")
    args = parser.parse_args()

    out = args.output or os.path.join("wavs", f"session_{args.session}")

    convert_session(
        session_id = args.session,
        input_dir  = args.input,
        output_dir = out,
        mic_filter = args.mic,
    )


if __name__ == "__main__":
    main()