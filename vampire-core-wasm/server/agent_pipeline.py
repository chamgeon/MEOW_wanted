"""Fixed-seed WASM comparison of reviewed, built-in collision modes.

All six alternatives run from the same source snapshot and binary, with
different runtime collision/memory settings. Arbitrary model-authored C++ is
never compiled in this workflow.
"""

from __future__ import annotations

import difflib
import hashlib
import json
import math
import shutil
import statistics
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Callable

from agent_candidates import ENGINE, TREE, comparison_algorithms, generate


ROOT = Path(__file__).resolve().parents[1]
GENERATED = Path(__file__).resolve().parent / "generated"
BENCH_FILES = ["core/src/Vector2D.cpp", "core/src/QuadTree.cpp", "core/src/EngineCore.cpp", "core/src/agent_bench.cpp"]
WASM_FILES = ["core/src/Vector2D.cpp", "core/src/QuadTree.cpp", "core/src/EngineCore.cpp", "core/src/bindings.cpp"]
MAX_EXPERIMENT_SECONDS = 300
MAX_BRUTE_FORCE_ENEMIES = 10000
MAX_BENCHMARK_ENEMIES = 100000


def source_snapshot() -> dict[Path, str]:
    paths = (ENGINE, TREE, Path("core/include/EngineCore.h"), Path("core/include/Config.h"),
             Path("core/src/bindings.cpp"), Path("core/src/agent_bench.cpp"))
    return {path: (ROOT / path).read_text(encoding="utf-8") for path in paths}


def source_hash(snapshot: dict[Path, str]) -> str:
    h = hashlib.sha256()
    for path in sorted(snapshot, key=str):
        h.update(str(path).encode("utf-8") + b"\0" + snapshot[path].encode("utf-8"))
    return h.hexdigest()


def process_log(records: list[dict], mode: str, memory: str, distribution: str,
                speed: float = 1.0) -> dict:
    """Keep comparable 1 Hz windows; aggregate stored per-frame percentiles.

    The p99 here is a median of window p99s, *not* a reconstructed tick p99.
    """
    valid = [r for r in records if r.get("collisionMode") == mode
             and r.get("memoryMode") == memory and r.get("distribution") == distribution
             and float(r.get("speedMultiplier", 1.0)) == speed
             and r.get("frames", 0) >= 1 and r.get("slots", 0) > 0
             and isinstance(r.get("simMs"), dict)]
    if len(valid) < 3:
        raise ValueError("Need at least three comparable 1 Hz records for the selected mode, distribution and speed")
    slots = [int(r["slots"]) for r in valid]
    sim = [float(r["simMs"].get("p50", 0)) for r in valid]
    tail = [float(r["simMs"].get("p99", 0)) for r in valid]
    frame = [float((r.get("frameMs") or {}).get("p50", 0)) for r in valid]
    fps = [float(r.get("fps", 0)) for r in valid]
    if not all(math.isfinite(v) and v >= 0 for v in sim + tail + frame + fps):
        raise ValueError("Non-finite or negative timing sample")
    return {
        "records": len(valid), "entityMin": min(slots), "entityMax": max(slots),
        "simMedianMs": round(statistics.median(sim), 3),
        "windowP99MedianMs": round(statistics.median(tail), 3),
        "frameMedianMs": round(statistics.median(frame), 3),
        "fpsMedian": round(statistics.median(fps), 3),
        "longFrames": sum(int(r.get("longFrames", 0)) for r in valid),
        "framesTotal": sum(int(r["frames"]) for r in valid),
        "lowSampleWindows": sum(int(r["frames"] < 3) for r in valid),
        "speedMultiplier": speed,
        "targetDistribution": distribution,
        "note": "Window p99 values are not pooled per-frame p99; benchmark measurements are separate.",
    }


def diagnostic_source(snapshot: dict[Path, str], mode: str) -> str:
    methods = {"BruteForce": "updateRepulsionBruteForce",
               "QuadTree": "updateRepulsionQuadTree",
               "UniformGrid": "updateRepulsionUniformGrid",
               "SpatialHash": "updateRepulsionSpatialHash"}
    name = methods[mode]
    source = snapshot[ENGINE]
    start = source.index(f"void EngineCore::{name}(float dt) {{")
    end = source.index("\nvoid EngineCore::", start + 1)
    relevant = source[start:end]
    if mode == "QuadTree":
        relevant += "\n\n" + snapshot[TREE]
    elif mode in {"UniformGrid", "SpatialHash"}:
        shared = source.index("void EngineCore::updateRepulsionSpatial(float dt, bool hashed) {")
        shared_end = source.index("\nvoid EngineCore::fireProjectile()", shared)
        relevant += "\n\n" + source[shared:shared_end]
    return relevant[:18000]


