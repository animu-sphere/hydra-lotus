"""Compare repeated ReSTIR reports at observed static quality checkpoints.

Uses only the standard library. Never interpolates convergence or treats moving
poses as samples of a static image. Run separately for each integrator/proposal.
"""

import argparse
import json
import math
from pathlib import Path
import statistics


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
            "expected finite nonnegative measurement")
    return value


def population(values):
    for value in values:
        number(value)
    return {"raw": values, "mean": statistics.mean(values),
            "median": statistics.median(values), "min": min(values), "max": max(values)}


def checkpoint(curve, target):
    """First observed checkpoint remaining below target through the final one."""
    last_above = max((i for i, point in enumerate(curve) if point["rmse"] > target), default=-1)
    return curve[last_above + 1] if last_above + 1 < len(curve) else None


def validate_curve(curve, procedure):
    require(bool(curve), "missing static convergence curve")
    expected_samples = [n for n in range(1, procedure["frames"] + 1)
                        if n & (n - 1) == 0 or n == procedure["frames"]]
    require([p["samples"] for p in curve] == expected_samples, "missing or unexpected convergence checkpoints")
    previous_samples, previous_gpu = 0, 0
    availability = {point["cumulative_gpu_ms"] is not None for point in curve}
    require(len(availability) == 1, "inconsistent timestamp availability")
    for point in curve:
        samples = point["samples"]
        require(type(samples) is int and previous_samples < samples <= procedure["frames"],
                "unordered convergence samples")
        previous_samples = samples
        rms = number(point["rmse"])
        raw = point["rmse_raw"]
        require(len(raw) == procedure["sequences"], "wrong independent sequence population")
        for value in raw:
            number(value)
        require(math.isclose(rms, math.sqrt(statistics.mean(x*x for x in raw)), rel_tol=1e-8, abs_tol=1e-10),
                "convergence RMS disagrees with raw sequences")
        gpu = point["cumulative_gpu_ms"]
        gpu_raw = point["cumulative_gpu_ms_raw"]
        if gpu is None:
            require(gpu_raw is None, "unavailable timestamp has a population")
        else:
            require(number(gpu) > previous_gpu, "non-increasing cumulative GPU time")
            previous_gpu = gpu
            require(gpu_raw is not None and len(gpu_raw) == procedure["sequences"],
                    "wrong timestamp population")
            for value in gpu_raw:
                require(number(value) > 0, "nonpositive timestamp")
            require(math.isclose(gpu, statistics.mean(gpu_raw), rel_tol=1e-8, abs_tol=1e-10),
                    "cumulative GPU time disagrees with raw sequences")
    require(previous_samples == procedure["frames"], "missing final convergence checkpoint")


def temporal_residuals(runs, procedure, static):
    measurements = [run.get("temporal_residuals") for run in runs]
    if all(value is None for value in measurements):
        return None  # Older v1 reports did not measure temporal residuals.
    require(all(isinstance(value, dict) for value in measurements),
            "temporal residual measurements must be present in every run")
    for run, value in zip(runs, measurements):
        for metric in ("sequence_mean_rmse", "centered_rmse"):
            series = value[metric]
            raw = series["raw"]
            require(len(raw) == procedure["sequences"], "wrong temporal residual sequence population")
            for entry in raw:
                number(entry)
            require(math.isclose(number(series["mean"]), statistics.mean(raw), rel_tol=1e-8, abs_tol=1e-10),
                    "temporal residual mean disagrees with raw sequences")
        means, centered = value["sequence_mean_rmse"]["raw"], value["centered_rmse"]["raw"]
        lag = value["lag1_autocorrelation_raw"]
        require(len(lag) == procedure["sequences"], "wrong temporal correlation sequence population")
        for correlation, rms in zip(lag, centered):
            require((correlation is None) == (rms == 0), "zero-variance temporal correlation must be null")
            require(correlation is None or (type(correlation) in (int, float) and
                    math.isfinite(correlation) and -1.00000001 <= correlation <= 1.00000001),
                    "invalid temporal correlation")
        fresh = run["fresh_rmse"]["raw"]
        require(len(fresh) == procedure["sequences"] * procedure["frames"],
                "wrong temporal fresh-frame population")
        for entry in fresh:
            number(entry)
        for sequence, (mean, rms) in enumerate(zip(means, centered)):
            start = sequence * procedure["frames"]
            mse = statistics.mean(x*x for x in fresh[start:start + procedure["frames"]])
            require(math.isclose(mse, mean*mean + rms*rms, rel_tol=1e-8, abs_tol=1e-10),
                    "temporal residual decomposition disagrees with fresh-frame error")
        if static:
            require(all(math.isclose(a, b, rel_tol=1e-8, abs_tol=1e-10)
                        for a, b in zip(means, run["convergence"][-1]["rmse_raw"])),
                    "temporal sequence mean disagrees with final static convergence")
    # Keep run and sequence boundaries, including undefined correlations.
    # These frames are not additional independent replications.
    return {key: [value[key]["raw"] for value in measurements]
            for key in ("sequence_mean_rmse", "centered_rmse")} | {
                "lag1_autocorrelation_raw": [value["lag1_autocorrelation_raw"] for value in measurements]}


