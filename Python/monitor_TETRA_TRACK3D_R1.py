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

from toa_tracker_live import CircularAlphaBetaTracker


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


def physical_azimuth(native):
    return (360.0 - native) % 360.0 if native is not None else None


def physical_direction(native):
    names = {
        'UP': 'NORTE / UP', 'UP_RIGHT': 'NORESTE',
        'RIGHT': 'ESTE / RIGHT', 'DOWN_RIGHT': 'SURESTE',
        'DOWN': 'SUR / DOWN', 'DOWN_LEFT': 'SUROESTE',
        'LEFT': 'OESTE / LEFT', 'UP_LEFT': 'NOROESTE',
    }
    return names.get(native, native or 'NONE')


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
            "tetra_elev": None,
            "tetra_norm": None,
            "tetra_rms": None,
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

    def feed_tetra(self, line):
        seq = search_int(line, "seq")
        if seq is None:
            return
        e = self.entry(seq)
        if search_int(line, "fit") == 1:
            e["tetra_elev"] = search_float(line, "elev")
            e["tetra_norm"] = search_float(line, "norm")
            e["tetra_rms"] = search_float(line, "rms")

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

        if e.get("tetra_elev") is not None:
            return "AZIMUTH", e.get("tetra_norm"), e.get("tetra_elev")
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
            "tetra_elev": e.get("tetra_elev"),
            "tetra_norm": e.get("tetra_norm"),
            "tetra_rms": e.get("tetra_rms"),
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
        "tracker_state",
        "tracker_valid",
        "tracker_az",
        "tracker_dir",
        "tracker_omega_dps",
        "tracker_source",
        "tracker_residual_deg",
        "tracker_gate_deg",
        "tracker_misses",
        "tracker_measurement_used",
        "tracker_measurement_rejected",
        "track3d_state", "track3d_valid", "track3d_used", "track3d_rejected",
        "track3d_az", "track3d_az_phys", "track3d_elev", "track3d_innovation",
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
            "tracker_state": c.get("tracker_state"),
            "tracker_valid": c.get("tracker_valid"),
            "tracker_az": c.get("tracker_az"),
            "tracker_dir": c.get("tracker_dir"),
            "tracker_omega_dps": c.get("tracker_omega_dps"),
            "tracker_source": c.get("tracker_source"),
            "tracker_residual_deg": c.get("tracker_residual_deg"),
            "tracker_gate_deg": c.get("tracker_gate_deg"),
            "tracker_misses": c.get("tracker_misses"),
            "tracker_measurement_used": c.get("tracker_measurement_used"),
            "tracker_measurement_rejected": c.get("tracker_measurement_rejected"),
            "track3d_state": c.get("track3d_state"),
            "track3d_valid": c.get("track3d_valid"),
            "track3d_used": c.get("track3d_used"),
            "track3d_rejected": c.get("track3d_rejected"),
            "track3d_az": c.get("track3d_az"),
            "track3d_az_phys": c.get("track3d_az_phys"),
            "track3d_elev": c.get("track3d_elev"),
            "track3d_innovation": c.get("track3d_innovation"),
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
        self.tracker = CircularAlphaBetaTracker(
            history_len=args.history,
            gate_base_deg=args.tracker_gate_base_deg,
            gate_max_deg=args.tracker_gate_max_deg,
            max_omega_dps=args.tracker_max_omega_dps,
            max_coast=args.tracker_max_coast,
            reacquire_count=args.tracker_reacquire_count,
            reacquire_consistency_deg=args.tracker_reacquire_consistency_deg,
        )
        self.csv = CsvWriter(args.csv)
        self.pending_csv = None
        self.track3d_history = deque(maxlen=args.history)
        self.track3d_latest = None
        log_path = args.log
        if log_path is None and args.csv:
            csv_path = Path(args.csv)
            log_path = str(csv_path.with_name(csv_path.stem + "_uart.log"))
        self.uart_log = UartLogWriter(log_path)
        if log_path:
            print(f"UART log: {log_path}")
        self.worker = SerialWorker(args.port, args.baud, self.messages)
        self.worker.start()

        root.title("Khamex TOA-1F.1 TRACKER LIVE")
        root.geometry("1240x900")
        root.minsize(1080, 760)
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
        self.tracker_state_var = tk.StringVar(value="EMPTY")
        self.tracker_az_var = tk.StringVar(value="--")
        self.tracker_dir_var = tk.StringVar(value="NONE")
        self.tracker_omega_var = tk.StringVar(value="--")
        self.tracker_source_var = tk.StringVar(value="NONE")
        self.tracker_residual_var = tk.StringVar(value="--")
        self.tracker_misses_var = tk.StringVar(value="0")
        self.track3d_state_var = tk.StringVar(value="SIN DATOS")
        self.track3d_az_var = tk.StringVar(value="--")
        self.track3d_elev_var = tk.StringVar(value="--")
        self.track3d_innov_var = tk.StringVar(value="--")
        self.track3d_use_var = tk.StringVar(value="--")

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
            ("Direccion fisica RAW", self.dir_var),
            ("Azimut fisico RAW", self.az_var),
            ("Elevation EXP", self.elev_var),
            ("rho EXP", self.rho_var),
            ("Closure", self.closure_var),
            ("Confidence", self.conf_var),
            ("Track eligible", self.track_var),
            ("Ambiguous pairs", self.amb_var),
            ("Peak gap min", self.gap_var),
            ("Tracker state", self.tracker_state_var),
            ("Azimut fisico tracker", self.tracker_az_var),
            ("Direccion fisica tracker", self.tracker_dir_var),
            ("Angular velocity", self.tracker_omega_var),
            ("Tracker source", self.tracker_source_var),
            ("Tracker residual", self.tracker_residual_var),
            ("Tracker misses", self.tracker_misses_var),
            ("TRACK3D estado", self.track3d_state_var),
            ("TRACK3D azimut fisico", self.track3d_az_var),
            ("TRACK3D elevacion", self.track3d_elev_var),
            ("TRACK3D innovacion", self.track3d_innov_var),
            ("TRACK3D uso/rechazo", self.track3d_use_var),
        ]
        for r, (name, var) in enumerate(rows):
            ttk.Label(grid, text=name).grid(row=r, column=0, sticky="w", padx=(0, 12), pady=3)
            ttk.Label(grid, textvariable=var, font=("Consolas", 11, "bold")).grid(row=r, column=1, sticky="e", pady=3)
        grid.columnconfigure(1, weight=1)

        note = (
            "Elevacion TETRA es experimental y NO alimenta este tracker. "
            "Azul/X = mediciones RAW E3.2. Naranja = TRACK azimuthal 2D. "
            "HIGH/MEDIUM actualizan el track; LOW/INVALID/NOT_EVALUATED no lo actualizan. "
            f"El tracker puede predecir hasta {self.args.tracker_max_coast} s durante gaps. "
            "Las predicciones se dibujan discontinuas; una reacquisicion grande rompe la linea."
        )
        ttk.Label(left, text=note, wraplength=330, foreground="#555555").pack(fill="x", pady=8)

        right = ttk.Frame(body, padding=8)
        right.grid(row=0, column=1, sticky="nsew")
        right.rowconfigure(1, weight=1)
        right.columnconfigure(0, weight=1)
        ttk.Label(right, text="RAW 3D + F1.1 2D + TRACK3D-R1", font=("Segoe UI", 15, "bold")).grid(row=0, column=0, sticky="w")
        self.radar = tk.Canvas(right, bg="white", highlightthickness=1, highlightbackground="#bbbbbb")
        self.radar.grid(row=1, column=0, sticky="nsew", pady=(8, 0))
        self.radar.bind("<Configure>", lambda event: self.draw_radar())

    def reset_track(self):
        self.model.history.clear()
        self.tracker.reset()
        self.track3d_history.clear()
        self.track3d_latest = None
        self.draw_radar()

    def request_stop(self):
        self.worker.stop_capture()

    def on_close(self):
        self.worker.stop_capture()
        self.worker.close()
        self.flush_pending_csv()
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
                    self.flush_pending_csv()
                    self.status_var.set("STOPPED")
        except queue.Empty:
            pass
        self.root.after(50, self.poll)

    def handle_line(self, line):
        print(line)
        self.uart_log.write(line)
        if line.startswith("[TRACK3D]"):
            self.feed_track3d(line)
        elif line.startswith("[LIVE_DET]"):
            self.model.feed_live_det(line)
        elif line.startswith("[TOA]"):
            self.model.feed_toa(line)
        elif line.startswith("[TOA_TETRA]"):
            self.model.feed_tetra(line)
        elif line.startswith("[TOA_CONF]"):
            self.model.feed_confidence(line)
        elif line.startswith("[TARGET]"):
            self.flush_pending_csv()
            e = self.model.feed_target(line)
            if e is not None:
                c = self.model.current
                tr = self.tracker.update(
                    c["seq"],
                    c.get("raw_az"),
                    c.get("confidence", "NONE"),
                    c.get("track_eligible", 0),
                    c.get("spatial", "UNKNOWN"),
                    c.get("alert", 0),
                )
                c.update(
                    tracker_state=tr["state"],
                    tracker_valid=tr["valid"],
                    tracker_az=tr["azimuth"],
                    tracker_dir=tr["direction"],
                    tracker_omega_dps=tr["omega_dps"],
                    tracker_source=tr["source"],
                    tracker_residual_deg=tr["residual_deg"],
                    tracker_gate_deg=tr["gate_deg"],
                    tracker_misses=tr["misses"],
                    tracker_measurement_used=tr["measurement_used"],
                    tracker_measurement_rejected=tr["measurement_rejected"],
                )
                self.refresh()
                self.pending_csv = dict(c)
                rho = "NA" if c["rho"] is None else f"{c['rho']:.3f}"
                elev = "NA" if c["elev_exp"] is None else f"{c['elev_exp']:.1f}"
                spatial_line = (
                    f"[SPATIAL_EXP] seq={c['seq']} mode={c['spatial']} "
                    f"rho={rho} elev_exp={elev} upper_hemisphere=1"
                )
                print(spatial_line)
                self.uart_log.write(spatial_line)
                tr_az = "NA" if tr["azimuth"] is None else f"{tr['azimuth']:.2f}"
                tr_res = "NA" if tr["residual_deg"] is None else f"{tr['residual_deg']:.2f}"
                tr_gate = "NA" if tr["gate_deg"] is None else f"{tr['gate_deg']:.2f}"
                track_line = (
                    f"[TRACK] seq={c['seq']} state={tr['state']} valid={tr['valid']} "
                    f"az={tr_az} dir={tr['direction']} omega={tr['omega_dps']:.2f} "
                    f"source={tr['source']} residual={tr_res} gate={tr_gate} "
                    f"misses={tr['misses']} used={tr['measurement_used']} rejected={tr['measurement_rejected']}"
                )
                print(track_line)
                self.uart_log.write(track_line)

    def flush_pending_csv(self):
        if self.pending_csv is not None:
            self.csv.write(self.pending_csv)
            self.pending_csv = None

    def feed_track3d(self, line):
        # La telemetria llega despues de TARGET. Asociar por seq, sin
        # mezclar datos de ventanas diferentes ni inventar mediciones.
        seq = search_int(line, "seq")
        if seq is None:
            return
        data = {
            "seq": seq,
            "track3d_state": search_word(line, "state") or "UNKNOWN",
            "track3d_valid": search_int(line, "valid"),
            "track3d_used": search_int(line, "used"),
            "track3d_rejected": search_int(line, "rejected"),
            "track3d_az": search_float(line, "az"),
            "track3d_az_phys": search_float(line, "az_phys"),
            "track3d_elev": search_float(line, "elev"),
            "track3d_innovation": search_float(line, "innovation"),
        }
        if data["track3d_az_phys"] is None and data["track3d_az"] is not None:
            data["track3d_az_phys"] = physical_azimuth(data["track3d_az"])
        self.track3d_latest = data
        self.track3d_history.append(data)
        if self.pending_csv is not None and self.pending_csv.get("seq") == seq:
            self.pending_csv.update(data)
            self.flush_pending_csv()
        if self.model.current.get("seq") == seq:
            self.model.current.update(data)
            self.refresh()

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
        self.dir_var.set(physical_direction(c.get("raw_dir")))

        raw_az = c.get("raw_az")
        if c.get("spatial") == "ZENITH_CANDIDATE":
            self.az_var.set("N/A (ZENITH EXP)")
        else:
            self.az_var.set("--" if raw_az is None else f"{physical_azimuth(raw_az):.2f} deg")

        elev = c.get("tetra_elev")
        self.elev_var.set("--" if elev is None else f"{elev:.1f} deg TETRA")
        rho = c.get("tetra_norm")
        self.rho_var.set("--" if rho is None else f"{rho:.3f}")
        closure = c.get("closure")
        self.closure_var.set("--" if closure is None else f"{closure:.2f} samp")
        self.conf_var.set(c.get("confidence") or "NONE")
        self.track_var.set(str(c.get("track_eligible", 0)))
        amb = c.get("ambiguous_pairs")
        self.amb_var.set("--" if amb is None else str(amb))
        gap = c.get("peak_gap_min")
        self.gap_var.set("--" if gap is None else f"{gap:.3f}")
        self.tracker_state_var.set(c.get("tracker_state") or "EMPTY")
        tr_az = c.get("tracker_az")
        self.tracker_az_var.set("--" if tr_az is None else f"{physical_azimuth(tr_az):.2f} deg")
        self.tracker_dir_var.set(physical_direction(c.get("tracker_dir")))
        tr_omega = c.get("tracker_omega_dps")
        self.tracker_omega_var.set("--" if tr_omega is None else f"{-tr_omega:+.2f} deg/s")
        self.tracker_source_var.set(c.get("tracker_source") or "NONE")
        tr_res = c.get("tracker_residual_deg")
        self.tracker_residual_var.set("--" if tr_res is None else f"{-tr_res:+.2f} deg")
        self.tracker_misses_var.set(str(c.get("tracker_misses", 0)))
        t = self.track3d_latest
        if t is not None and t.get("seq") == c.get("seq"):
            self.track3d_state_var.set(t["track3d_state"])
            az3 = t.get("track3d_az_phys")
            el3 = t.get("track3d_elev")
            iv3 = t.get("track3d_innovation")
            self.track3d_az_var.set("--" if az3 is None else f"{az3:.2f} deg")
            self.track3d_elev_var.set("--" if el3 is None else f"{el3:.2f} deg")
            self.track3d_innov_var.set("--" if iv3 is None else f"{iv3:.2f} deg")
            self.track3d_use_var.set(f"{t.get('track3d_used')}/{t.get('track3d_rejected')}")
        else:
            self.track3d_state_var.set("ESPERANDO")
            self.track3d_az_var.set("--")
            self.track3d_elev_var.set("--")
            self.track3d_innov_var.set("--")
            self.track3d_use_var.set("--")
        self.draw_radar()

    def _vector_3d(self, h, v, model):
        if h is None or v is None:
            return None
        if model == "A (original)":
            ux, uy = -h / self.args.h_scale, v / self.args.v_scale
        else:
            ux, uy = -h / (44100.0 * 0.09 / 343.0), v / (44100.0 * 0.12 / 343.0)
        rho = math.hypot(ux, uy)
        if model == "C (experimental)":
            rho_view = clamp((rho - 0.42096) / 0.63686, 0.0, 1.0)
        else:
            rho_view = clamp(rho, 0.0, 1.0)
        if rho > 1.05 and model != "C (experimental)":
            return (ux / rho, uy / rho, 0.0, rho, None)
        az = math.atan2(ux, uy)
        return (rho_view * math.sin(az), rho_view * math.cos(az),
                math.sqrt(max(0.0, 1.0-rho_view*rho_view)), rho,
                math.degrees(math.acos(rho_view)))

    def _view_project(self, x, y, z):
        yaw = math.radians(self.view_yaw)
        pitch = math.radians(self.view_pitch)
        xr = math.cos(yaw)*x - math.sin(yaw)*y
        yr = math.sin(yaw)*x + math.cos(yaw)*y
        return xr, -yr*math.sin(pitch)-z*math.cos(pitch)

    def _camera_drag_start(self, event):
        self.drag_origin = (event.x, event.y)

    def _camera_drag(self, event):
        if self.drag_origin is None:
            return
        dx, dy = event.x-self.drag_origin[0], event.y-self.drag_origin[1]
        self.drag_origin = (event.x, event.y)
        self.view_yaw = (self.view_yaw+dx*0.65)%360
        self.view_pitch = clamp(self.view_pitch+dy*0.45, 5.0, 90.0)
        self.draw_radar()

    def _camera_zoom(self, event):
        delta = event.delta if hasattr(event, 'delta') else 0
        if delta == 0 and hasattr(event, 'num'):
            delta = 120 if event.num == 4 else -120
        self.view_zoom = clamp(self.view_zoom*(1.12 if delta>0 else 1/1.12), 0.45, 2.8)
        self.draw_radar()

    def _set_view(self, yaw, pitch):
        self.view_yaw, self.view_pitch = yaw, pitch
        self.draw_radar()

    def _change_model(self, event=None):
        self.draw_radar()

    @staticmethod
    def _fade_color(hex_color, strength):
        strength = clamp(strength, 0.0, 1.0)
        rgb = [int(hex_color[i:i + 2], 16) for i in (1, 3, 5)]
        return '#%02x%02x%02x' % tuple(round(255 - (255 - c) * strength) for c in rgb)

    def draw_radar(self):
        cv = self.radar
        cv.delete('all')
        w, h = max(100,cv.winfo_width()), max(100,cv.winfo_height())
        cx, cy = w/2, h*0.58
        scale = min(w,h)*0.36*self.view_zoom
        def p(x,y,z):
            a,b=self._view_project(x,y,z)
            return cx+scale*a,cy+scale*b
        def segment(a,b,**kw):
            cv.create_line(*p(*a),*p(*b),**kw)
        cv.create_text(12,12,text='RADAR TETRA 3D  |  TOP: N arriba, O izq., E der.  |  arrastra: girar  |  rueda: zoom',anchor='nw',fill='#555555')
        cv.create_text(12,51,text='AZIMUT FISICO: NORTE 0 | OESTE 90 | SUR 180 | ESTE 270 (antihorario)',anchor='nw',fill='#475569',font=('Segoe UI',9))
        if self.show_grid.get():
            # Upper hemisphere graticule: concentric elevation parallels.
            for elev_deg in (0, 15, 30, 45, 60, 75):
                elev_rad = math.radians(elev_deg)
                ring_radius = math.cos(elev_rad)
                ring_z = math.sin(elev_rad)
                pts = []
                for i in range(97):
                    a = 2 * math.pi * i / 96
                    pts.extend(p(-ring_radius * math.sin(a),
                                 ring_radius * math.cos(a), ring_z))
                cv.create_line(*pts, fill='#dbe6ed' if elev_deg else '#bfced8', width=1)
            # Meridians show the spherical elevation grid (not measured 3D locations).
            for az_deg in range(0, 360, 45):
                az_rad = math.radians(az_deg)
                pts = []
                for elev_deg in range(0, 91, 3):
                    elev_rad = math.radians(elev_deg)
                    radius = math.cos(elev_rad)
                    pts.extend(p(-radius * math.sin(az_rad),
                                 radius * math.cos(az_rad), math.sin(elev_rad)))
                cv.create_line(*pts, fill='#e0e8ef', width=1, dash=(3, 4))
            # Labels on the vertical great-circle used for elevation guidance.
            # 0°..90° are shown from UP to ZENITH on the right side;
            # 105°..165° continue from ZENITH toward DOWN on the left side.
            for elev_deg in range(0, 91, 15):
                er = math.radians(elev_deg)
                lx, ly = p(0.0, math.cos(er), math.sin(er))
                cv.create_text(lx + 10, ly - 7, text=f'{elev_deg}°',
                               anchor='sw', fill='#72899a', font=('Segoe UI', 8))
            for elev_deg in range(105, 180, 15):
                er = math.radians(elev_deg)
                lx, ly = p(0.0, math.cos(er), math.sin(er))
                cv.create_text(lx - 10, ly - 7, text=f'{elev_deg}°',
                               anchor='se', fill='#72899a', font=('Segoe UI', 8))
        for r in (0.25,0.5,0.75,1.0):
            pts=[]
            for i in range(73):
                a=2*math.pi*i/72
                pts.extend(p(r*math.sin(a),r*math.cos(a),0))
            cv.create_line(*pts,fill='#e5e7eb',width=1)
        segment((-1.12,0,0),(1.12,0,0),fill='#b7c1cc',width=1)
        segment((0,-1.12,0),(0,1.12,0),fill='#b7c1cc',width=1)
        segment((0,0,0),(0,0,1.18),fill='#64748b',width=2,dash=(5,4))
        for v,label in [((0,1.25,0),'NORTE / UP 0°'),((1.25,0,0),'ESTE / RIGHT 270°'),((0,-1.25,0),'SUR / DOWN 180°'),((-1.25,0,0),'OESTE / LEFT 90°'),((0,0,1.26),'ZENITH')]:
            cv.create_text(*p(*v),text=label,fill='#555555',font=('Segoe UI',9))
        # Eight fixed azimuth references around the XY horizon, projected with the camera.
        for angle, label in [(45,'NO 45°'),(135,'SO 135°'),(225,'SE 225°'),(315,'NE 315°')]:
            rad = math.radians(angle)
            ax, ay = p(-1.17*math.sin(rad), 1.17*math.cos(rad), 0)
            cv.create_text(ax, ay, text=label, fill='#5e7791', font=('Segoe UI',8))
        # Pantalla: X positivo = ESTE, Y positivo = NORTE, Z positivo = arriba.
        # Coordenadas en escala de 10 cm; coinciden con MIC2 SUR, MIC5 OESTE, MIC6 ESTE.
        vertices=[(0,-.866,0),(.75,.433,0),(-.75,.433,0),(0,0,1.2)]
        for i,j in ((0,1),(1,2),(2,0),(0,3),(1,3),(2,3)):
            segment(vertices[i],vertices[j],fill='#008da9',width=2)
        for xyz,label in zip(vertices,('MIC2 / CH0 SUR','MIC6 / CH1 ESTE',
                                        'MIC5 / CH2 OESTE','MIC7 / CH3 +12cm')):
            px,py=p(*xyz)
            cv.create_oval(px-4,py-4,px+4,py+4,fill='#0891b2',outline='white')
            cv.create_text(px+6,py-7,text=label,anchor='sw',fill='#007a91',font=('Segoe UI',8))
        model=self.view_model.get()
        prior=None
        total = len(self.view_history)
        for index, item in enumerate(self.view_history):
            age_strength = 0.18 + 0.82 * ((index + 1) / max(1, total)) ** 1.5
            point_color = self._fade_color('#2563eb', age_strength)
            link_color = self._fade_color('#80a7df', age_strength)
            low_color = self._fade_color('#a33a3a', age_strength)
            if item is None or not item['valid']:
                prior=None
                continue
            if item.get('tetra_elev') is not None:
                az_ph = math.radians(physical_azimuth(item['az']))
                el = math.radians(item['tetra_elev'])
                vec = (-math.cos(el)*math.sin(az_ph), math.cos(el)*math.cos(az_ph), math.sin(el), item.get('tetra_norm'), item['tetra_elev'])
            else:
                vec=None
            if vec is None:
                prior=None
                continue
            xyz=vec[:3]
            xy=p(*xyz)
            if item['eligible']:
                if prior is not None:
                    dot=sum(prior[i]*xyz[i] for i in range(3))
                    step=math.degrees(math.acos(clamp(dot,-1.0,1.0)))
                    if step<=self.args.max_link_step_deg:
                        segment(prior,xyz,fill=link_color,width=2)
                cv.create_oval(xy[0]-2,xy[1]-2,xy[0]+2,xy[1]+2,fill=point_color,outline='')
                prior=xyz
            else:
                cv.create_line(xy[0]-4,xy[1]-4,xy[0]+4,xy[1]+4,fill=low_color,width=2)
                cv.create_line(xy[0]-4,xy[1]+4,xy[0]+4,xy[1]-4,fill=low_color,width=2)
                prior=None
        current=self.model.current
        if current.get('loc_valid'):
            if current.get('tetra_elev') is not None and current.get('raw_az') is not None:
                az_ph=math.radians(physical_azimuth(current['raw_az']))
                el=math.radians(current['tetra_elev'])
                vec=(-math.cos(el)*math.sin(az_ph),math.cos(el)*math.cos(az_ph),math.sin(el),current.get('tetra_norm'),current['tetra_elev'])
            else:
                vec=None
            if vec is not None:
                xyz=vec[:3]
                segment((0,0,0),xyz,fill='#2563eb',width=4,arrow=tk.LAST)
                if vec[4] is None:
                    label='RAW 3D TETRA: SIN ELEVACION'
                else:
                    label=f"RAW 3D TETRA | norm={vec[3]:.3f} | elev={vec[4]:.1f}°"
                cv.create_text(12,33,text=label,anchor='nw',fill='#2563eb',font=('Consolas',10,'bold'))
        else:
            cv.create_text(12,33,text='RAW 3D: SIN MEDICION VALIDA',anchor='nw',fill='#a33a3a',font=('Consolas',10,'bold'))
        # F1.1 stays purely azimuthal and is shown in plane XY, never as 3D tracking.
        tracker_total = len(self.tracker.history)
        for tracker_index, item in enumerate(self.tracker.history):
            if item is None or item.get('azimuth') is None:
                continue
            fade = 0.18 + 0.82 * ((tracker_index + 1) / max(1, tracker_total)) ** 1.5
            a=math.radians(item['azimuth'])
            xyz=(.95*math.sin(a),.95*math.cos(a),0)
            px,py=p(*xyz)
            cv.create_oval(px-2,py-2,px+2,py+2,fill=self._fade_color('#d97706', fade),outline='')
        az=current.get('tracker_az')
        if az is not None and current.get('tracker_valid'):
            a=math.radians(az)
            segment((0,0,0),(.95*math.sin(a),.95*math.cos(a),0),fill='#d97706',width=2,dash=(5,3))
        # TRACK3D-R1 verde: cada muestra representa una direccion 3D real,
        # no una distancia. Durante COASTING, mostrar punto sin enlazar.
        previous = None
        t_total = len(self.track3d_history)
        for idx, item in enumerate(self.track3d_history):
            az = item.get('track3d_az_phys')
            el = item.get('track3d_elev')
            if item.get('track3d_valid') != 1 or az is None or el is None:
                previous = None
                continue
            azr, elr = math.radians(az), math.radians(el)
            xyz = (-math.cos(elr)*math.sin(azr),
                   math.cos(elr)*math.cos(azr), math.sin(elr))
            x2, y2 = p(*xyz)
            fade = 0.25 + 0.75 * ((idx + 1) / max(1, t_total)) ** 1.5
            color = self._fade_color('#059669', fade)
            if item.get('track3d_used') == 1:
                if previous is not None:
                    dot = sum(previous[j]*xyz[j] for j in range(3))
                    step = math.degrees(math.acos(clamp(dot, -1, 1)))
                    if step <= self.args.max_link_step_deg:
                        segment(previous, xyz, fill=color, width=2)
                previous = xyz
            else:
                previous = None
            cv.create_oval(x2-3,y2-3,x2+3,y2+3,fill=color,outline='')
        latest = self.track3d_latest
        if (latest is not None and latest.get('seq') == current.get('seq')
                and latest.get('track3d_valid') == 1
                and latest.get('track3d_az_phys') is not None
                and latest.get('track3d_elev') is not None):
            azr = math.radians(latest['track3d_az_phys'])
            elr = math.radians(latest['track3d_elev'])
            xyz = (-math.cos(elr)*math.sin(azr), math.cos(elr)*math.cos(azr), math.sin(elr))
            segment((0,0,0),xyz,fill='#059669',width=3,arrow=tk.LAST)
        cv.create_text(12,h-35,text='Azul: RAW 3D  |  Naranja: F1.1 2D  |  Verde: TRACK3D-R1  |  Rojo: LOW',anchor='nw',fill='#555555',font=('Segoe UI',9))
        cv.create_text(12,h-18,text='Direccion 3D experimental, no distancia. Z positivo = arriba; validar con TOA valido.',anchor='nw',fill='#777777',font=('Segoe UI',9))


