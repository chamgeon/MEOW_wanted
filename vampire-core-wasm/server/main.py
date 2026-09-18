import os
import re
import asyncio
import json
import uuid
from typing import Any

import openai
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from pydantic import BaseModel

from analyze import analyze
from prompts import SYSTEM_PROMPT, build_user_prompt
from agent_candidates import comparison_algorithms
from agent_pipeline import (GENERATED, MAX_BRUTE_FORCE_ENEMIES, diagnostic_source,
                            process_log, run_experiment, source_hash, source_snapshot)

load_dotenv()

# Override with MODEL=... in .env if you want a different OpenAI tier.
# This is deliberately separate from AGENT_MODEL, which ranks verified
# optimization candidates in the background workflow.
MODEL = os.environ.get("MODEL", "gpt-4.1-mini")

# This caps the report returned by the OpenAI Responses API. A malformed or
# truncated response still degrades to readable text in parse_reply below.
MAX_TOKENS = int(os.environ.get("MAX_TOKENS", "5000"))

app = FastAPI(title="Vampire-Core AI Profiler", version="0.2.0")
optimization_jobs: dict[str, dict] = {}
optimization_lock = asyncio.Lock()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:5173", "http://localhost:4173"],
    allow_methods=["POST", "GET"],
    allow_headers=["*"],
)

# A shared async client avoids blocking the event loop while a report is made.
_client: openai.AsyncOpenAI | None = None


def get_client() -> openai.AsyncOpenAI:
    global _client
    if _client is None:
        api_key = os.environ.get("OPENAI_API_KEY")
        if not api_key:
            raise HTTPException(
                status_code=500,
                detail="OPENAI_API_KEY not set. Put it in .env or export it.",
            )
        _client = openai.AsyncOpenAI(api_key=api_key, timeout=35)
    return _client


class OptimizeRequest(BaseModel):
    fps: float
    frameTimeMs: float
    entityCount: int
    collisionMode: str
    memoryMode: str
    codeSnippet: str

    # Both optional, so a client built against v0.1.0 still works unchanged.
    #
    # telemetry is typed as a bare dict rather than a mirrored pydantic model on
    # purpose. The producing schema lives in web/src/logging/PerfLogger.ts and
    # will keep growing fields; a strict model here would turn every frontend
    # addition into a 422 at exactly the wrong moment, and the formatter reads
    # the payload defensively with .get() anyway. The cost of being wrong is a
    # blank column in a table, not a corrupted analysis.
    telemetry: dict[str, Any] | None = None
    auraSnippet: str | None = None

    # Not validated against an enum here, unlike the recommendation parser
    # below. That parser guards a value that gets fed back into embind, where a
    # bad string is a crash; this one only ever reaches a prompt, where the
    # worst case is a mislabelled column. Rejecting the request outright would
    # mean a frontend that adds a third distribution breaks the endpoint before
    # anyone can see what it sent.
    spawnDistribution: str | None = None


class Recommendation(BaseModel):
    """The parsed 'Recommended Configuration' block.

    This exists so the demo can eventually act on the model's verdict -- swap
    the running collision/memory mode, or load a pre-built wasm variant --
    without a human reading the prose. Every field is optional because a parse
    failure must degrade to 'no recommendation' rather than fabricating one; a
    wrong mode swap is worse than none.
    """

    collision:  str | None = None
    memory:     str | None = None
    confidence: str | None = None
    reason:     str | None = None


class OptimizeResponse(BaseModel):
    analysis: str
    model: str
    recommendation: Recommendation | None = None
    telemetrySamples: int = 0


_FIELD_RE = re.compile(r"^\s*(collision|memory|confidence|reason)\s*:\s*(.+?)\s*$", re.IGNORECASE)

_VALID_COLLISION = {"bruteforce": "BruteForce", "quadtree": "QuadTree",
                    "uniformgrid": "UniformGrid", "spatialhash": "SpatialHash"}
_VALID_MEMORY = {"aos": "AoS", "soa": "SoA"}
_VALID_CONFIDENCE = {"high", "medium", "low"}