def close(a, b):
    return math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-12)


def sequence_ensemble(runs, procedure, static, splits):
    if all("sequence_ensemble" not in run for run in runs):
        return None  # Older reports predate the decomposition.
    require(all("sequence_ensemble" in run for run in runs),
            "sequence ensemble measurements must be present in every run")
    measurements = [run["sequence_ensemble"] for run in runs]
    if not static:
        require(all(value is None for value in measurements), "moving workloads have no sequence ensemble")
        return None
    require(all(isinstance(value, dict) for value in measurements), "static workloads need a sequence ensemble")
    for run, value, split in zip(runs, measurements, splits):
        require(value["sequences"] == procedure["sequences"] and value["frames"] == procedure["frames"],
                "wrong sequence ensemble population")
        for metric in ("frame_variance", "sequence_mean_variance", "grand_mean_mse",
                       "grand_mean_noise_mse", "reference_noise_mse"):
            number(value[metric])
        require(type(value["excess_mse"]) in (int, float) and math.isfinite(value["excess_mse"]),
                "invalid excess MSE")
        # The grand mean of all sequences is the static ensemble image.
        require(close(value["grand_mean_mse"], run["ensemble_rmse"] ** 2),
                "sequence ensemble disagrees with the static ensemble error")
        require(close(value["grand_mean_noise_mse"], value["sequence_mean_variance"] / value["sequences"]),
                "grand mean noise disagrees with sequence mean variance")
        reference = 0 if split is None else split[0] ** 2 / 4
        require(close(value["reference_noise_mse"], reference), "reference noise disagrees with split RMS")
        expected = value["grand_mean_noise_mse"] + value["reference_noise_mse"]
        # Excess is a small difference of large terms; compare at their scale.
        require(math.isclose(value["excess_mse"], value["grand_mean_mse"] - expected, rel_tol=0,
                             abs_tol=1e-6 * max(value["grand_mean_mse"], expected) + 1e-15),
                "excess MSE disagrees with its decomposition")
        normalized = value["noise_normalized_mse"]
        require((normalized is None) == (expected == 0), "noise-normalized MSE needs positive expected noise")
        require(normalized is None or close(number(normalized), value["grand_mean_mse"] / expected),
                "noise-normalized MSE disagrees with its decomposition")
        inflation, effective = value["variance_inflation"], value["effective_frames"]
        require((inflation is None) == (value["frame_variance"] == 0) and
                (effective is None) == (inflation is None or inflation == 0),
                "variance inflation needs positive frame variance")
        if inflation is not None:
            require(close(number(inflation), value["frames"] * value["sequence_mean_variance"] / value["frame_variance"]),
                    "variance inflation disagrees with its decomposition")
        if effective is not None:
            require(close(number(effective), value["frames"] / inflation),
                    "effective frames disagree with variance inflation")
    result = {key: [value[key] for value in measurements]
              for key in ("frame_variance", "sequence_mean_variance", "variance_inflation", "effective_frames",
                          "grand_mean_mse", "grand_mean_noise_mse", "reference_noise_mse",
                          "noise_normalized_mse", "excess_mse")}
    for key in ("variance_inflation", "noise_normalized_mse"):
        result[key + "_summary"] = population(result[key]) if all(v is not None for v in result[key]) else None
    return result