class App3D(App):
    def __init__(self, root, args):
        self.view_yaw = 0.0
        self.view_pitch = 28.0
        self.view_zoom = 1.0
        self.drag_origin = None
        self.view_history = deque(maxlen=args.history)
        self.view_model = tk.StringVar(value='TETRA (medido)')
        self.show_grid = tk.BooleanVar(value=True)
        super().__init__(root, args)
        root.title('Khamex TOA-1F.1 LIVE - RADAR 3D EXP')

    def build_ui(self):
        super().build_ui()
        self.radar.bind('<ButtonPress-1>', self._camera_drag_start)
        self.radar.bind('<B1-Motion>', self._camera_drag)
        self.radar.bind('<ButtonRelease-1>', lambda e: setattr(self,'drag_origin',None))
        self.radar.bind('<MouseWheel>', self._camera_zoom)
        self.radar.bind('<Button-4>', self._camera_zoom)
        self.radar.bind('<Button-5>', self._camera_zoom)
        bar=ttk.Frame(self.radar.master)
        bar.grid(row=2,column=0,sticky='ew',pady=(5,0))
        ttk.Label(bar,text='Elevacion:').pack(side='left')
        cmb=ttk.Combobox(bar,textvariable=self.view_model,values=['TETRA (medido)'],state='readonly',width=18)
        cmb.pack(side='left',padx=5)
        cmb.bind('<<ComboboxSelected>>',self._change_model)
        for label,yaw,pitch in [('ISO',25,55),('TOP',0,90),('LATERAL',0,12),('FRONTAL',0,28)]:
            ttk.Button(bar,text=label,command=lambda y=yaw,p=pitch:self._set_view(y,p)).pack(side='left',padx=2)
        ttk.Checkbutton(bar, text='Cuadricula 3D', variable=self.show_grid, command=self.draw_radar).pack(side='left', padx=(12,0))
        self.draw_radar()

    def reset_track(self):
        self.view_history.clear()
        super().reset_track()

    def handle_line(self,line):
        super().handle_line(line)
        if not line.startswith('[TARGET]'):
            return
        c=self.model.current
        if search_int(line,'seq') != c.get('seq'):
            return
        if c.get('toa_status') == 'NOT_EVALUATED' or not c.get('loc_valid'):
            self.view_history.append(None)
        else:
            self.view_history.append({'valid':True,'h':c.get('h'),'v':c.get('v'),
                                      'az':c.get('raw_az'),'tetra_elev':c.get('tetra_elev'),
                                      'tetra_norm':c.get('tetra_norm'),
                                      'eligible':bool(c.get('track_eligible'))})
        self.draw_radar()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=1000000)
    parser.add_argument("--h-scale", type=float, default=8.59)
    parser.add_argument("--v-scale", type=float, default=15.13)
    parser.add_argument("--zenith-rho", type=float, default=0.20)
    parser.add_argument("--history", type=int, default=180)
    parser.add_argument("--max-link-step-deg", type=float, default=20.0)
    parser.add_argument("--tracker-gate-base-deg", type=float, default=22.0)
    parser.add_argument("--tracker-gate-max-deg", type=float, default=45.0)
    parser.add_argument("--tracker-max-omega-dps", type=float, default=60.0)
    parser.add_argument("--tracker-max-coast", type=int, default=3)
    parser.add_argument("--tracker-reacquire-count", type=int, default=2)
    parser.add_argument("--tracker-reacquire-consistency-deg", type=float, default=25.0)
    parser.add_argument("--csv", default=None)
    parser.add_argument("--log", default=None)
    args = parser.parse_args()

    if args.h_scale <= 0.0 or args.v_scale <= 0.0:
        parser.error("h-scale y v-scale deben ser > 0")
    if not 0.0 < args.zenith_rho < 1.0:
        parser.error("zenith-rho debe estar entre 0 y 1")
    if args.max_link_step_deg <= 0.0 or args.max_link_step_deg > 180.0:
        parser.error("max-link-step-deg debe estar entre 0 y 180")
    if args.tracker_gate_base_deg <= 0.0 or args.tracker_gate_max_deg < args.tracker_gate_base_deg:
        parser.error("tracker gate invalido")
    if args.tracker_max_omega_dps <= 0.0:
        parser.error("tracker-max-omega-dps debe ser > 0")
    if args.tracker_max_coast < 0:
        parser.error("tracker-max-coast debe ser >= 0")
    if args.tracker_reacquire_count < 2:
        parser.error("tracker-reacquire-count debe ser >= 2")
    if args.tracker_reacquire_consistency_deg <= 0.0 or args.tracker_reacquire_consistency_deg > 180.0:
        parser.error("tracker-reacquire-consistency-deg debe estar entre 0 y 180")

    root = tk.Tk()
    App3D(root, args)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
