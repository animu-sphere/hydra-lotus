"""Run a plan of headless ReSTIR benchmark invocations, optionally concurrently.

Uses only the standard library. A plan is JSON:

    {"executable": "build/<target>/adapters/headless/lotus-headless",
     "output": "build/restir-<topic>",
     "label": "<source> plus <change>; Release; <date>",
     "defaults": {"benchmark-size": 32, "restir-sequences": 64, ...},
     "runs": {"<name>": {"restir-workload": "distant", "restir-seed": 17}, ...}}

`executable` may instead be an argv prefix list. Each run passes its merged
options as `--<key> <value>` to `--benchmark-restir <output>/<name>.json`,
logging to `<output>/<name>.log`. Relative paths resolve against the plan's
directory. A run is kept when its report already passed with the same command
(recorded in `<name>.argv.json`), so an interrupted campaign resumes and an
edited plan reruns what changed. Concurrent invocations share the GPU: their
image statistics are unaffected, but timings are not comparable, so `--jobs`
above one appends `; jobs=N` to each report's label. Measure timing with
`--jobs 1`. `<output>/campaign.json` records every command, wall time and status.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys
import time

RESERVED = {"benchmark-restir", "benchmark-label"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_plan(path):
    plan = json.loads(path.read_text(encoding="utf-8"))
    require(isinstance(plan, dict), "plan must be an object")
    for key in ("executable", "output", "label"):
        require(isinstance(plan.get(key), (str, list)) and plan[key], f"plan requires {key}")
    require(isinstance(plan.get("runs"), dict) and plan["runs"], "plan requires runs")
    defaults = plan.get("defaults", {})
    require(isinstance(defaults, dict), "defaults must be an object")
    for name, options in plan["runs"].items():
        require(name and all(c.isalnum() or c in "-_." for c in name), f"invalid run name {name!r}")
        require(isinstance(options, dict), f"run {name} options must be an object")
        for key in (*defaults, *options):
            require(key not in RESERVED, f"{key} is set by the campaign")
    return plan


def resolve(base, value):
    path = Path(value)
    return path if path.is_absolute() else base / path


def commands(plan, base, jobs):
    """Ordered (name, report path, argv) for every run."""
    executable = plan["executable"]
    prefix = [str(resolve(base, executable))] if isinstance(executable, str) else list(executable)
    output = resolve(base, plan["output"])
    label = plan["label"] + (f"; jobs={jobs}" if jobs > 1 else "")
    result = []
    for name, options in plan["runs"].items():
        report = output / f"{name}.json"
        argv = [*prefix, "--benchmark-restir", str(report), "--benchmark-label", label]
        for key, value in {**plan.get("defaults", {}), **options}.items():
            argv += [f"--{key}", str(value).lower() if isinstance(value, bool) else str(value)]
        result.append((name, report, argv))
    return result


def status(report):
    try:
        return json.loads(report.read_text(encoding="utf-8")).get("status")
    except (OSError, ValueError, AttributeError):
        return None


def identity(report):
    """The jobs-independent command recorded beside a report."""
    try:
        return json.loads(report.with_suffix(".argv.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def run(name, report, argv, recorded, cwd):
    started = time.time()
    report.with_suffix(".argv.json").unlink(missing_ok=True)
    with report.with_suffix(".log").open("w", encoding="utf-8") as log:
        code = subprocess.run(argv, cwd=cwd, stdout=log, stderr=subprocess.STDOUT).returncode
    report.with_suffix(".argv.json").write_text(json.dumps(recorded) + "\n", encoding="utf-8")
    return {"name": name, "command": argv, "exit_code": code, "status": status(report),
            "started_unix": started, "wall_s": time.time() - started}


def campaign(plan_path, jobs=1, dry_run=False, log=print):
    require(jobs >= 1, "jobs must be at least 1")
    plan = load_plan(plan_path)
    base = plan_path.resolve().parent
    output = resolve(base, plan["output"])
    # A passing report is kept only when the same command produced it.
    work = [(*item, recorded) for item, (_, _, recorded)
            in zip(commands(plan, base, jobs), commands(plan, base, 1))]
    pending = [item for item in work if status(item[1]) != "pass" or identity(item[1]) != item[3]]
    pending_names = {item[0] for item in pending}
    for name, _, argv, _ in work:
        log(("RUN " if name in pending_names else "KEEP ") + name)
        if dry_run:
            log("  " + subprocess.list2cmdline(argv))
    if dry_run:
        return []
    output.mkdir(parents=True, exist_ok=True)
    started = time.time()
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run, *item, base) for item in pending]
        results = []
        for future in futures:
            result = future.result()
            log(f"{result['name']}: {result['status'] or 'no report'} "
                f"(exit {result['exit_code']}, {result['wall_s']:.0f} s)")
            results.append(result)
    kept = [{"name": name, "status": "pass", "kept": True}
            for name, _, _, _ in work if name not in pending_names]
    manifest = {"schema": "lotus-restir-campaign-v1", "plan": plan, "jobs": jobs,
                "started_unix": started, "wall_s": time.time() - started, "runs": kept + results}
    (output / "campaign.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return [r for r in results if r["exit_code"] != 0 or r["status"] != "pass"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("plan", type=Path)
    parser.add_argument("--jobs", type=int, default=1,
                        help="concurrent invocations (default 1; timing needs 1)")
    parser.add_argument("--dry-run", action="store_true", help="print commands without running")
    args = parser.parse_args()
    try:
        failed = campaign(args.plan, args.jobs, args.dry_run)
    except ValueError as error:
        parser.error(str(error))
    for result in failed:
        print(f"FAILED {result['name']}: see {result['name']}.log", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
