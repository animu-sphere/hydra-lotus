# SPDX-License-Identifier: Apache-2.0
"""Optional, read-only whole-device observations around benchmark processes."""

import csv
import io
import json
import math
from pathlib import Path
import subprocess
import threading
import time


FIELDS = ("timestamp", "index", "uuid", "name", "driver_version", "pstate",
          "clocks.gr", "clocks.mem", "utilization.gpu", "memory.used", "memory.total")
NUMERIC = {"clocks.gr": "graphics_clock_mhz", "clocks.mem": "memory_clock_mhz",
           "utilization.gpu": "gpu_utilization_percent", "memory.used": "device_memory_used_mib",
           "memory.total": "device_memory_total_mib"}


def parse_snapshot(output):
    devices = []
    for row in csv.reader(io.StringIO(output)):
        if not row:
            continue
        if len(row) != len(FIELDS):
            raise ValueError("unexpected nvidia-smi column count")
        values = dict(zip(FIELDS, (value.strip() for value in row)))
        if not values["uuid"].startswith("GPU-"):
            raise ValueError("missing nvidia-smi device UUID")
        device = {key: values[key] for key in FIELDS[:6]}
        for key, name in NUMERIC.items():
            raw = values[key]
            value = None if raw in ("N/A", "[N/A]", "[Not Supported]") else float(raw)
            if value is not None and (not math.isfinite(value) or value < 0):
                raise ValueError(f"invalid nvidia-smi {key}")
            device[name] = value
        devices.append(device)
    if not devices or len({device["uuid"] for device in devices}) != len(devices):
        raise ValueError("empty or duplicate nvidia-smi devices")
    return devices


def summarize(samples):
    devices = {}
    for sample in samples:
        for device in sample["devices"]:
            summary = devices.setdefault(device["uuid"], {
                "name": device["name"], "driver_version": device["driver_version"],
                "samples": 0, "pstates": [], "ranges": {}})
            summary["samples"] += 1
            if device["pstate"] not in summary["pstates"]:
                summary["pstates"].append(device["pstate"])
            for field in NUMERIC.values():
                value = device[field]
                bounds = summary["ranges"].setdefault(field, {"min": None, "max": None})
                if value is not None:
                    bounds["min"] = value if bounds["min"] is None else min(bounds["min"], value)
                    bounds["max"] = value if bounds["max"] is None else max(bounds["max"], value)
    return devices


class GpuTelemetry:
    def __init__(self, path, interval_ms):
        self.path = Path(path)
        self.interval_ms = interval_ms
        self.samples = []
        self.errors = []
        self.stop_event = threading.Event()
        self.thread = None

    def sample(self, phase):
        started = time.monotonic()
        try:
            result = subprocess.run(
                ["nvidia-smi", "--query-gpu=" + ",".join(FIELDS), "--format=csv,noheader,nounits"],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=5)
            if result.returncode != 0:
                raise ValueError(f"nvidia-smi exit {result.returncode}: " +
                                 (result.stderr or result.stdout).strip())
            devices = parse_snapshot(result.stdout)
        except (OSError, subprocess.TimeoutExpired, ValueError) as error:
            self.errors.append({"elapsed_ms": (started - self.started) * 1000,
                                "phase": phase, "error": str(error)})
            return False
        self.samples.append({"elapsed_ms": (started - self.started) * 1000,
                             "query_duration_ms": (time.monotonic() - started) * 1000,
                             "phase": phase, "devices": devices})
        return True

    def start(self):
        self.started = time.monotonic()
        self.started_unix = time.time()
        # Bracket the process even when it is too short for a periodic sample.
        if self.sample("before_process"):
            self.thread = threading.Thread(target=self.poll, daemon=True)
            self.thread.start()

    def poll(self):
        while not self.stop_event.wait(self.interval_ms / 1000):
            if not self.sample("process_window"):
                break

    def finish(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join()  # Each in-flight query has a five-second timeout.
        if not self.errors:
            self.sample("after_process")
        status = "partial" if self.errors and self.samples else "unavailable" if self.errors else "available"
        data = {"schema": "lotus-gpu-telemetry-v1", "status": status,
                "source": "nvidia-smi", "started_unix": self.started_unix,
                "requested_interval_ms": self.interval_ms,
                "scope": "All NVIDIA devices; whole process including startup, warmup and teardown. "
                         "Not synchronized to measured frames. Device memory includes other processes; "
                         "sampled maxima are not process peak residency. Query duration adds to the interval. "
                         "Before/after samples bracket the process; process_window queries may finish after exit. "
                         "Polling may perturb CPU/GPU timing; clocks are observed, never changed.",
                "samples": self.samples, "errors": self.errors, "devices": summarize(self.samples)}
        self.path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        return {"status": status, "report": self.path.name}
