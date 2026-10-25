#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the shared manifest sequentially; keep raw evidence per adapter."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

from gpu_telemetry import GpuTelemetry

MOTIONS = ("static", "camera", "transform", "deform")


def load_manifest(path):
    manifest = json.loads(Path(path).read_text(encoding="utf-8"))
    if manifest["schema"] != "lotus-performance-workloads-v1":
        raise ValueError("unsupported workload manifest")
    settings = manifest["settings"]
    for key, low, high in (("size", 16, 4096), ("frames", 1, 4096),
                           ("warmup", 1, 4096), ("runs", 1, 16)):
        value = settings[key]
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f"invalid {key}")
    if (settings["sample_index"] != 0 or settings["max_bounces"] != 64
            or settings["light_sampling"] != "mis"):
        raise ValueError("this procedure requires seed 0, 64 bounces and MIS")
    workloads = manifest["workloads"]
    if (not workloads or len(set(workloads)) != len(workloads)
            or any(not isinstance(w, str) or not w.replace("-", "").isalnum() for w in workloads)):
        raise ValueError("invalid or duplicate workload IDs")
    motions = manifest.get("motions", ["static"])
    if ("static" not in motions or len(set(motions)) != len(motions)
            or any(m not in MOTIONS for m in motions)):
        raise ValueError("invalid or duplicate motions")
    return manifest


def command(executable, adapter, workload, integrator, settings, report, label):
    if adapter == "headless":
        cmd = [str(executable), "--benchmark", str(report),
                "--benchmark-label", label, "--benchmark-workload", workload,
                "--benchmark-integrator", integrator, "--benchmark-light-sampling", "mis",
                "--benchmark-size", str(settings["size"]), "--benchmark-frames", str(settings["frames"]),
                "--benchmark-warmup", str(settings["warmup"])]
        if settings["sample_index"]:
            cmd += ["--benchmark-sample-index", str(settings["sample_index"])]
        if settings.get("motion", "static") != "static":
            cmd += ["--benchmark-motion", settings["motion"]]
        return cmd
    return [str(executable), "--workload", workload, "--benchmark", str(report),
            "--integrator", integrator, "--nee", "on", "--width", str(settings["size"]),
            "--height", str(settings["size"]), "--samples", str(settings["frames"]),
            "--frames", str(settings["frames"] + settings["warmup"]),
            "--benchmark-warmup", str(settings["warmup"]), "--vsync", "off",
            "--hidden", "--validate", "--allow-unavailable"]


