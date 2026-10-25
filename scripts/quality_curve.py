#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure static-workload MIS error at disjoint sample checkpoints, sequentially."""

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import shutil
import statistics
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


def reference_run(path, workload, size):
    """Validate one finite Reference PT baseline attempt and retain its provenance."""
    run = json.loads(path.read_text(encoding="utf-8"))
    require(run["schema"] == "lotus-performance-run-v1" and run["status"] == "pass"
            and run["integrator"] == "reference" and len(run["runs"]) == 1,
            "reference requires one successful Reference PT baseline attempt")
    entry = run["runs"][0]
    require(entry["status"] == "pass" and entry["exit_code"] == 0
            and entry["adapter"] == "headless" and entry["workload"] == workload,
            "reference attempt does not match the selected workload")

    def sibling(name):
        require(isinstance(name, str) and name == Path(name).name
                and name not in ("", ".", ".."), "reference evidence must be sibling files")
        return path.parent / name

    settings = run["manifest"]["settings"]
    require(settings["size"] == size and settings["max_bounces"] == 64
            and settings["light_sampling"] == "mis"
            and type(settings["frames"]) is int and 1 <= settings["frames"] <= 4096
            and type(settings["warmup"]) is int and 1 <= settings["warmup"] <= 4096
            and type(settings["sample_index"]) is int and settings["sample_index"] >= 0
            and settings["sample_index"] + settings["frames"] <= 1 << 21,
            "reference settings differ from the quality procedure")
    report_path = sibling(entry["report"])
    result = baseline.check_report(report_path, "headless", workload, "reference", settings)
    require(result["image_sha256"] == entry["image_sha256"],
            "reference image changed after baseline validation")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    procedure = report["procedure"]
    require(procedure["max_bounces"] == 64 and procedure["samples_per_frame"] == 1,
            "reference transport settings differ from the quality procedure")
    image_path = sibling(report["scenes"][0]["measurements"]["image"])
    image = read_pfm(image_path)
    require(image[0] == (size, size), "reference PFM dimensions differ from procedure")
    log = sibling(entry["log"])
    files = [path, report_path, image_path, log]
    require(len({p.name for p in files}) == len(files), "reference evidence filenames collide")
    metadata = {"path": "reference/" + image_path.name, "sha256": result["image_sha256"],
                "samples": settings["frames"], "sample_index": settings["sample_index"],
                "light_sampling": "mis", "integrator": "reference", "workload": workload,
                "source_run": str(path), "run_summary": "reference/" + path.name,
                "evidence_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                    for p in files}}
    return image, metadata, files


def reduce_runs(path, reference, frames, workload="cornell-v1"):
    run = json.loads(path.read_text(encoding="utf-8"))
    require(run["status"] == "pass", "campaign did not pass")
    points = []
    for entry in run["runs"]:
        require(entry["status"] == "pass" and entry["workload"] == workload,
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


def sample_populations(curves, frames):
    """Count each disjoint sample sequence once, excluding timing repetitions."""
    independent = [curve for curve in curves if curve["run"] == 0]
    populations = []
    for i, samples in enumerate(frames):
        errors = [curve["points"][i]["rmse"] for curve in independent]
        populations.append({"samples": samples, "independent_sequences": len(errors),
            "rmse": {"min": min(errors), "median": statistics.median(errors),
                     "max": max(errors), "mean": statistics.mean(errors),
                     "sample_stddev": statistics.stdev(errors) if len(errors) > 1 else None}})
    return populations


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headless", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--clocks", required=True)
    parser.add_argument("--integrator", choices=("reference", "wavefront"), default="reference")
    parser.add_argument("--workload", default="cornell-v1", help="one shared manifest workload")
    parser.add_argument("--reference-run", type=Path,
                        help="single-attempt Reference PT baseline run.json; required outside Cornell")
    parser.add_argument("--frames", type=int, nargs="+", default=[1, 16, 64, 256])
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=8)
    parser.add_argument("--sample-index", type=int, action="append",
                        help="repeat for disjoint measured sequences; default: 1048576")
    parser.add_argument("--rmse-target", type=float, action="append")
    parser.add_argument("--gpu-telemetry", choices=("off", "nvidia-smi"), default="off")
    args = parser.parse_args(argv)
    if (args.frames != sorted(set(args.frames)) or not all(1 <= n <= 4096 for n in args.frames)
            or not 1 <= args.runs <= 16 or not 1 <= args.warmup <= 4096):
        parser.error("use increasing unique frames in 1..4096, runs in 1..16, warmup in 1..4096")
    targets = args.rmse_target or [0.2, 0.1]
    if len(set(targets)) != len(targets) or any(not math.isfinite(t) or t <= 0 for t in targets):
        parser.error("RMSE targets must be finite, positive and unique")
    indices = args.sample_index if args.sample_index is not None else [SAMPLE_INDEX]
    manifest = baseline.load_manifest(ROOT / "validation/performance-workloads.json")
    if args.workload not in manifest["workloads"]:
        parser.error("workload is absent from the shared manifest")
    reference_files = []
    if args.reference_run:
        try:
            reference, reference_metadata, reference_files = reference_run(
                args.reference_run.resolve(), args.workload, manifest["settings"]["size"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            parser.error(str(error))
        if reference_metadata["samples"] <= max(args.frames):
            parser.error("reference must contain more samples than every measured checkpoint")
    else:
        if args.workload != "cornell-v1":
            parser.error("non-Cornell workloads require --reference-run")
        reference_path = ROOT / "validation/reference/cornell-box-mean.pfm"
        reference = read_pfm(reference_path)
        reference_metadata = {"path": "validation/reference/cornell-box-mean.pfm",
                              "sha256": hashlib.sha256(reference_path.read_bytes()).hexdigest(),
                              "samples": 1024, "sample_index": 0, "light_sampling": "bsdf"}
    if (not 1 <= len(indices) <= 16 or len(set(indices)) != len(indices)
            or any(index < args.warmup + 1
                   or index + max(args.frames) > 1 << 21 for index in indices)):
        parser.error("use 1..16 unique sample indices disjoint from reference, warmup and variance")
    reference_start = reference_metadata["sample_index"]
    reference_end = reference_start + reference_metadata["samples"]
    if any(index < reference_end and reference_start < index + max(args.frames) for index in indices):
        parser.error("measured sample sequences must not overlap the reference")
    ordered = sorted(indices)
    if any(a + max(args.frames) > b for a, b in zip(ordered, ordered[1:])):
        parser.error("measured sample sequences must not overlap")
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("output directory must be empty to preserve previous measurements")
    executable = args.headless.resolve()
    summary = {"schema": "lotus-quality-curve-v2", "status": "fail",
               "label": args.label, "clock_conditions": args.clocks,
               "integrator": args.integrator, "workload": args.workload, "sample_indices": indices,
               "reference": reference_metadata,
               "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()
                   if executable.is_file() else None,
               "rmse_targets": targets, "checkpoints": [], "curves": [],
               "sample_populations": []}
    output.mkdir(parents=True, exist_ok=True)
    try:
        if reference_files:
            (output / "reference").mkdir()
            for source in reference_files:
                target = output / "reference" / source.name
                shutil.copyfile(source, target)
                require(hashlib.sha256(target.read_bytes()).hexdigest() ==
                        reference_metadata["evidence_sha256"][source.name],
                        "reference evidence changed while copying")
        for index in indices:
            sequence = output / f"sequence-{index}" if len(indices) > 1 else output
            sequence.mkdir(parents=True, exist_ok=True)
            checkpoints = []
            for frames in args.frames:
                manifest = baseline.load_manifest(ROOT / "validation/performance-workloads.json")
                manifest["settings"].update(frames=frames, runs=args.runs, warmup=args.warmup)
                manifest["workloads"] = [args.workload]
                manifest["scope"] = "Static independent-sequence MIS quality checkpoint."
                manifest_path = sequence / f"manifest-{frames}.json"
                manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
                cell = sequence / f"spp-{frames}"
                checkpoint = {"sample_index": index, "samples": frames,
                              "run_summary": (cell / "run.json").relative_to(output).as_posix(),
                              "status": "fail"}
                checkpoints.append(checkpoint)
                summary["checkpoints"].append(checkpoint)
                code = baseline.main(["--manifest", str(manifest_path), "--headless", str(executable),
                    "--output", str(cell), "--label", args.label, "--clocks", args.clocks,
                    "--integrator", args.integrator, "--sample-index", str(index),
                    "--gpu-telemetry", args.gpu_telemetry])
                if code:
                    checkpoint["status"] = "skip" if code == 77 else "fail"
                    summary["status"] = checkpoint["status"]
                    return code
                checkpoint["points"] = reduce_runs(cell / "run.json", reference, frames, args.workload)
                require(len({p["image_sha256"] for p in checkpoint["points"]}) == 1,
                        "images differ between timing repetitions of the same sequence")
                checkpoint["status"] = "pass"
            for run in range(args.runs):
                curve = [c["points"][run] for c in checkpoints]
                summary["curves"].append({"sample_index": index, "run": run, "points": curve,
                    "targets": [observed_target(curve, target) for target in targets]})
        summary["sample_populations"] = sample_populations(summary["curves"], args.frames)
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
