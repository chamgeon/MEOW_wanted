import os
import re
from typing import Any

import anthropic
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

from prompts import SYSTEM_PROMPT, build_user_prompt

load_dotenv()

# Override with MODEL=... in .env if you want a different tier.
# claude-opus-5 gives deeper analysis; claude-haiku-4-5-20251001 is fastest.
MODEL = os.environ.get("MODEL", "claude-sonnet-5")

# Raised from 1024: the response now carries a diagnosis, a code block, an
# impact estimate AND the machine-parsed configuration block. At 1024 the
# configuration block was the part that got truncated, which is precisely the
# part a caller cannot recover by re-reading prose.
MAX_TOKENS = int(os.environ.get("MAX_TOKENS", "1600"))

app = FastAPI(title="Vampire-Core AI Profiler", version="0.2.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:5173", "http://localhost:4173"],
    allow_methods=["POST", "GET"],
    allow_headers=["*"],
)

# AsyncAnthropic, not Anthropic: the sync client blocks the event loop for the
# whole model call, which would stall every other request on this worker.
_client: anthropic.AsyncAnthropic | None = None


def get_client() -> anthropic.AsyncAnthropic:
    global _client
    if _client is None:
        api_key = os.environ.get("ANTHROPIC_API_KEY")
        if not api_key:
            raise HTTPException(
                status_code=500,
                detail="ANTHROPIC_API_KEY not set. Put it in server/.env or export it.",
            )
        _client = anthropic.AsyncAnthropic(api_key=api_key)
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

_VALID_COLLISION = {"bruteforce": "BruteForce", "quadtree": "QuadTree"}
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


@app.post("/api/optimize", response_model=OptimizeResponse)
async def optimize(req: OptimizeRequest) -> OptimizeResponse:
    client = get_client()
    samples = len((req.telemetry or {}).get("records") or [])
    try:
        message = await client.messages.create(
            model=MODEL,
            max_tokens=MAX_TOKENS,
            system=SYSTEM_PROMPT,
            messages=[
                {
                    "role": "user",
                    "content": build_user_prompt(
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
                }
            ],
        )
        text = "".join(block.text for block in message.content if block.type == "text")
        return OptimizeResponse(
            analysis=text,
            model=MODEL,
            recommendation=parse_recommendation(text),
            telemetrySamples=samples,
        )
    except anthropic.APIStatusError as e:
        raise HTTPException(status_code=502, detail=f"{e.status_code}: {e.message}")
    except anthropic.APIError as e:
        raise HTTPException(status_code=502, detail=str(e))


@app.get("/health")
async def health() -> dict:
    return {"status": "ok", "model": MODEL, "key_configured": bool(os.environ.get("ANTHROPIC_API_KEY"))}
