import argparse
import csv
import math
import queue
import re
import sys
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path

import serial
import tkinter as tk
from tkinter import ttk


ALERTS = {
    0: ("SIN ALERTA", "#25a244"),
    1: ("ALERTA MANTENIDA", "#d4a017"),
    2: ("DRON LEJANO", "#f08c00"),
    3: ("DRON CERCANO", "#d62828"),
}


def search_int(line, key):
    m = re.search(rf"(?:^|\s){re.escape(key)}=(\d+)", line)
    return int(m.group(1)) if m else None


def search_hex(line, key):
    m = re.search(rf"(?:^|\s){re.escape(key)}=0x([0-9A-Fa-f]+)", line)
    return int(m.group(1), 16) if m else None


def search_float(line, key):
    m = re.search(
        rf"(?:^|\s){re.escape(key)}=([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)",
        line,
    )
    return float(m.group(1)) if m else None


def search_word(line, key):
    m = re.search(rf"(?:^|\s){re.escape(key)}=([A-Z_]+)", line)
    return m.group(1) if m else None


def clamp(value, lo, hi):
    return max(lo, min(hi, value))


class LiveModel:
    def __init__(self, h_scale, v_scale, zenith_rho, history_len):
        self.h_scale = h_scale
        self.v_scale = v_scale
        self.zenith_rho = zenith_rho
        self.history = deque(maxlen=history_len)
        self.by_seq = {}
        self.current = {
            "seq": 0,
            "alert": 0,
            "fused": None,
            "ema": None,
            "rel_db": None,
            "active": None,
            "toa_status": "NOT_EVALUATED",
            "loc_valid": 0,
            "raw_dir": "NONE",
            "raw_az": None,
            "closure": None,
            "h": None,
            "v": None,
            "rho": None,
            "elev_exp": None,
            "spatial": "NOT_EVALUATED",
            "confidence": "NONE",
            "track_eligible": 0,
            "ambiguous_pairs": None,
            "peak_gap_min": None,
            "corr_min": None,
            "corr_mean": None,
            "solver_used": None,
            "solver_ratio": None,
        }

    def entry(self, seq):
        if seq not in self.by_seq:
            self.by_seq[seq] = {"seq": seq}
        return self.by_seq[seq]

    def feed_live_det(self, line):
        seq = search_int(line, "order")
        if seq is None:
            return
        e = self.entry(seq)
        e.update(
            alert=search_int(line, "alert"),
            fused=search_float(line, "fused"),
            ema=search_float(line, "ema"),
            rel_db=search_float(line, "rel_db"),
            baseline=search_int(line, "baseline"),
            active=search_hex(line, "active"),
            det_us=search_int(line, "det_us"),
        )

    def feed_toa(self, line):
        seq = search_int(line, "seq")
        if seq is None:
            return
        e = self.entry(seq)
        e.update(
            toa_valid=search_int(line, "valid"),
            closure_valid=search_int(line, "cvalid"),
            flags=search_hex(line, "flags"),
            raw_dir=search_word(line, "dir"),
            raw_az=search_float(line, "az"),
            h=search_float(line, "H"),
            v=search_float(line, "V"),
            closure=search_float(line, "cl"),
            toa_us=search_int(line, "toa_us"),
            proc_us=search_int(line, "proc_us"),
        )

    def feed_confidence(self, line):
        seq = search_int(line, "seq")
        if seq is None:
            return
        e = self.entry(seq)
        e.update(
            confidence=search_word(line, "level") or "NONE",
            track_eligible=search_int(line, "track") or 0,
            ambiguous_pairs=search_int(line, "amb"),
            peak_gap_min=search_float(line, "gap_min"),
            corr_min=search_float(line, "corr_min"),
            corr_mean=search_float(line, "corr_mean"),
            solver_used=search_int(line, "solver"),
            solver_ratio=search_float(line, "ratio"),
        )

    def spatial_from_entry(self, e):
        if not e.get("toa_valid"):
            return "UNKNOWN", None, None

        h = e.get("h")
        v = e.get("v")
        if h is None or v is None:
            return "AZIMUTH", None, None

        ux = -h / self.h_scale
        uy = v / self.v_scale
        rho = math.hypot(ux, uy)

        if rho <= 1.05:
            elev = math.degrees(math.acos(clamp(rho, 0.0, 1.0)))
        else:
            elev = None

        if rho <= self.zenith_rho:
            return "ZENITH_CANDIDATE", rho, elev

        return "AZIMUTH", rho, elev

    def feed_target(self, line):
        seq = search_int(line, "seq")
        if seq is None:
            return None

        e = self.entry(seq)
        alert = search_int(line, "alert")
        detected = search_int(line, "detected")
        loc_valid = search_int(line, "loc_valid")
        toa_status = search_word(line, "toa")
        target_dir = search_word(line, "dir")
        target_az = search_float(line, "az")
        closure = search_float(line, "cl")
        target_conf = search_word(line, "conf")
        target_track = search_int(line, "track")

        if alert is not None:
            e["alert"] = alert
        e["detected"] = detected
        e["loc_valid"] = loc_valid
        e["toa_status"] = toa_status or "UNKNOWN"
        if target_dir is not None:
            e["target_dir"] = target_dir
        if target_az is not None:
            e["target_az"] = target_az
        if closure is not None:
            e["closure"] = closure
        if target_conf is not None:
            e["confidence"] = target_conf
        if target_track is not None:
            e["track_eligible"] = target_track

        if e["toa_status"] == "NOT_EVALUATED":
            spatial = "NOT_EVALUATED"
            rho = None
            elev = None
            self.history.append(None)
        elif loc_valid:
            spatial, rho, elev = self.spatial_from_entry(e)
            az = e.get("raw_az", target_az)
            if spatial == "ZENITH_CANDIDATE":
                self.history.append((seq, az, rho, spatial, e.get("confidence", "NONE"), e.get("track_eligible", 0)))
            elif az is not None and rho is not None:
                self.history.append((seq, az, clamp(rho, 0.0, 1.0), spatial, e.get("confidence", "NONE"), e.get("track_eligible", 0)))
        else:
            spatial = "UNKNOWN"
            rho = None
            elev = None
            self.history.append(None)

        e["spatial"] = spatial
        e["rho"] = rho
        e["elev_exp"] = elev

        self.current = {
            "seq": seq,
            "alert": e.get("alert", 0),
            "fused": e.get("fused"),
            "ema": e.get("ema"),
            "rel_db": e.get("rel_db"),
            "active": e.get("active"),
            "toa_status": e.get("toa_status"),
            "loc_valid": e.get("loc_valid", 0),
            "raw_dir": e.get("raw_dir", e.get("target_dir", "NONE")),
            "raw_az": e.get("raw_az", target_az),
            "closure": e.get("closure"),
            "h": e.get("h"),
            "v": e.get("v"),
            "rho": rho,
            "elev_exp": elev,
            "spatial": spatial,
            "confidence": e.get("confidence", "NONE"),
            "track_eligible": e.get("track_eligible", 0),
            "ambiguous_pairs": e.get("ambiguous_pairs"),
            "peak_gap_min": e.get("peak_gap_min"),
            "corr_min": e.get("corr_min"),
            "corr_mean": e.get("corr_mean"),
            "solver_used": e.get("solver_used"),
            "solver_ratio": e.get("solver_ratio"),
        }
        return e