def _tool(name: str) -> str:
    resolved = shutil.which(name)
    if resolved:
        return resolved
    sdk = ROOT.parent.parent / "emsdk"
    fallback = sdk / ("upstream/emscripten/em++.exe" if name == "em++" else "node/24.19.0_64bit/node.exe")
    if fallback.is_file():
        return str(fallback)
    raise RuntimeError(f"{name} unavailable; activate the Emscripten SDK")


def _run(args: list[str], cwd: Path, timeout: int = 120) -> str:
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                            errors="replace", timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError(f"{Path(args[0]).name} exit {result.returncode}: " +
                           (result.stderr or result.stdout)[-1200:])
    return result.stdout


def _copy_core(destination: Path, snapshot: dict[Path, str]) -> None:
    shutil.copytree(ROOT / "core/include", destination / "core/include")
    shutil.copytree(ROOT / "core/src", destination / "core/src")
    for path, text in snapshot.items():
        (destination / path).write_text(text, encoding="utf-8")


def _compile(root: Path, browser: bool = False, output: Path | None = None) -> Path:
    if browser:
        assert output is not None
        output.mkdir(parents=True, exist_ok=True)
        target = output / "core_engine.js"
        args = [_tool("em++"), "-O3", "-std=c++17", "-msimd128", "--bind", "-sWASM=1",
                "-sMODULARIZE=1", "-sEXPORT_NAME=CoreEngineModule", "-sALLOW_MEMORY_GROWTH=1",
                "-sINITIAL_MEMORY=67108864", "-sENVIRONMENT=web",
                "-sEXPORTED_RUNTIME_METHODS=['HEAPF32','HEAP32']", *WASM_FILES, "-o", str(target)]
    else:
        target = root / "agent_bench.js"
        args = [_tool("em++"), "-O3", "-std=c++17", "-sENVIRONMENT=node",
                "-sEXIT_RUNTIME=1", "-sALLOW_MEMORY_GROWTH=1",
                "-sINITIAL_MEMORY=67108864", *BENCH_FILES, "-o", str(target)]
    _run(args, root, timeout=180)
    if browser and not (output / "core_engine.wasm").is_file():
        raise RuntimeError("Browser WASM artifact missing")
    return target


def _bench(binary: Path, root: Path, count: int, ticks: int, mode: str,
           dist: str, memory: str, speed: float = 1.0,
           frame_dt: float = 0.016) -> dict:
    mode_id = {"BruteForce": "0", "QuadTree": "1", "UniformGrid": "2", "SpatialHash": "3"}[mode]
    args = [_tool("node"), str(binary), str(count), str(ticks),
            mode_id,
            "0" if dist == "Uniform" else "1", "0" if memory == "AoS" else "1",
            str(speed), str(frame_dt)]
    return json.loads(_run(args, root, timeout=120))


def _summary(values: list[float]) -> dict:
    ordered = sorted(values)
    index = (len(ordered) - 1) * 0.99
    lo, hi = math.floor(index), math.ceil(index)
    return {"medianMs": round(statistics.median(values), 3),
            "p99Ms": round(ordered[lo] + (ordered[hi] - ordered[lo]) * (index - lo), 3),
            "samples": len(values)}


def _diff(base: dict[Path, str], candidate: dict[Path, str]) -> str:
    return "".join("".join(difflib.unified_diff(
        base[path].splitlines(keepends=True), candidate[path].splitlines(keepends=True),
        fromfile=str(path), tofile=str(path) + " (candidate)"))
        for path in sorted(base, key=str) if base[path] != candidate[path])


def _compare_world(reference: dict, candidate: dict) -> dict:
    positions_a, positions_b = reference["positions"], candidate["positions"]
    if len(positions_a) != len(positions_b):
        return {"passed": False, "reason": "entity array length changed"}
    delta = max((abs(a - b) for old, new in zip(positions_a, positions_b)
                 for a, b in zip(old, new)), default=0.0)
    passed = (reference["alive"] == candidate["alive"] and
              reference["kills"] == candidate["kills"] and delta <= 0.1)
    return {"passed": passed, "aliveMatch": reference["alive"] == candidate["alive"],
            "killsMatch": reference["kills"] == candidate["kills"],
            "maxPositionDelta": round(delta, 6), "tolerance": 0.1}