def parse_recommendation(text: str) -> Recommendation | None:
    """Pull the configuration block out of the model's markdown.

    Values are validated against the enums the engine actually accepts rather
    than passed through. An unrecognised mode string would sail through the API
    and only fail at the embind call, where the error would look like a wasm
    bug instead of a malformed model response.
    """
    idx = text.lower().rfind("## recommended configuration")
    if idx == -1:
        return None

    rec = Recommendation()
    for line in text[idx:].splitlines()[1:]:
        stripped = line.strip()
        # A following heading ends the block. Without this, a model that adds a
        # trailing '## Notes' section would have its prose parsed as fields.
        if stripped.startswith("#"):
            break
        m = _FIELD_RE.match(line)
        if not m:
            continue
        key, value = m.group(1).lower(), m.group(2).strip().strip("`")
        if key == "collision":
            rec.collision = _VALID_COLLISION.get(value.lower())
        elif key == "memory":
            rec.memory = _VALID_MEMORY.get(value.lower())
        elif key == "confidence":
            low = value.lower()
            rec.confidence = low if low in _VALID_CONFIDENCE else None
        elif key == "reason":
            rec.reason = value[:200]

    if rec.collision is None and rec.memory is None:
        return None
    return rec


# Section headings for the woven markdown. Korean, because they sit directly
# above Korean prose in the report window.
#
# prompts.py is held to pure ASCII because a stray character there can reach the
# cp949 console through a log line. These do not: they are concatenated into the
# response body and serialized to UTF-8 JSON, and nothing prints them.
_SECTIONS = (
    ("diagnosis",      "## \ubcd1\ubaa9 \uc9c4\ub2e8",              ""),
    ("snippet",        "## \ucd5c\uc801\ud654\ub41c C++ \uc2a4\ub2c8\ud3ab", "cpp"),
    ("expectedImpact", "## \uc608\uc0c1 \ud6a8\uacfc",              ""),
)


def _weave_markdown(data: dict[str, Any]) -> str:
    """Render the model's JSON object as the markdown string the client shows.

    Done here rather than in the browser so that OptimizeResponse.analysis keeps
    meaning exactly what it meant under the old contract -- one string of
    markdown, ready to display. The frontend needs no change, and a client built
    against v0.1.0 keeps working.

    A section whose field is missing or blank is omitted rather than rendered as
    an empty heading: a heading over nothing reads as "analysed, found nothing",
    which is a different claim from "the model did not return this field".
    """
    parts = []
    for key, heading, fence in _SECTIONS:
        body = str(data.get(key) or "").strip()
        if not body:
            continue
        parts.append(f"{heading}\n```{fence}\n{body}\n```" if fence else f"{heading}\n{body}")
    return "\n\n".join(parts)


def _recommendation_from(obj: Any) -> Recommendation | None:
    """Validate the JSON recommendation against the enums embind accepts.

    Same contract as parse_recommendation: an unrecognised value becomes None
    rather than travelling on to the wasm call, and an object with neither mode
    is no recommendation at all.
    """
    if not isinstance(obj, dict):
        return None
    confidence = str(obj.get("confidence", "")).lower()
    rec = Recommendation(
        collision=_VALID_COLLISION.get(str(obj.get("collision", "")).lower()),
        memory=_VALID_MEMORY.get(str(obj.get("memory", "")).lower()),
        confidence=confidence if confidence in _VALID_CONFIDENCE else None,
        # Dropped from the prompt, but still read if a model emits it, so that
        # re-adding the field to the schema needs no change here.
        reason=str(obj.get("reason", ""))[:200] or None,
    )
    return rec if (rec.collision or rec.memory) else None


def parse_reply(text: str) -> tuple[str, Recommendation | None]:
    """Split the model's reply into display markdown and the parsed verdict.

    Degrades instead of failing. A reply that is not valid JSON -- a stray
    preamble, or a response truncated by MAX_TOKENS mid-snippet -- is passed
    through verbatim and handed to the old markdown parser for the verdict. The
    user then sees a malformed answer, which is recoverable; raising here would
    show them an empty report window, which is not.
    """
    raw = text.strip()
    if raw.startswith("```"):                      # fenced despite the instruction
        raw = raw.split("\n", 1)[-1].rsplit("```", 1)[0]
    try:
        data = json.loads(raw)
    except (json.JSONDecodeError, ValueError):
        return text, parse_recommendation(text)
    if not isinstance(data, dict):
        return text, parse_recommendation(text)
    return _weave_markdown(data) or text, _recommendation_from(data.get("recommendation"))