class CsvWriter:
    FIELDS = [
        "timestamp",
        "seq",
        "alert",
        "fused",
        "ema",
        "rel_db",
        "active",
        "toa_status",
        "loc_valid",
        "raw_dir",
        "raw_az",
        "H",
        "V",
        "closure",
        "rho_exp",
        "elevation_exp_deg",
        "spatial_exp",
        "confidence",
        "track_eligible",
        "ambiguous_pairs",
        "peak_gap_min",
        "corr_min",
        "corr_mean",
        "solver_used",
        "solver_ratio",
    ]

    def __init__(self, path):
        self.file = None
        self.writer = None
        if path:
            self.file = open(path, "w", newline="", encoding="utf-8")
            self.writer = csv.DictWriter(self.file, fieldnames=self.FIELDS)
            self.writer.writeheader()
            self.file.flush()

    def write(self, c):
        if not self.writer:
            return
        row = {
            "timestamp": datetime.now().isoformat(timespec="milliseconds"),
            "seq": c.get("seq"),
            "alert": c.get("alert"),
            "fused": c.get("fused"),
            "ema": c.get("ema"),
            "rel_db": c.get("rel_db"),
            "active": c.get("active"),
            "toa_status": c.get("toa_status"),
            "loc_valid": c.get("loc_valid"),
            "raw_dir": c.get("raw_dir"),
            "raw_az": c.get("raw_az"),
            "H": c.get("h"),
            "V": c.get("v"),
            "closure": c.get("closure"),
            "rho_exp": c.get("rho"),
            "elevation_exp_deg": c.get("elev_exp"),
            "spatial_exp": c.get("spatial"),
            "confidence": c.get("confidence"),
            "track_eligible": c.get("track_eligible"),
            "ambiguous_pairs": c.get("ambiguous_pairs"),
            "peak_gap_min": c.get("peak_gap_min"),
            "corr_min": c.get("corr_min"),
            "corr_mean": c.get("corr_mean"),
            "solver_used": c.get("solver_used"),
            "solver_ratio": c.get("solver_ratio"),
        }
        self.writer.writerow(row)
        self.file.flush()

    def close(self):
        if self.file:
            self.file.close()


