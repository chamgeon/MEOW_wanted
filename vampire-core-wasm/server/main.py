import os

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
MAX_TOKENS = int(os.environ.get("MAX_TOKENS", "1024"))

app = FastAPI(title="Vampire-Core AI Profiler", version="0.1.0")

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


class OptimizeResponse(BaseModel):
    analysis: str
    model: str


@app.post("/api/optimize", response_model=OptimizeResponse)
async def optimize(req: OptimizeRequest) -> OptimizeResponse:
    client = get_client()
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
                    ),
                }
            ],
        )
        text = "".join(block.text for block in message.content if block.type == "text")
        return OptimizeResponse(analysis=text, model=MODEL)
    except anthropic.APIStatusError as e:
        raise HTTPException(status_code=502, detail=f"{e.status_code}: {e.message}")
    except anthropic.APIError as e:
        raise HTTPException(status_code=502, detail=str(e))


@app.get("/health")
async def health() -> dict:
    return {"status": "ok", "model": MODEL, "key_configured": bool(os.environ.get("ANTHROPIC_API_KEY"))}