@app.post("/api/optimize", response_model=OptimizeResponse)
async def optimize(req: OptimizeRequest) -> OptimizeResponse:
    client = get_client()
    samples = len((req.telemetry or {}).get("records") or [])
    try:
        message = await client.responses.create(
            model=MODEL,
            max_output_tokens=MAX_TOKENS,
            instructions=SYSTEM_PROMPT,
            input=build_user_prompt(
                req.fps,
                req.frameTimeMs,
                req.entityCount,
                req.collisionMode,
                req.memoryMode,
                req.codeSnippet,
                telemetry=req.telemetry,
                aura_snippet=req.auraSnippet,
                spawn_distribution=req.spawnDistribution or "Uniform",
            ),
        )
        text = message.output_text
        analysis, recommendation = parse_reply(text)
        return OptimizeResponse(
            analysis=analysis,
            model=MODEL,
            recommendation=recommendation,
            telemetrySamples=samples,
        )
    except openai.APIStatusError as e:
        raise HTTPException(status_code=502, detail=f"{e.status_code}: {e.message}")
    except openai.APIError as e:
        raise HTTPException(status_code=502, detail=str(e))


@app.get("/health")
async def health() -> dict:
    return {"status": "ok", "model": MODEL, "key_configured": bool(os.environ.get("OPENAI_API_KEY"))}


# New verified optimization workflow. The older /api/optimize endpoint remains
# available for clients still using its text-only analysis contract.
class OptimizationJobRequest(BaseModel):
    collisionMode: str
    memoryMode: str
    distribution: str
    speedMultiplier: float = 1.0
    sourceHash: str
    telemetry: dict[str, Any]


async def _plan(req: OptimizationJobRequest, observations: dict, findings: list[dict],
                source: dict) -> tuple[list[str], str, str | None]:
    available = comparison_algorithms(req.collisionMode)
    key = os.environ.get("OPENAI_API_KEY")
    if not key:
        return available, "reviewed candidate fallback (OPENAI_API_KEY missing)", None
    try:
        client = openai.AsyncOpenAI(api_key=key, timeout=35)
        response = await client.responses.create(
            model=os.environ.get("AGENT_MODEL", "gpt-4.1-mini"),
            max_output_tokens=650,
            instructions=("You are a C++ game performance advisor. Read the real active source, "
                          "observations and findings. Return JSON only: "
                          "{\"candidates\":[IDs in priority order],"
                          "\"reason\":\"concise Korean analysis with uncertainty\"}. "
                          "Use only offered IDs; symptoms are hypotheses, not proof. "
                          "If frame time rises but sim time does not, explain that the C++ "
                          "collision path may not be the bottleneck. "
                          "findings[] is arithmetic the server computed over the same window: "
                          "take those numbers as given rather than recomputing them. They are "
                          "measurements, never recommendations, and a finding whose text "
                          "contains a CAVEAT cannot by itself justify ranking a candidate "
                          "first -- say what further evidence would be needed instead."),
            input=json.dumps({"available": available, "observations": observations,
                              "findings": findings,
                              "activeSource": diagnostic_source(source, req.collisionMode)},
                             ensure_ascii=False),
        )
        payload = response.output_text.strip()
        if payload.startswith("```"):
            payload = payload.split("\n", 1)[1].rsplit("```", 1)[0]
        parsed = json.loads(payload)
        if not isinstance(parsed, dict):
            raise ValueError("Advisor response is not an object")
        selected = parsed.get("candidates", [])
        order = [item for item in selected if isinstance(item, str) and item in available] if isinstance(selected, list) else []
        order = list(dict.fromkeys(order + available))[:3]
        return order, "OpenAI advisor", str(parsed.get("reason", ""))[:1500]
    except (openai.APIError, ValueError, KeyError, IndexError, TypeError):
        return available, "reviewed candidate fallback (advisor unavailable)", None


def _save_optimization(job_id: str) -> None:
    directory = GENERATED / job_id
    directory.mkdir(parents=True, exist_ok=True)
    temporary = directory / "report.tmp"
    temporary.write_text(json.dumps(optimization_jobs[job_id], ensure_ascii=False), encoding="utf-8")
    temporary.replace(directory / "report.json")