class UartLogWriter:
    def __init__(self, path):
        self.path = path
        self.file = None
        if path:
            self.file = open(path, "w", encoding="utf-8", buffering=1)

    def write(self, line):
        if self.file:
            ts = datetime.now().isoformat(timespec="milliseconds")
            self.file.write(f"{ts} {line}\n")

    def close(self):
        if self.file:
            self.file.close()
            self.file = None


class SerialWorker(threading.Thread):
    def __init__(self, port, baud, messages):
        super().__init__(daemon=True)
        self.port = port
        self.baud = baud
        self.messages = messages
        self.ser = None
        self.running = True
        self.started = False

    def run(self):
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=0.1)
            time.sleep(0.3)
            self.ser.reset_input_buffer()
            self.messages.put(("status", "Esperando [RECORD_READY]..."))

            while self.running:
                raw = self.ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip()
                self.messages.put(("line", line))

                if (not self.started) and line.startswith("[RECORD_READY]"):
                    self.ser.write(b"R")
                    self.ser.flush()
                    self.started = True
                    self.messages.put(("status", "LIVE"))

                if (line.startswith("[TOA1E3_END]") or
                        line.startswith("[TOA1E2_END]") or
                        line.startswith("[TOA1E1_END]")):
                    self.messages.put(("ended", line))
                    break
        except Exception as exc:
            self.messages.put(("error", str(exc)))
        finally:
            if self.ser:
                try:
                    self.ser.close()
                except Exception:
                    pass

    def stop_capture(self):
        if self.ser and self.ser.is_open:
            try:
                self.ser.write(b"S")
                self.ser.flush()
                self.messages.put(("status", "STOP solicitado; esperando frontera de segundo..."))
            except Exception as exc:
                self.messages.put(("error", str(exc)))

    def close(self):
        self.running = False