def summarize(reports, targets):
    require(len(reports) >= 2, "at least two independent reports are required")
    require(bool(targets), "at least one quality target is required")
    for target in targets:
        require(number(target) > 0, "quality targets must be positive")
    first = reports[0]
    procedure = dict(first["procedure"])
    procedure.pop("sample_index")
    intervals = []
    scene_ids = [s["id"] for s in first["scenes"]]
    require(bool(scene_ids) and len(set(scene_ids)) == len(scene_ids), "invalid workload identities")
    for report in reports:
        require(report["schema"] == "lotus-restir-benchmark-v1" and report["status"] == "pass",
                "only successful ReSTIR reports can be compared")
        settings = dict(report["procedure"])
        seed = settings.pop("sample_index")
        require(settings == procedure and report["build"] == first["build"] and report["device"] == first["device"],
                "build, device and procedure must match except sample_index")
        require([s["id"] for s in report["scenes"]] == scene_ids, "workloads must match")
        require(type(seed) is int and seed >= 0, "invalid measured seed")
        for sequence in range(procedure["sequences"]):
            start = seed + sequence * procedure["sequence_stride"]
            end = start + procedure["frames"]
            require(end <= 1 << 23, "measured seeds overlap warmup/reference ranges")
            require(all(end <= a or start >= b for a, b in intervals), "overlapping measured sample ranges")
            intervals.append((start, end))

    scenes = []
    for index, scene_id in enumerate(scene_ids):
        scene_runs = [report["scenes"][index] for report in reports]
        keys = [(c["mode"], c["candidates"]) for c in scene_runs[0]["cases"]]
        require(len(set(keys)) == len(keys) and keys.count(("mis", 0)) == 1, "invalid case identities")
        require(all([(c["mode"], c["candidates"]) for c in s["cases"]] == keys for s in scene_runs),
                "measured cases must match")
        moving = scene_runs[0]["moving"]
        require(type(moving) is bool and all(s["moving"] == moving for s in scene_runs),
                "inconsistent moving workload identity")
        static = not moving
        # Older v1 reports predate the explicit reference label and regions.
        default_reference = "independent-lambert-integral" if static else "finite-spp-nee-mis"
        reference = scene_runs[0].get("error_reference", default_reference)
        require(reference in ("independent-lambert-integral", "finite-spp-nee-mis") and
                (static or reference == "finite-spp-nee-mis") and
                all(s.get("error_reference", default_reference) == reference for s in scene_runs),
                "inconsistent error reference")
        splits = [s.get("reference_split_rmse") for s in scene_runs]
        require(not any("error_reference" in s for s in scene_runs) or all(s is not None for s in splits),
                "explicit error reference requires split measurements")
        if any(s is not None for s in splits):
            require(all(s is not None and len(s) == (procedure["frames"] if moving else 1)
                        for s in splits), "wrong reference split population")
            for split in splits:
                for value in split:
                    number(value)
        for scene in scene_runs:
            for case in scene["cases"]:
                require((case["convergence"] is not None) == static, "inconsistent static/moving curves")
                if static:
                    validate_curve(case["convergence"], procedure)
        mis_index = keys.index(("mis", 0))
        cases = []
        for case_index, (mode, candidates) in enumerate(keys):
            runs = [s["cases"][case_index] for s in scene_runs]
            for run in runs:
                rgb = run["signed_mean_error_rgb"]
                require(len(rgb) == 3 and all(type(x) in (int, float) and math.isfinite(x) for x in rgb),
                        "invalid signed RGB mean error")
            result = {"mode": mode, "candidates": candidates,
                      "final_prefix_rmse": population([r["convergence"][-1]["rmse"] for r in runs]) if static else None,
                      "fresh_rmse": population([r["fresh_rmse"]["mean"] for r in runs]),
                      "residual_frame_delta_rmse": population([r["residual_frame_delta_rmse"]["mean"] for r in runs]),
                      "ensemble_rmse": population([r["ensemble_rmse"] for r in runs]),
                      "max_abs_tile_mean_error": population([r["max_abs_tile_mean_error"] for r in runs]),
                      "signed_mean_error_rgb": [r["signed_mean_error_rgb"] for r in runs],
                      "quality_budgets": None}
            result["temporal_residuals"] = temporal_residuals(runs, procedure, static)
            result["sequence_ensemble"] = sequence_ensemble(
                runs, procedure, static, splits if reference == "finite-spp-nee-mis" else [None] * len(runs))
            region_keys = [(r["id"], r["pixels"]) for r in runs[0].get("regions", [])]
            require(len(set(k[0] for k in region_keys)) == len(region_keys) and
                    all(isinstance(k[0], str) and k[0] and type(k[1]) is int and k[1] > 0
                        for k in region_keys), "invalid region identities")
            require(all([(r["id"], r["pixels"]) for r in run.get("regions", [])] == region_keys
                        for run in runs), "regions must match")
            regions = []
            for region_index, (region_id, pixels) in enumerate(region_keys):
                region_runs = [r["regions"][region_index] for r in runs]
                for region in region_runs:
                    rgb = region["signed_mean_error_rgb"]
                    require(len(rgb) == 3 and all(type(x) in (int, float) and math.isfinite(x) for x in rgb),
                            "invalid region signed RGB mean error")
                regions.append({"id": region_id, "pixels": pixels,
                                "ensemble_rmse": population([r["ensemble_rmse"] for r in region_runs]),
                                "signed_mean_error_rgb": [r["signed_mean_error_rgb"] for r in region_runs]})
            result["regions"] = regions
            if static:
                budgets = []
                for target in targets:
                    hits = [checkpoint(r["convergence"], target) for r in runs]
                    mis = [checkpoint(s["cases"][mis_index]["convergence"], target) for s in scene_runs]
                    times = [h["cumulative_gpu_ms"] if h else None for h in hits]
                    ratios = [b["cumulative_gpu_ms"] / h["cumulative_gpu_ms"]
                              if b and h and b["cumulative_gpu_ms"] is not None and h["cumulative_gpu_ms"] is not None
                              else None for b, h in zip(mis, hits)]
                    budgets.append({"target_rmse": target, "reached_runs": sum(h is not None for h in hits),
                                    "samples": [h["samples"] if h else None for h in hits],
                                    "cumulative_gpu_ms": times,
                                    "gpu_ms_summary": population(times) if all(t is not None for t in times) else None,
                                    "paired_mis_speedup": ratios,
                                    "speedup_summary": population(ratios) if all(r is not None for r in ratios) else None})
                result["quality_budgets"] = budgets
            cases.append(result)
        scenes.append({"id": scene_id, "error_reference": reference,
                       "reference_split_rmse": splits if splits[0] is not None else None, "cases": cases})
    return {"schema": "lotus-restir-budget-v1", "build": first["build"], "device": first["device"],
            "procedure": procedure, "run_seeds": [r["procedure"]["sample_index"] for r in reports],
            "notes": {"quality": "First observed checkpoint at/below target that stays below through the final checkpoint. No interpolation; no claim beyond the measured horizon.",
                      "reference": "Budgets use each scene's error_reference. Finite-spp NEE/MIS noise is retained, with split discrepancies reported separately; repeated references share seeds.",
                      "replication": "Populations are across separate invocations. Frames within a reused sequence are correlated. Shared references are not independent replications.",
                      "temporal_residuals": "Run/sequence populations retain undefined zero-variance correlations as null. Centering removes persistent error, which remains in sequence_mean_rmse. Motion and finite references affect interpretation; no effective sample count is inferred.",
                      "sequence_ensemble": "Static only. Inflation compares across-sequence variance of sequence means with within-sequence frame variance; noise-normalized MSE compares the grand mean error with its expected noise, including finite-reference noise. Values near one are consistent with uncorrelated frames and with no detectable bias respectively; neither is a confidence bound.",
                      "missing": "Unreached budgets or unavailable GPU timestamps are null. Aggregate times/ratios require every run; no successful-subset averages.",
                      "timing": "Scene GPU time includes reservoir/metadata passes and excludes updates/readback. Dynamic clocks and validation limit inference.",
                      "moving": "Moving poses have no convergence budget. Finite ensemble and tile errors do not establish bias bounds."},
            "scenes": scenes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", type=Path, nargs="+")
    parser.add_argument("--rmse", type=float, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = summarize([json.loads(p.read_text(encoding="utf-8")) for p in args.reports], args.rmse)
        result["source_reports"] = [p.name for p in args.reports]
        serialized = json.dumps(result, indent=2, allow_nan=False) + "\n"
        temporary = args.output.with_name(args.output.name + ".tmp")
        temporary.write_text(serialized, encoding="utf-8")
        temporary.replace(args.output)
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(1, f"ReSTIR budget: {error}\n")


if __name__ == "__main__":
    main()
