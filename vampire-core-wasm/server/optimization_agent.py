"""Constrained optimization experiment for the local Vampire-Core demo.

The first generator uses a reviewed source transformation. The optional LLM
explains the diagnosis; model-authored C++ is deliberately not executed here.
"""

from __future__ import annotations

import difflib
import hashlib
import json
import math
import os
import shutil
import statistics
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATED = Path(__file__).resolve().parent / "generated"
SOURCE = Path("core/src/EngineCore.cpp")
BENCH_SOURCE = [
    "core/src/Vector2D.cpp",
    "core/src/QuadTree.cpp",
    "core/src/EngineCore.cpp",
    "core/src/agent_bench.cpp",
]
WASM_SOURCE = [
    "core/src/Vector2D.cpp",
    "core/src/QuadTree.cpp",
    "core/src/EngineCore.cpp",
    "core/src/bindings.cpp",
]


def _tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    emsdk = ROOT.parent.parent / "emsdk"
    fallback = (
        emsdk / "upstream/emscripten/em++.exe" if name == "em++"
        else emsdk / "node/24.19.0_64bit/node.exe"
    )
    if fallback.exists():
        return str(fallback)
    raise RuntimeError(f"{name} not found. Activate Emscripten SDK first.")


def _run(command: list[str], cwd: Path, timeout: int = 90) -> str:
    result = subprocess.run(
        command, cwd=cwd, capture_output=True, text=True, errors="replace",
        timeout=timeout, check=False,
    )
    if result.returncode:
        raise RuntimeError(
            f"Command failed ({result.returncode}): {Path(command[0]).name}\n"
            + (result.stderr or result.stdout)[-3000:]
        )
    return result.stdout


def _generate_candidate(source: str) -> str:
    """Replace only the selected repulsion method with the existing spatial index.

    Reusing the checked-in QuadTree implementation makes the first vertical
    slice inspectable and prevents model output from becoming executable code.
    """
    start_marker = "void EngineCore::updateRepulsionBruteForce(float dt) {"
    end_marker = "\nvoid EngineCore::updateRepulsionQuadTree(float dt) {"
    if source.count(start_marker) != 1 or source.count(end_marker) != 1:
        raise RuntimeError("Expected repulsion source markers were not found exactly once")
    before, rest = source.split(start_marker, 1)
    _, after = rest.split(end_marker, 1)
    replacement = (
        start_marker + "\n"
        "    // Candidate: use the existing spatial index for nearby-pair lookup.\n"
        "    updateRepulsionQuadTree(dt);\n"
        "}\n"
    )
    return before + replacement + end_marker + after


def _copy_core(target: Path) -> None:
    shutil.copytree(ROOT / "core/include", target / "core/include")
    shutil.copytree(ROOT / "core/src", target / "core/src")


def _compile_bench(root: Path) -> Path:
    output = root / "agent_bench.js"
    _run(
        [_tool("em++"), "-O3", "-std=c++17", "-sENVIRONMENT=node",
         "-sEXIT_RUNTIME=1", *BENCH_SOURCE, "-o", str(output)],
        root,
    )
    return output


