"""Serial, completion-inclusive renderer microbenchmark; no ROM or game FPS claims."""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess


def run(exe, backend, label, scale, args, validation=False):
    out = args.out / (label + ("-validation" if validation else "")) / str(scale)
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for key in ("VK_INSTANCE_LAYERS", "VK_LAYER_ENABLES", "VK_LAYER_VALIDATE_SYNC"):
        env.pop(key, None)
    if validation:
        env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
        env["VK_LAYER_VALIDATE_SYNC"] = "1"
    cmd = [str(exe), "--backend", backend, "--out", str(out), "--scale", str(scale),
           "--warmup", str(1 if validation else args.warmup),
           "--repeat", str(1 if validation else args.repeats),
           "--iters", str(2 if validation else args.iters)]
    if label == "vulkan-baseline":
        cmd.append("--skip-wide")  # baseline has no wide_dump_full hook
    result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=120, encoding="utf-8", errors="replace")
    (out / "run.log").write_text(result.stdout, encoding="utf-8")
    if result.returncode or "Validation Error" in result.stdout or "SYNC-HAZARD" in result.stdout:
        raise RuntimeError(f"{label} failed ({result.returncode}); see {out / 'run.log'}")
    reports = [json.loads(line) for line in result.stdout.splitlines()
               if line.startswith('{"backend":')]
    if len(reports) != 1:
        raise RuntimeError(f"Missing/unparseable measurement from {label}")
    report = reports[0]
    expected = {"software": 0, "opengl": 1, "vulkan": 2}[backend]
    if report["effective_backend"] != expected or report["scale"] != scale:
        raise RuntimeError(f"Unexpected renderer fallback: {report}")
    samples = [r["wall_ms"] / report["iters"] for r in report["repeats"]]
    hashes = []
    for r in report["repeats"]:
        data = (out / f"{backend}-r{r['repeat']}.vram16").read_bytes()
        if len(data) != 1024 * 512 * 2:
            raise RuntimeError("Incomplete VRAM capture")
        hashes.append(hashlib.sha256(data).hexdigest())
    if len(set(hashes)) != 1:
        raise RuntimeError(f"Nondeterministic repeated output for {label}")
    report.update(label=label, command=cmd, median_ms_per_iteration=statistics.median(samples),
                  min_ms_per_iteration=min(samples), max_ms_per_iteration=max(samples),
                  stable_sha256=hashes[0], validation=validation, output_dir=str(out))
    return report


def compare(a, b):
    def pixels(report):
        path = Path(report["output_dir"]) / f"{report['backend']}-r0.vram16"
        return array.array("H", path.read_bytes())
    left, right = pixels(a), pixels(b)
    diff = sum(x != y for x, y in zip(left, right))
    return {"left": a["label"], "right": b["label"], "scale": a["scale"],
            "different_pixels": diff, "total_pixels": len(left),
            "exact_match": diff == 0}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--baseline", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--scales", type=int, nargs="+", default=[1])
    p.add_argument("--warmup", type=int, default=20)
    p.add_argument("--repeats", type=int, default=7)
    p.add_argument("--iters", type=int, default=100)
    p.add_argument("--validate", action="store_true",
                   help="Also run Vulkan with core/synchronization validation (requires layer installed)")
    args = p.parse_args()
    args.exe = args.exe.resolve()
    args.out = args.out.resolve()
    if args.baseline:
        args.baseline = args.baseline.resolve()
    if args.warmup < 0 or args.repeats < 1 or args.iters < 1 or any(s not in (1, 2, 4) for s in args.scales):
        p.error("Positive samples and scales 1, 2, 4 are required")
    results, comparisons, validations = [], [], []
    for scale in args.scales:
        if args.validate:
            validations.append(run(args.exe, "vulkan", "vulkan", scale, args, True))
        current = [run(args.exe, name, name, scale, args)
                   for name in ("software", "opengl", "vulkan")]
        if args.baseline:
            current.append(run(args.baseline, "vulkan", "vulkan-baseline", scale, args))
        results.extend(current)
        for a, b in ((0, 1), (0, 2), (1, 2)):
            comparisons.append(compare(current[a], current[b]))
        if args.baseline:
            comparisons.append(compare(current[3], current[2]))
    report = {"kind": "renderer-microbenchmark", "timed_work": "fixed draws/uploads plus full 1MiB VRAM readback per iteration",
              "excludes": "game CPU emulation, presentation, vsync; not NoGraphicsAPI",
              "results": results, "comparisons": comparisons, "validation_runs": validations}
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for r in results:
        print(f"{r['label']} {r['scale']}x: {r['median_ms_per_iteration']:.4f} ms/iteration")
    for c in comparisons:
        print(f"{c['left']} vs {c['right']} {c['scale']}x: {c['different_pixels']} differing pixels")
    print(args.out / "report.json")


if __name__ == "__main__":
    main()
