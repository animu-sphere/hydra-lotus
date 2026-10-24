#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure Cornell MIS error at independent sample checkpoints, sequentially."""

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import sys

import performance_baseline as baseline

ROOT = Path(__file__).resolve().parents[1]
SAMPLE_INDEX = 1 << 20


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_pfm(path):
    """Read RGB PFM in file row order, including scale and endianness."""
    with Path(path).open("rb") as source:
        def line():
            value = source.readline().strip()
            while value.startswith(b"#"):
                value = source.readline().strip()
            return value
        require(line() == b"PF", "expected RGB PFM")
        width, height = map(int, line().split())
        require(0 < width <= 4096 and 0 < height <= 4096, "invalid PFM dimensions")
        scale = float(line())
        require(math.isfinite(scale) and scale != 0, "invalid PFM scale")
        payload = source.read()
    require(len(payload) == width * height * 12, "invalid PFM payload length")
    values = array("f")
    values.frombytes(payload)
    if (scale < 0) != (sys.byteorder == "little"):
        values.byteswap()
    values = [v * abs(scale) for v in values]
    require(all(math.isfinite(v) for v in values), "non-finite PFM radiance")
    return (width, height), values


def image_error(path, reference):
    shape, values = read_pfm(path)
    require(shape == reference[0], "PFM dimensions differ from reference")
    mse = math.fsum((a - b) ** 2 for a, b in zip(values, reference[1])) / len(values)
    return {"mse": mse, "rmse": math.sqrt(mse)}


def total_ms(series, frames, nullable=False):
    if nullable and series is None:
        return None
    require(isinstance(series, dict), "missing timing series")
    raw = series["raw"]
    require(len(raw) == frames and all(type(v) in (int, float) and
            math.isfinite(v) and v > 0 for v in raw), "invalid timing population")
    return math.fsum(raw)


def reduce_runs(path, reference, frames):
    run = json.loads(path.read_text(encoding="utf-8"))
    require(run["status"] == "pass", "campaign did not pass")
    points = []
    for entry in run["runs"]:
        require(entry["status"] == "pass" and entry["workload"] == "cornell-v1",
                "invalid quality attempt")
        report_path = path.parent / entry["report"]
        report = json.loads(report_path.read_text(encoding="utf-8"))
        data = report["scenes"][0]["measurements"]
        image_path = report_path.parent / data["image"]
        require(hashlib.sha256(image_path.read_bytes()).hexdigest() == entry["image_sha256"],
                "image changed after baseline validation")
        points.append({"run": entry["run"], "samples": frames,
                       **image_error(image_path, reference),
                       "gpu_total_ms": total_ms(data["gpu_frame_ms"], frames, nullable=True),
                       "render_wall_total_ms": total_ms(data["render_wall_ms"], frames),
                       "image_sha256": entry["image_sha256"]})
    return points


def observed_target(curve, target):
    """First checkpoint below target at every remaining observed checkpoint."""
    last_above = max((i for i, p in enumerate(curve) if p["rmse"] > target), default=-1)
    point = curve[last_above + 1] if last_above + 1 < len(curve) else None
    return {"rmse_target": target, "samples": point["samples"] if point else None,
            "gpu_total_ms": point["gpu_total_ms"] if point else None,
            "render_wall_total_ms": point["render_wall_total_ms"] if point else None}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headless", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--clocks", required=True)
    parser.add_argument("--integrator", choices=("reference", "wavefront"), default="reference")
    parser.add_argument("--frames", type=int, nargs="+", default=[1, 16, 64, 256])
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=8)
    parser.add_argument("--rmse-target", type=float, action="append")
    parser.add_argument("--gpu-telemetry", choices=("off", "nvidia-smi"), default="off")
    args = parser.parse_args(argv)
    if (args.frames != sorted(set(args.frames)) or not all(1 <= n <= 4096 for n in args.frames)
            or not 1 <= args.runs <= 16 or not 1 <= args.warmup <= 4096):
        parser.error("use increasing unique frames in 1..4096, runs in 1..16, warmup in 1..4096")
    targets = args.rmse_target or [0.2, 0.1]
    if len(set(targets)) != len(targets) or any(not math.isfinite(t) or t <= 0 for t in targets):
        parser.error("RMSE targets must be finite, positive and unique")
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output directory must be empty to preserve previous measurements")
    reference_path = ROOT / "validation/reference/cornell-box-mean.pfm"
    reference = read_pfm(reference_path)
    executable = args.headless.resolve()
    summary = {"schema": "lotus-quality-curve-v1", "status": "fail",
               "label": args.label, "clock_conditions": args.clocks,
               "integrator": args.integrator, "sample_index": SAMPLE_INDEX,
               "reference": {"path": "validation/reference/cornell-box-mean.pfm",
                             "sha256": hashlib.sha256(reference_path.read_bytes()).hexdigest(),
                             "samples": 1024, "sample_index": 0, "light_sampling": "bsdf"},
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()
                   if executable.is_file() else None,
               "rmse_targets": targets, "checkpoints": [], "curves": []}
    output.mkdir(parents=True, exist_ok=True)
    try:
        for frames in args.frames:
            manifest = baseline.load_manifest(ROOT / "validation/performance-workloads.json")
            manifest["settings"].update(frames=frames, runs=args.runs, warmup=args.warmup)
            manifest["workloads"] = ["cornell-v1"]
            manifest["scope"] = "Cornell independent-sequence MIS quality checkpoint."
            manifest_path = output / f"manifest-{frames}.json"
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            cell = output / f"spp-{frames}"
            checkpoint = {"samples": frames, "run_summary": f"spp-{frames}/run.json",
                          "status": "fail"}
            summary["checkpoints"].append(checkpoint)
            code = baseline.main(["--manifest", str(manifest_path), "--headless", str(executable),
                "--output", str(cell), "--label", args.label, "--clocks", args.clocks,
                "--integrator", args.integrator, "--sample-index", str(SAMPLE_INDEX),
                "--gpu-telemetry", args.gpu_telemetry])
            if code:
                checkpoint["status"] = "skip" if code == 77 else "fail"
                summary["status"] = checkpoint["status"]
                return code
            checkpoint["points"] = reduce_runs(cell / "run.json", reference, frames)
            checkpoint["status"] = "pass"
        for run in range(args.runs):
            curve = [c["points"][run] for c in summary["checkpoints"]]
            summary["curves"].append({"run": run, "points": curve,
                "targets": [observed_target(curve, target) for target in targets]})
        summary["status"] = "pass"
        return 0
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
        summary["error"] = str(error)
        print(error, file=sys.stderr)
        return 1
    finally:
        (output / "quality.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n",
                                            encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