async def _process_optimization(job_id: str, req: OptimizationJobRequest) -> None:
    async with optimization_lock:
        job = optimization_jobs[job_id]
        try:
            job["status"] = "advising"
            _save_optimization(job_id)
            snapshot = source_snapshot()
            if source_hash(snapshot) != req.sourceHash:
                raise ValueError("Source changed while the experiment was queued")
            observations = process_log(req.telemetry["records"], req.collisionMode,
                                       req.memoryMode, req.distribution, req.speedMultiplier)

            # process_log filters to the requested mode, discarding the other side of
            # every toggle -- which is the evidence analyze recovers.
            try:
                findings = [{"label": f.label, "text": f.text} for f in analyze(req.telemetry)]
            except Exception:
                findings = []
            job.update(findings=findings)

            plan, advisor_mode, advice = await _plan(req, observations, findings, snapshot)
            job.update(advisorMode=advisor_mode, aiAdvice=advice, plannedCandidates=plan)
            _save_optimization(job_id)

            def on_progress(status: str, candidates: list[dict]) -> None:
                # The worker thread owns candidate dictionaries during execution.
                # Snapshot to avoid serializing an object mid-mutation.
                job.update(status=status, candidates=json.loads(json.dumps(candidates)))
                _save_optimization(job_id)

            report = await asyncio.to_thread(run_experiment, job_id, req.model_dump(), plan, on_progress)
            job.update(report)
        except Exception as exc:
            job.update(status="failed", reason=str(exc)[:600])
        _save_optimization(job_id)


@app.get("/api/agent-meta")
async def agent_meta() -> dict:
    return {"sourceHash": source_hash(source_snapshot()),
            "sourceScope": "EngineCore.cpp, QuadTree.cpp, EngineCore.h, Config.h, bindings.cpp and agent_bench.cpp"}


@app.post("/api/optimization-jobs", status_code=202)
async def create_optimization_job(req: OptimizationJobRequest) -> dict:
    if req.collisionMode not in {"BruteForce", "QuadTree", "UniformGrid", "SpatialHash"} or req.memoryMode not in {"AoS", "SoA"}:
        raise HTTPException(status_code=422, detail="Unsupported engine mode")
    if req.distribution not in {"Uniform", "Clustered"}:
        raise HTTPException(status_code=422, detail="Unsupported spawn distribution")
    if req.speedMultiplier not in {0.25, 0.5, 1.0, 2.0, 4.0}:
        raise HTTPException(status_code=422, detail="Unsupported simulation speed")
    records = req.telemetry.get("records")
    if not isinstance(records, list) or not 3 <= len(records) <= 60 or not all(isinstance(r, dict) for r in records):
        raise HTTPException(status_code=422, detail="Supply 3 to 60 recent telemetry records")
    if req.collisionMode == "BruteForce" and any(
        isinstance(record.get("slots"), (int, float)) and
        record["slots"] > MAX_BRUTE_FORCE_ENEMIES for record in records
    ):
        raise HTTPException(status_code=422, detail="BruteForce is limited to 10,000 enemies")
    if req.sourceHash != source_hash(source_snapshot()):
        raise HTTPException(status_code=409, detail="Server source changed; retry with current source version")
    try:
        process_log(records, req.collisionMode, req.memoryMode, req.distribution, req.speedMultiplier)
    except (ValueError, TypeError, KeyError) as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    active = sum(job.get("status") not in {"ready", "no_improvement", "failed", "interrupted"}
                 for job in optimization_jobs.values())
    if active >= 2:
        raise HTTPException(status_code=429, detail="Experiment queue is full; try again later")
    job_id = uuid.uuid4().hex
    optimization_jobs[job_id] = {"jobId": job_id, "status": "queued", "candidates": []}
    _save_optimization(job_id)
    asyncio.create_task(_process_optimization(job_id, req))
    return {"jobId": job_id, "status": "queued"}


@app.get("/api/optimization-jobs/{job_id}")
async def get_optimization_job(job_id: str) -> dict:
    if len(job_id) != 32 or any(c not in "0123456789abcdef" for c in job_id):
        raise HTTPException(status_code=404, detail="Job not found")
    job = optimization_jobs.get(job_id)
    if job is None:
        report = GENERATED / job_id / "report.json"
        if report.is_file():
            job = json.loads(report.read_text(encoding="utf-8"))
            if job.get("status") not in {"ready", "no_improvement", "failed"}:
                job = {**job, "status": "interrupted", "reason": "Server restarted before the experiment finished"}
    if job is None:
        raise HTTPException(status_code=404, detail="Job not found")
    return job


@app.get("/api/optimization-jobs/{job_id}/artifacts/{filename}")
async def get_optimization_artifact(job_id: str, filename: str) -> FileResponse:
    if filename not in {"core_engine.js", "core_engine.wasm"}:
        raise HTTPException(status_code=404, detail="Artifact not found")
    job = await get_optimization_job(job_id)
    if job.get("status") != "ready":
        raise HTTPException(status_code=404, detail="Artifact not ready")
    path = GENERATED / job_id / filename
    if not path.is_file():
        raise HTTPException(status_code=404, detail="Artifact not found")
    return FileResponse(path)