def comparison_matrix(mode: str, plan: list[str]) -> list[dict[str, str]]:
    """Three alternative algorithms times both memory layouts, always six rows.

    AI advice may reorder reviewed alternatives but cannot omit one. The
    active algorithm/layout is retained separately as the seventh baseline.
    """
    available = comparison_algorithms(mode)
    ordered = list(dict.fromkeys([name for name in plan if name in available] + available))
    return [{"algorithm": algorithm, "memoryMode": memory,
             "runMode": algorithm}
            for algorithm in ordered for memory in ("AoS", "SoA")]


def run_experiment(job_id: str, request: dict, plan: list[str],
                   progress: Callable[[str, list[dict]], None]) -> dict:
    started = time.monotonic()
    mode, memory, dist = request["collisionMode"], request["memoryMode"], request["distribution"]
    speed = float(request.get("speedMultiplier", 1.0))
    snapshot = source_snapshot()
    if source_hash(snapshot) != request["sourceHash"]:
        raise ValueError("Source changed since the request; collect a new build/source version")
    observations = process_log(request["telemetry"]["records"], mode, memory, dist, speed)
    observed_fps = observations["fpsMedian"]
    frame_dt = max(0.001, min(0.05, 1.0 / observed_fps)) if observed_fps > 0 else 0.016
    observations["benchmarkFrameDtMs"] = round(frame_dt * 1000, 3)
    def bench(binary: Path, root: Path, count: int, ticks: int, algorithm: str,
              scenario: str, layout: str) -> dict:
        return _bench(binary, root, count, ticks, algorithm, scenario, layout, speed, frame_dt)
    planned = comparison_matrix(mode, plan)
    opposite = "Clustered" if dist == "Uniform" else "Uniform"
    observed_count = int(statistics.median(
        [r["slots"] for r in request["telemetry"]["records"]
         if r.get("collisionMode") == mode and r.get("memoryMode") == memory
         and r.get("distribution") == dist
         and float(r.get("speedMultiplier", 1.0)) == speed
         and int(r.get("frames", 0)) >= 1]))
    target_count = max(1, min(MAX_BENCHMARK_ENEMIES, observed_count))
    scenarios = [(min(500, max(1, target_count // 2)), 5, dist),
                 (target_count, 4 if target_count <= 2000 else 2, dist),
                 (min(1000, target_count), 4, opposite)]
    results: list[dict] = []
    with tempfile.TemporaryDirectory(prefix="vampire-candidates-") as temporary:
        work = Path(temporary)
        baseline = work / "baseline"
        _copy_core(baseline, snapshot)
        progress("baseline", results)
        base_bin = _compile(baseline)
        prepared: dict[str, tuple[Path, Path, str]] = {}
        for index, spec in enumerate(planned):
            name, candidate_memory, run_mode = spec["algorithm"], spec["memoryMode"], spec["runMode"]
            if time.monotonic() - started > MAX_EXPERIMENT_SECONDS:
                results.extend({**skipped, "status": "budget_exhausted",
                                "reason": "Total experiment time limit reached"}
                               for skipped in planned[index:])
                break
            candidate_started = time.monotonic()
            item: dict = {**spec, "status": "generating", "reason": None}
            results.append(item)
            try:
                if name not in prepared:
                    if name in {"BruteForce", "QuadTree", "UniformGrid", "SpatialHash"}:
                        # Each algorithm is now a built-in selectable engine path.
                        prepared[name] = (baseline, base_bin, "")
                    else:
                        modified = generate(snapshot, mode, name)
                        diff = _diff(snapshot, modified)
                        if not diff:
                            raise ValueError("Empty candidate diff")
                        root = work / f"candidate-{name}"
                        _copy_core(root, modified)
                        progress("building", results)
                        prepared[name] = (root, _compile(root), diff)
                root, candidate_bin, item["diff"] = prepared[name]
                item["sourceKind"] = "existing" if not item["diff"] else "generated"
                bounded_scenarios = [scenario for scenario in scenarios
                                     if run_mode != "BruteForce" or scenario[0] <= MAX_BRUTE_FORCE_ENEMIES]
                if len(bounded_scenarios) != len(scenarios):
                    item["skippedTargetCount"] = target_count
                item["status"] = "validating"
                progress("validating", results)
                checks = []
                for count, ticks, scenario_dist in bounded_scenarios:
                    reference = bench(base_bin, baseline, count, ticks, mode, scenario_dist, memory)
                    current = bench(candidate_bin, root, count, ticks, run_mode, scenario_dist, candidate_memory)
                    check = _compare_world(reference, current)
                    checks.append({"entities": count, "distribution": scenario_dist, **check})
                item["correctness"] = checks
                item["correctnessPassed"] = all(check["passed"] for check in checks)
                # Even invalid candidates are measured. Their timing is useful
                # evidence but must never qualify them as a recommendation.
                item["status"] = "benchmarking"
                progress("benchmarking", results)
                rows = []
                for count, ticks, scenario_dist in bounded_scenarios:
                    # ABBA counteracts gradual thermal/browser drift.
                    a1 = bench(base_bin, baseline, count, ticks, mode, scenario_dist, memory)
                    b1 = bench(candidate_bin, root, count, ticks, run_mode, scenario_dist, candidate_memory)
                    b2 = bench(candidate_bin, root, count, ticks, run_mode, scenario_dist, candidate_memory)
                    a2 = bench(base_bin, baseline, count, ticks, mode, scenario_dist, memory)
                    base_time = _summary(a1["samples"] + a2["samples"])
                    cand_time = _summary(b1["samples"] + b2["samples"])
                    rows.append({"entities": count, "distribution": scenario_dist,
                                 "baseline": base_time, "candidate": cand_time,
                                 "speedup": round(base_time["medianMs"] / max(cand_time["medianMs"], 0.001), 3)})
                item["benchmarks"] = rows
                primary = next((row for row in rows if row["entities"] == target_count
                                and row["distribution"] == dist), None)
                passed = (primary is not None and primary["speedup"] >= 1.05 and
                          all(row["candidate"]["p99Ms"] <= row["baseline"]["p99Ms"] * 1.10
                              for row in rows))
                item["performancePassed"] = passed
                if item.get("skippedTargetCount"):
                    item.update(status="scale_limit",
                                reason="BruteForce is not run above 10,000 entities; smaller scenarios are measured only")
                elif not item["correctnessPassed"]:
                    item.update(status="correctness_failed",
                                reason="Measured for comparison only: world state differs from baseline")
                elif not passed:
                    item.update(status="performance_failed", reason="Insufficient speedup or p99 regression")
                else:
                    item.update(status="passed", reason=None)
            except (RuntimeError, ValueError, subprocess.TimeoutExpired, OSError) as exc:
                item.update(status="build_failed", reason=str(exc)[:600])
            finally:
                item["elapsedSec"] = round(time.monotonic() - candidate_started, 2)
                progress("evaluating", results)

        ranked = sorted((item for item in results if item["status"] == "passed"),
                        key=lambda item: item["benchmarks"][1]["speedup"], reverse=True)
        winner_item = None
        for item in ranked:
            try:
                progress("packaging", results)
                _compile(prepared[item["algorithm"]][0], browser=True, output=GENERATED / job_id)
                winner_item = item
                break
            except (RuntimeError, subprocess.TimeoutExpired, OSError) as exc:
                item.update(status="browser_build_failed", reason=str(exc)[:600])
        measured = [item for item in results if any(
            row["entities"] == target_count and row["distribution"] == dist
            for row in item.get("benchmarks", []))]
        fastest = min(measured, key=lambda item: next(
            row["candidate"]["medianMs"] for row in item["benchmarks"]
            if row["entities"] == target_count and row["distribution"] == dist)) if measured else None
        return {"status": "ready" if winner_item else "no_improvement",
                "sourceHash": source_hash(snapshot), "observations": observations,
                "baseline": {"algorithm": mode, "memory": memory, "distribution": dist,
                             "speedMultiplier": speed},
                "scenarioCounts": [count for count, _, _ in scenarios],
                "elapsedSec": round(time.monotonic() - started, 2),
                "candidates": results, "winner": winner_item["algorithm"] if winner_item else None,
                "winnerMemory": winner_item["memoryMode"] if winner_item else None,
                "winnerRunMode": winner_item["runMode"] if winner_item else None,
                "fastestMeasured": {"algorithm": fastest["algorithm"], "memoryMode": fastest["memoryMode"],
                                    "eligible": fastest["status"] == "passed"} if fastest else None,
                "diff": winner_item["diff"] if winner_item else None,
                "artifactUrl": f"/api/optimization-jobs/{job_id}/artifacts/core_engine.js" if winner_item else None,
                "limitations": "Fixed-seed pilot WASM runs; differing world state disqualifies a fast candidate. "
                               "BruteForce is not measured above 10,000 entities. "
                               "Small p99 samples do not establish a universal optimum."}