def check_report(report, adapter, workload, integrator, settings):
    data = json.loads(report.read_text(encoding="utf-8"))
    if adapter == "headless":
        procedure = data["procedure"]
        if (data["schema"] != "lotus-benchmark-v1" or data["status"] != "pass"
                or procedure["integrator"] != integrator or procedure["light_sampling"] != "mis"
                or procedure["width"] != settings["size"] or procedure["height"] != settings["size"]
                or procedure["measured_frames"] != settings["frames"]
                or procedure["warmup_frames"] != settings["warmup"]
                or procedure["sample_index"] != settings["sample_index"]
                or procedure.get("motion", "static") != settings.get("motion", "static")
                or len(data["scenes"]) != 1 or data["scenes"][0]["id"] != workload
                or data["scenes"][0]["status"] != "pass"
                or data["scenes"][0]["measurements"]["validation"]["messages"] != 0):
            raise ValueError("invalid headless measurement")
        motion = settings.get("motion", "static")
        if motion != "static":
            moved = data["scenes"][0]["measurements"]["motion"]
            frames = settings["frames"]
            if (len(moved["update_wall_ms"]["raw"]) != frames
                    or moved["blas_updates"] != (frames if motion == "deform" else 0)
                    or moved["tlas_updates"] != (0 if motion == "camera" else frames)):
                raise ValueError("motion did not edit every measured frame")
        image = report.parent / data["scenes"][0]["measurements"]["image"]
        return {"image_sha256": hashlib.sha256(image.read_bytes()).hexdigest()}
    if (data["schema"] != "lotus.viewport-benchmark/v1" or data["scene_source"] != workload
            or data["controls"]["integrator"] != integrator or not data["controls"]["nee"]
            or data["controls"]["restir"] != "off" or data["controls"]["temporal_radiance"]
            or data["controls"]["denoise"] or not data["controls"]["accumulate"]
            or data["width"] != settings["size"] or data["height"] != settings["size"]
            or data["measurement"]["frames"] != settings["frames"]
            or data["measurement"]["warmup_frames"] != settings["warmup"]
            or data["samples_per_pixel"] != settings["frames"]
            or data["validation_messages"] != 0 or data["readback_bytes"] != 0):
        raise ValueError("invalid viewport measurement or accumulation reset")
    return {}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).resolve().parents[1] /
                        "validation/performance-workloads.json")
    parser.add_argument("--headless", type=Path)
    parser.add_argument("--viewport", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--label", required=True, help="source revision and build identity")
    parser.add_argument("--clocks", required=True, help="locked clock settings, or explicitly unlocked")
    parser.add_argument("--integrator", choices=("reference", "wavefront"), default="reference")
    parser.add_argument("--workload", action="append", help="optional subset of manifest IDs")
    parser.add_argument("--sample-index", type=int, default=0,
                        help="headless measured sequence start; default: manifest seed 0")
    parser.add_argument("--motion", choices=MOTIONS, default="static",
                        help="headless per-frame scene motion; default: static accumulation")
    parser.add_argument("--plan", action="store_true", help="print commands without running or writing")
    parser.add_argument("--gpu-telemetry", choices=("off", "nvidia-smi"), default="off",
                        help="optional whole-device clock/memory observations (may perturb timing)")
    parser.add_argument("--gpu-sample-ms", type=int, default=100,
                        help="delay between telemetry queries, 100..10000 ms (default: 100)")
    args = parser.parse_args(argv)
    if not 100 <= args.gpu_sample_ms <= 10000:
        parser.error("gpu sample interval must be 100..10000 ms")
    manifest = load_manifest(args.manifest)
    settings = manifest["settings"]
    if not 0 <= args.sample_index <= (1 << 21) - settings["frames"]:
        parser.error("measured sample range must end before variance sample index 2097152")
    if args.sample_index and args.viewport:
        parser.error("nonzero sample index is supported only by headless")
    if args.motion not in manifest.get("motions", ["static"]):
        parser.error("motion is absent from the manifest")
    if args.motion != "static" and args.viewport:
        parser.error("motion is supported only by headless")
    settings["sample_index"] = args.sample_index
    settings["motion"] = args.motion
    workloads = args.workload or manifest["workloads"]
    if any(w not in manifest["workloads"] for w in workloads):
        parser.error("workload is absent from the manifest")
    if len(set(workloads)) != len(workloads):
        parser.error("duplicate workload selection would overwrite measurements")
    adapters = [(name, executable.resolve()) for name, executable in
                (("headless", args.headless), ("viewport", args.viewport)) if executable]
    if not adapters:
        parser.error("supply --headless and/or --viewport")
    output = args.output.resolve()
    if not args.plan:
        if output.exists() and any(output.iterdir()):
            parser.error("output directory must be empty to preserve previous measurements")
        output.mkdir(parents=True, exist_ok=True)
    summary = {"schema": "lotus-performance-run-v1", "label": args.label,
               "clock_conditions": args.clocks, "manifest": manifest,
               "gpu_telemetry": args.gpu_telemetry,
               "integrator": args.integrator, "started_unix": int(time.time()), "runs": []}
    exit_code = 0
    try:
        for run in range(settings["runs"]):
            for workload in workloads:
                for adapter, executable in adapters:
                    stem = f"{adapter}-{workload}-{run}"
                    report = output / (stem + ".json")
                    cmd = command(executable, adapter, workload, args.integrator, settings, report, args.label)
                    if args.plan:
                        print(json.dumps(cmd))
                        continue
                    print(stem, flush=True)
                    entry = {"adapter": adapter, "workload": workload, "run": run,
                             "exit_code": None, "status": "fail", "report": None,
                             "command": cmd, "log": stem + ".log"}
                    summary["runs"].append(entry)
                    log = output / entry["log"]
                    observer = (GpuTelemetry(output / (stem + ".gpu.json"), args.gpu_sample_ms)
                                if args.gpu_telemetry == "nvidia-smi" else None)
                    try:
                        if observer:
                            observer.start()
                        result = subprocess.run(cmd, capture_output=True, text=True,
                                                encoding="utf-8", errors="replace", timeout=300)
                    except subprocess.TimeoutExpired as error:
                        # TimeoutExpired can carry bytes even with text=True.
                        def decoded(value):
                            if isinstance(value, bytes):
                                return value.decode("utf-8", errors="replace")
                            return value or ""
                        entry["error"] = "process timed out after 300 seconds"
                        log.write_text(decoded(error.stdout) + decoded(error.stderr) +
                                       "\n" + entry["error"] + "\n", encoding="utf-8")
                        raise RuntimeError(f"{stem} timed out; inspect its log") from error
                    except OSError as error:
                        entry["error"] = str(error)
                        log.write_text(str(error) + "\n", encoding="utf-8")
                        raise
                    finally:
                        entry["report"] = report.name if report.exists() else None
                        if observer:
                            entry["gpu_telemetry"] = observer.finish()
                    entry["exit_code"] = result.returncode
                    log.write_text(result.stdout + result.stderr, encoding="utf-8")
                    if result.returncode == 77:
                        if not (result.stdout + result.stderr).strip():
                            raise ValueError("unexplained capability skip")
                        entry["status"] = "skip"
                        exit_code = 77
                    elif result.returncode != 0:
                        entry["status"] = "fail"
                        raise RuntimeError(f"{stem} failed; inspect its log")
                    else:
                        entry.update(check_report(report, adapter, workload, args.integrator, settings))
                        entry["status"] = "pass"
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        summary["error"] = str(error)
        print(error, file=sys.stderr)
        exit_code = 1
    finally:
        if not args.plan:
            summary["completed_unix"] = int(time.time())
            summary["status"] = "pass" if exit_code == 0 else "skip" if exit_code == 77 else "fail"
            (output / "run.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
