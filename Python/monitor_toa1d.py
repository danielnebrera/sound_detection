import argparse
import sys
import time

import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=1000000)
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
        time.sleep(0.3)
        ser.reset_input_buffer()

        print("Esperando [RECORD_READY]...")
        started = False

        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue

                line = raw.decode("utf-8", errors="replace").rstrip()
                print(line)

                if (not started) and line.startswith("[RECORD_READY]"):
                    ser.write(b"R")
                    ser.flush()
                    started = True

                if line.startswith("[TOA1D_END]"):
                    break

        except KeyboardInterrupt:
            print("\nSTOP solicitado...")
            ser.write(b"\x03")
            ser.flush()

            deadline = time.time() + 5.0
            while time.time() < deadline:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip()
                print(line)
                if line.startswith("[TOA1D_END]"):
                    break


if __name__ == "__main__":
    sys.exit(main())