class App:
    def __init__(self, root, args):
        self.root = root
        self.args = args
        self.messages = queue.Queue()
        self.model = LiveModel(args.h_scale, args.v_scale, args.zenith_rho, args.history)
        self.csv = CsvWriter(args.csv)
        log_path = args.log
        if log_path is None and args.csv:
            csv_path = Path(args.csv)
            log_path = str(csv_path.with_name(csv_path.stem + "_uart.log"))
        self.uart_log = UartLogWriter(log_path)
        if log_path:
            print(f"UART log: {log_path}")
        self.worker = SerialWorker(args.port, args.baud, self.messages)
        self.worker.start()

        root.title("Khamex TOA-1E3.2 LIVE")
        root.geometry("1180x760")
        root.minsize(1000, 650)
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        root.bind("<Control-c>", lambda event: self.request_stop())

        self.status_var = tk.StringVar(value="Conectando...")
        self.seq_var = tk.StringVar(value="0")
        self.alert_var = tk.StringVar(value="SIN ALERTA")
        self.prob_var = tk.StringVar(value="--")
        self.level_var = tk.StringVar(value="--")
        self.toa_var = tk.StringVar(value="NOT_EVALUATED")
        self.dir_var = tk.StringVar(value="--")
        self.az_var = tk.StringVar(value="--")
        self.elev_var = tk.StringVar(value="--")
        self.rho_var = tk.StringVar(value="--")
        self.closure_var = tk.StringVar(value="--")
        self.spatial_var = tk.StringVar(value="NOT_EVALUATED")
        self.conf_var = tk.StringVar(value="NONE")
        self.track_var = tk.StringVar(value="0")
        self.amb_var = tk.StringVar(value="--")
        self.gap_var = tk.StringVar(value="--")

        self.build_ui()
        self.root.after(50, self.poll)

    def build_ui(self):
        outer = ttk.Frame(self.root, padding=12)
        outer.pack(fill="both", expand=True)

        top = ttk.Frame(outer)
        top.pack(fill="x")
        ttk.Label(top, text="KHAMEX TOA LIVE", font=("Segoe UI", 20, "bold")).pack(side="left")
        ttk.Label(top, textvariable=self.status_var).pack(side="left", padx=20)
        ttk.Button(top, text="Reset trayectoria", command=self.reset_track).pack(side="right", padx=6)
        ttk.Button(top, text="STOP", command=self.request_stop).pack(side="right", padx=6)

        body = ttk.Frame(outer)
        body.pack(fill="both", expand=True, pady=(12, 0))
        body.columnconfigure(0, weight=1)
        body.columnconfigure(1, weight=2)
        body.rowconfigure(0, weight=1)

        left = ttk.Frame(body, padding=8)
        left.grid(row=0, column=0, sticky="nsew")

        self.alert_canvas = tk.Canvas(left, width=190, height=190, highlightthickness=0)
        self.alert_canvas.pack(pady=(4, 0))
        self.alert_circle = self.alert_canvas.create_oval(25, 25, 165, 165, fill="#25a244", outline="")
        self.alert_text = self.alert_canvas.create_text(95, 95, text="SIN ALERTA", width=130, font=("Segoe UI", 15, "bold"), fill="white")

        ttk.Label(left, text="Estado detector", font=("Segoe UI", 11, "bold")).pack(pady=(4, 0))
        ttk.Label(left, textvariable=self.alert_var, font=("Segoe UI", 14, "bold")).pack()

        grid = ttk.Frame(left)
        grid.pack(fill="x", pady=14)
        rows = [
            ("Segundo", self.seq_var),
            ("Probabilidad fused", self.prob_var),
            ("Nivel relativo max", self.level_var),
            ("TOA", self.toa_var),
            ("Spatial EXP", self.spatial_var),
            ("Direccion RAW", self.dir_var),
            ("Azimuth RAW", self.az_var),
            ("Elevation EXP", self.elev_var),
            ("rho EXP", self.rho_var),
            ("Closure", self.closure_var),
            ("Confidence", self.conf_var),
            ("Track eligible", self.track_var),
            ("Ambiguous pairs", self.amb_var),
            ("Peak gap min", self.gap_var),
        ]
        for r, (name, var) in enumerate(rows):
            ttk.Label(grid, text=name).grid(row=r, column=0, sticky="w", padx=(0, 12), pady=3)
            ttk.Label(grid, textvariable=var, font=("Consolas", 11, "bold")).grid(row=r, column=1, sticky="e", pady=3)
        grid.columnconfigure(1, weight=1)

        note = (
            "Elevation y ZENITH son EXPERIMENTALES. "
            "Se asume hemisferio superior. El centro del radar es ZENITH; "
            "el borde es el horizonte. Todos los puntos son RAW; la linea solo une "
            f"muestras consecutivas con salto espacial <= {self.args.max_link_step_deg:.0f} deg. "
            "LOW se dibuja como X y no se enlaza. HIGH/MEDIUM pueden enlazarse. "
            "Todavia no es un tracker."
        )
        ttk.Label(left, text=note, wraplength=330, foreground="#555555").pack(fill="x", pady=8)

        right = ttk.Frame(body, padding=8)
        right.grid(row=0, column=1, sticky="nsew")
        right.rowconfigure(1, weight=1)
        right.columnconfigure(0, weight=1)
        ttk.Label(right, text="Trayectoria angular RAW", font=("Segoe UI", 15, "bold")).grid(row=0, column=0, sticky="w")
        self.radar = tk.Canvas(right, bg="white", highlightthickness=1, highlightbackground="#bbbbbb")
        self.radar.grid(row=1, column=0, sticky="nsew", pady=(8, 0))
        self.radar.bind("<Configure>", lambda event: self.draw_radar())

    def reset_track(self):
        self.model.history.clear()
        self.draw_radar()

    def request_stop(self):
        self.worker.stop_capture()

    def on_close(self):
        self.worker.stop_capture()
        self.worker.close()
        self.csv.close()
        self.uart_log.close()
        self.root.destroy()

    def poll(self):
        try:
            while True:
                kind, payload = self.messages.get_nowait()
                if kind == "line":
                    self.handle_line(payload)
                elif kind == "status":
                    self.status_var.set(payload)
                elif kind == "error":
                    self.status_var.set("ERROR: " + payload)
                elif kind == "ended":
                    self.status_var.set("STOPPED")
        except queue.Empty:
            pass
        self.root.after(50, self.poll)

    def handle_line(self, line):
        print(line)
        self.uart_log.write(line)
        if line.startswith("[LIVE_DET]"):
            self.model.feed_live_det(line)
        elif line.startswith("[TOA]"):
            self.model.feed_toa(line)
        elif line.startswith("[TOA_CONF]"):
            self.model.feed_confidence(line)
        elif line.startswith("[TARGET]"):
            e = self.model.feed_target(line)
            if e is not None:
                self.refresh()
                self.csv.write(self.model.current)
                c = self.model.current
                rho = "NA" if c["rho"] is None else f"{c['rho']:.3f}"
                elev = "NA" if c["elev_exp"] is None else f"{c['elev_exp']:.1f}"
                spatial_line = (
                    f"[SPATIAL_EXP] seq={c['seq']} mode={c['spatial']} "
                    f"rho={rho} elev_exp={elev} upper_hemisphere=1"
                )
                print(spatial_line)
                self.uart_log.write(spatial_line)

    def refresh(self):
        c = self.model.current
        alert = c.get("alert") or 0
        label, color = ALERTS.get(alert, ("DESCONOCIDA", "#666666"))
        self.alert_var.set(f"{alert} - {label}")
        self.alert_canvas.itemconfigure(self.alert_circle, fill=color)
        self.alert_canvas.itemconfigure(self.alert_text, text=label)
        self.seq_var.set(str(c.get("seq", 0)))

        fused = c.get("fused")
        self.prob_var.set("--" if fused is None else f"{100.0 * fused:.1f} %")
        rel_db = c.get("rel_db")
        self.level_var.set("--" if rel_db is None else f"{rel_db:+.2f} dB")
        self.toa_var.set(c.get("toa_status") or "--")
        self.spatial_var.set(c.get("spatial") or "--")
        self.dir_var.set(c.get("raw_dir") or "--")

        raw_az = c.get("raw_az")
        if c.get("spatial") == "ZENITH_CANDIDATE":
            self.az_var.set("N/A (ZENITH EXP)")
        else:
            self.az_var.set("--" if raw_az is None else f"{raw_az:.2f} deg")

        elev = c.get("elev_exp")
        self.elev_var.set("--" if elev is None else f"{elev:.1f} deg EXP")
        rho = c.get("rho")
        self.rho_var.set("--" if rho is None else f"{rho:.3f}")
        closure = c.get("closure")
        self.closure_var.set("--" if closure is None else f"{closure:.2f} samp")
        self.conf_var.set(c.get("confidence") or "NONE")
        self.track_var.set(str(c.get("track_eligible", 0)))
        amb = c.get("ambiguous_pairs")
        self.amb_var.set("--" if amb is None else str(amb))
        gap = c.get("peak_gap_min")
        self.gap_var.set("--" if gap is None else f"{gap:.3f}")
        self.draw_radar()

    def draw_radar(self):
        cv = self.radar
        cv.delete("all")
        w = max(100, cv.winfo_width())
        h = max(100, cv.winfo_height())
        cx = w / 2.0
        cy = h / 2.0
        radius = max(20.0, min(w, h) * 0.42)

        for fraction in (0.25, 0.5, 0.75, 1.0):
            r = radius * fraction
            cv.create_oval(cx-r, cy-r, cx+r, cy+r, outline="#d0d0d0")

        cv.create_line(cx, cy-radius, cx, cy+radius, fill="#b0b0b0")
        cv.create_line(cx-radius, cy, cx+radius, cy, fill="#b0b0b0")
        cv.create_text(cx, cy-radius-14, text="UP / 0 deg")
        cv.create_text(cx+radius+34, cy, text="RIGHT\n90 deg")
        cv.create_text(cx, cy+radius+18, text="DOWN / 180 deg")
        cv.create_text(cx-radius-34, cy, text="LEFT\n270 deg")
        cv.create_text(cx+6, cy+12, text="ZENITH", anchor="nw", fill="#666666")
        cv.create_text(cx+radius-70, cy-radius+18, text="HORIZON", fill="#666666")

        def unit_vector(az_deg, rho_value):
            rho_clamped = clamp(rho_value, 0.0, 1.0)
            az_rad = math.radians(az_deg)
            z = math.sqrt(max(0.0, 1.0 - rho_clamped * rho_clamped))
            return (
                rho_clamped * math.sin(az_rad),
                rho_clamped * math.cos(az_rad),
                z,
            )

        def angular_step_deg(a, b):
            dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
            dot = clamp(dot, -1.0, 1.0)
            return math.degrees(math.acos(dot))

        previous = None
        previous_vector = None
        for item in self.model.history:
            if item is None:
                previous = None
                previous_vector = None
                continue
            seq, az, rho, spatial, confidence, track_eligible = item
            if az is None:
                az = 0.0
            rho_for_vector = clamp(rho, 0.0, 1.0)
            vector = unit_vector(az, rho_for_vector)
            rr = 0.0 if spatial == "ZENITH_CANDIDATE" else radius * rho_for_vector
            rad = math.radians(az)
            x = cx + rr * math.sin(rad)
            y = cy - rr * math.cos(rad)
            if track_eligible:
                if previous is not None and previous_vector is not None:
                    step_deg = angular_step_deg(previous_vector, vector)
                    if step_deg <= self.args.max_link_step_deg:
                        cv.create_line(previous[0], previous[1], x, y, width=2, fill="#2f6fdd")
                cv.create_oval(x-3, y-3, x+3, y+3, fill="#2f6fdd", outline="")
                previous = (x, y)
                previous_vector = vector
            else:
                cv.create_line(x-4, y-4, x+4, y+4, width=2, fill="#a33a3a")
                cv.create_line(x-4, y+4, x+4, y-4, width=2, fill="#a33a3a")
                previous = None
                previous_vector = None

        if previous is not None:
            cv.create_oval(previous[0]-6, previous[1]-6, previous[0]+6, previous[1]+6, outline="#111111", width=2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=1000000)
    parser.add_argument("--h-scale", type=float, default=8.59)
    parser.add_argument("--v-scale", type=float, default=15.13)
    parser.add_argument("--zenith-rho", type=float, default=0.20)
    parser.add_argument("--history", type=int, default=180)
    parser.add_argument("--max-link-step-deg", type=float, default=20.0)
    parser.add_argument("--csv", default=None)
    parser.add_argument("--log", default=None)
    args = parser.parse_args()

    if args.h_scale <= 0.0 or args.v_scale <= 0.0:
        parser.error("h-scale y v-scale deben ser > 0")
    if not 0.0 < args.zenith_rho < 1.0:
        parser.error("zenith-rho debe estar entre 0 y 1")
    if args.max_link_step_deg <= 0.0 or args.max_link_step_deg > 180.0:
        parser.error("max-link-step-deg debe estar entre 0 y 180")

    root = tk.Tk()
    App(root, args)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