def _compile_browser(root: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    _run(
        [_tool("em++"), "-O3", "-std=c++17", "-msimd128", "--bind",
         "-sWASM=1", "-sMODULARIZE=1", "-sEXPORT_NAME=CoreEngineModule",
         "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=67108864",
         "-sENVIRONMENT=web", "-sEXPORTED_RUNTIME_METHODS=['HEAPF32','HEAP32']",
         *WASM_SOURCE, "-o", str(destination / "core_engine.js")],
        root,
    )
    if not (destination / "core_engine.wasm").exists():
        raise RuntimeError("Browser WASM was not produced")


def _bench(binary: Path, root: Path, count: int, ticks: int) -> dict:
    raw = _run([_tool("node"), str(binary), str(count), str(ticks)], root, timeout=90)
    return json.loads(raw)


def _percentile(values: list[float], p: float) -> float:
    ordered = sorted(values)
    index = (len(ordered) - 1) * p
    lower = math.floor(index)
    upper = math.ceil(index)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def _summary(values: list[float]) -> dict:
    return {
        "medianMs": round(statistics.median(values), 3),
        "p99Ms": round(_percentile(values, 0.99), 3),
        "samples": len(values),
    }


def run_experiment(job_id: str, samples: list[dict], collision_mode: str) -> dict:
    if collision_mode != "BruteForce":
        raise ValueError("First implementation supports BruteForce collision mode only")

    raw_source = (ROOT / SOURCE).read_text(encoding="utf-8")
    candidate_source = _generate_candidate(raw_source)
    diff = "".join(difflib.unified_diff(
        raw_source.splitlines(keepends=True), candidate_source.splitlines(keepends=True),
        fromfile=str(SOURCE), tofile=str(SOURCE) + " (candidate)",
    ))
    usable = [s for s in samples if s.get("simMs", 0) > 0 and s.get("entities", 0) > 0]
    observed = _summary([float(s["simMs"]) for s in usable]) if usable else None
    diagnosis = (
        "BruteForce 경로는 적마다 모든 적을 검사합니다. 적 수가 커질수록 "
        "쌍 검사 수가 제곱으로 증가하므로, 주변 적만 조회하는 기존 QuadTree 경로를 "
        "후보로 검증합니다. 이 설명은 원인 가설이며 아래 실측으로 판정합니다."
    )

    with tempfile.TemporaryDirectory(prefix="vampire-agent-") as temp:
        base = Path(temp) / "baseline"
        candidate = Path(temp) / "candidate"
        _copy_core(base)
        _copy_core(candidate)
        (candidate / SOURCE).write_text(candidate_source, encoding="utf-8")

        base_binary = _compile_bench(base)
        candidate_binary = _compile_bench(candidate)
        rows = []
        max_delta = 0.0
        alive_match = True
        for count, ticks in [(500, 10), (2000, 6), (5000, 4)]:
            # ABBA order offsets drift without changing either scenario.
            base_a = _bench(base_binary, base, count, ticks)
            cand_a = _bench(candidate_binary, candidate, count, ticks)
            cand_b = _bench(candidate_binary, candidate, count, ticks)
            base_b = _bench(base_binary, base, count, ticks)
            baseline = _summary(base_a["samples"] + base_b["samples"])
            optimized = _summary(cand_a["samples"] + cand_b["samples"])
            alive_match &= base_a["alive"] == cand_a["alive"]
            for old, new in zip(base_a["positions"], cand_a["positions"]):
                max_delta = max(max_delta, abs(old[0] - new[0]), abs(old[1] - new[1]))
            rows.append({"entities": count, "baseline": baseline,
                         "candidate": optimized,
                         "speedup": round(baseline["medianMs"] / max(optimized["medianMs"], 0.001), 2)})

        # This first candidate changes query/update order. A small numerical
        # drift is allowed, but a visibly different world must be rejected.
        equivalent = alive_match and max_delta <= 2.0
        faster = rows[-1]["speedup"] >= 1.15
        no_tail_regression = all(
            row["candidate"]["p99Ms"] <= row["baseline"]["p99Ms"] * 1.05
            for row in rows
        )
        status = "ready" if equivalent and faster and no_tail_regression else "rejected"
        artifact_url = None
        if status == "ready":
            output = GENERATED / job_id
            _compile_browser(candidate, output)
            artifact_url = f"/api/optimization-jobs/{job_id}/artifacts/core_engine.js"

    return {
        "status": status,
        "generator": "reviewed spatial-index transformation",
        "diagnosis": diagnosis,
        "observed": observed,
        "sourceFile": str(SOURCE),
        "sourceHash": hashlib.sha256(raw_source.encode("utf-8")).hexdigest(),
        "candidateHash": hashlib.sha256(candidate_source.encode("utf-8")).hexdigest(),
        "diff": diff,
        "correctness": {"aliveMatch": alive_match, "maxPositionDelta": round(max_delta, 4),
                        "tolerance": 2.0, "passed": equivalent},
        "benchmarks": rows,
        "artifactUrl": artifact_url,
        "reason": None if status == "ready" else
                  ("World-state comparison failed" if not equivalent else
                   "Measured speedup below 15%" if not faster else
                   "Tail latency regressed in one scenario"),
    }
