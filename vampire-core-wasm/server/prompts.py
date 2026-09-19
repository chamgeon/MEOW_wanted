"""Prompt construction for the AI code profiler.

The interesting design decision here is the telemetry table. The frontend
logger emits one JSON record per second with full percentile distributions; a
naive integration would dump that JSON straight into the prompt. That is a bad
idea for two reasons. It is roughly 400 tokens per second of runtime, so a
30-second window costs more than the code being analysed, and a model reading
nested JSON has to do the transposition into a time series itself before it can
see a trend. A fixed-width table of the same data is about a fifth of the size
and puts each metric in a column the model can scan down.

Everything written here must stay pure ASCII: the demo is driven from a cp949
Korean Windows console, where a stray typographic dash renders as mojibake in
the uvicorn log. That constraint is about this file's own bytes, not about the
model's reply: the prose fields of the response are asked for in Korean, and
they travel as UTF-8 JSON over HTTP to a browser, never through that console.
The instruction to write Korean is therefore itself written in English.
"""

from typing import Any

from analyze import analyze, mode_tag, render

SYSTEM_PROMPT = """\
You are a senior C++ game engine programmer specializing in Data-Oriented
Design, cache optimization, SIMD, and spatial partitioning.

Input:
- A 1 Hz telemetry table from a live WebAssembly run: FPS, C++ tick time
  (mean/p50/p95/p99/max), JS frame time, entity count, and the collision mode,
  memory mode and spawn distribution active for each second.
- Markers for every mode, distribution, entity-count or speed toggle.
- A findings block the server computed from that same series. Those numbers are
  arithmetic: take them as given, never recompute or contradict them. They are
  measurements, never recommendations, and a finding's caveat (uncontrolled
  comparison, too few samples) limits what it can support. If it says no
  controlled comparison exists, there is none to cite.
- The active C++ code path.
- Modes: Collision [BruteForce | QuadTree | UniformGrid | SpatialHash],
  Memory [AoS | SoA], Distribution [Uniform | Clustered].

Distribution is the workload, not a tunable: Uniform scatters enemies evenly,
Clustered packs them into 5 gaussian blobs. Never recommend changing it, and a
mode that wins on only one of the two is not correct.

Reading the telemetry - this is where the diagnosis is made:
- mean and p95 close together: uniformly expensive kernel, so the fix is
  algorithmic or layout-level.
- p99 or max far above mean: periodic stalls - the per-tick QuadTree rebuild, an
  allocation in the hot loop, or the aura pulse (linear scan, every 2.0 s).
- frame p95 far above sim p95: the renderer is the bottleneck. Say so plainly
  rather than optimizing C++ that is not the problem.
- The right mode depends on N. The QuadTree build is paid every tick and repaid
  only when N is large enough. Read the entity column; QuadTree does not always
  win.
- The right mode also depends on distribution. BruteForce runs the same N^2
  tests whatever the positions are, so its cost barely moves across a
  distribution marker. QuadTree cost is set by how entities are spread: under
  Clustered it subdivides to max depth and every query returns a large candidate
  set, so its advantage shrinks. If sim moves across a distribution marker at
  fixed mode, that is the spatial structure talking, not the code. Say which
  cost moved and check the winner on both sides - a mode that wins on only one
  distribution is low confidence, and say so.
- Compare across a marker when one is present: the seconds either side of a
  toggle are a controlled A/B and beat any single number.

Your response must:
1. Diagnose the architectural bottleneck (cache misses, O-complexity, branch
   mispredictions, memory bandwidth), citing numbers from the series.
2. Give a concrete C++ refactoring.
3. State what it changes and in which direction, inventing NO numbers for it.
   The snippet you were shown is a condensed illustration, not the file that
   gets compiled, so a predicted cache-utilization % or FPS figure would be
   fabricated. Name the mechanism (fewer bytes touched per entity, aliasing
   removed so the loop vectorizes, complexity class changed) and stop.
   Telemetry numbers are evidence for the diagnosis; never extrapolate them
   into a prediction about the refactoring.
4. Emit a recommendation object, parsed by machine to drive a runtime mode
   swap, using the enum spellings verbatim.
5. Copy the Risk projection finding's numbers (if present above) into
   riskProjection, in Korean, without changing them. If the finding says
   no projection was made, state that instead of inventing one.

Language: diagnosis, expectedImpact and riskProjection in KOREAN. Keep technical terms with no
settled Korean form in English inside the Korean sentence (cache line, prefetch,
SoA, AoS, QuadTree, p95, branch misprediction, SIMD), and keep every number,
unit and metric name exactly as the telemetry writes it - "sim_p95" stays
"sim_p95". snippet is C++ with English identifiers and comments. recommendation
holds enum values only and has no free-text field.

Output a single JSON object and NOTHING else - no prose, no ```json fence. The
first character is { and the last is }. It is parsed with json.loads, so inside
a string escape newlines as \\n (never a literal line break; snippet is
multi-line), a backslash as \\\\ and a quote as \\". No trailing commas, no
comments, no NaN/Infinity.

{
  "diagnosis":      "<Korean. Root cause, citing numbers from the series.>",
  "snippet":        "<C++ refactoring. Compilable fragment, not a diff.>",
  "expectedImpact": "<Korean. What it changes and which way cost moves. No invented figures.>",
  "riskProjection":  "<Korean, one sentence. Restate the Risk projection finding above (projected value, 90% interval, probability) using its numbers verbatim. If none was made, say so.>",
  "recommendation": {
    "collision":  "BruteForce" | "QuadTree" | "UniformGrid" | "SpatialHash",
    "memory":     "AoS" | "SoA",
    "confidence": "high" | "medium" | "low"
  }
}

Hard limits (a truncated reply is unparseable JSON, worse than a shallow one):
- diagnosis:      at most 500 Korean characters
- snippet:        at most 30 lines; hot loop only, not the class
- expectedImpact: at most 150 Korean characters, mechanism and direction only
Never pad. If you run long, shorten the code, not the numbers.
"""

# Column widths chosen so a 10,000-entity run at 8 ms with a 3-digit frame count
# still lines up. Misaligned columns defeat the entire point of using a table.
_HEADER = (
    "  t(s)  mode/dst        N      fps   sim_mean  sim_p95  sim_p99  sim_max  "
    "frm_p95  over/frames  kills"
)
_RULE = "  " + "-" * (len(_HEADER) - 2)


def _mode_tag(collision: str, memory: str, distribution: str = "Uniform") -> str:
    """Short mode tag: 'QT/SoA/U', 'BF/AoS/C'. Full names in every row would
    triple the width of the column for no added information.

    The distribution is folded into the same column rather than given its own
    because it belongs to the same idea: this triple is the complete answer to
    'what was running during this second'. Splitting it out would let a reader
    scan the mode column, see it unchanged across a step in sim_mean, and
    conclude the machine hiccuped.

    Defaulted to Uniform so that records written before the distribution axis
    existed still render. They were all Uniform runs -- that was the only
    behaviour the engine had -- so the default is the true value, not a guess.

    Delegates to analyze.mode_tag so the findings above cannot label a segment
    differently from the rows here. Speed is not passed through: '@4x' would push
    this column past the header width, so it goes in the note line instead.
    """
    return mode_tag(collision, memory, distribution)


def _fmt_record(rec: dict[str, Any]) -> str:
    sim = rec.get("simMs") or {}
    frm = rec.get("frameMs") or {}
    over = f"{rec.get('longFrames', 0)}/{rec.get('frames', 0)}"
    return (
        f"  {rec.get('t', 0.0):5.1f}  "
        f"{_mode_tag(rec.get('collisionMode', '?'), rec.get('memoryMode', '?'), rec.get('distribution', 'Uniform')):<10}"
        f"{rec.get('entities', 0):>7}  "
        f"{rec.get('fps', 0.0):>7.1f}  "
        f"{sim.get('mean', 0.0):>8.2f}  "
        f"{sim.get('p95', 0.0):>7.2f}  "
        f"{sim.get('p99', 0.0):>7.2f}  "
        f"{sim.get('max', 0.0):>7.2f}  "
        f"{frm.get('p95', 0.0):>7.2f}  "
        f"{over:>11}  "
        f"{rec.get('kills', 0):>5}"
    )


def format_telemetry(telemetry: dict[str, Any] | None) -> str:
    """Render a TelemetrySession (as posted by the frontend) as a table.

    Returns an empty string when there is nothing to show, so the caller can
    omit the section entirely rather than sending a header over no data - an
    empty table reads to the model as 'measured, found nothing', which is a
    different and much more misleading claim than 'not measured'.
    """
    if not telemetry:
        return ""

    # Filtered, not trusted: the endpoint validates nothing inside telemetry, and
    # the old truthiness check let a string through to _fmt_record row by row.
    records = [r for r in (telemetry.get("records") or []) if isinstance(r, dict)]
    if not records:
        return ""

    meta = telemetry.get("meta") or {}
    lines: list[str] = []

    budget = meta.get("budgetMs", 16.667)
    lines.append(
        f"Telemetry ({len(records)} samples at {meta.get('intervalMs', 1000)} ms, "
        f"frame budget {budget:.2f} ms, {meta.get('hardwareConcurrency', 0)} logical cores)"
    )
    lines.append("Columns are milliseconds unless noted. sim = C++ EngineCore::tick only;")
    lines.append("frm = tick + canvas draw; over = frames above the 60 FPS budget.")
    lines.append("mode/dst is collision/memory/distribution: "
                 "BF|QT|UG|SH, SoA|AoS, U=Uniform C=Clustered.")

    # Not a column (it would not fit), but it cannot be omitted either: two rows
    # with the same mode tag and a different speed are not the same experiment.
    speeds = sorted({float(r.get("speedMultiplier", 1.0)) for r in records})
    if speeds != [1.0]:
        lines.append(
            "Simulation speed in this window: "
            + ", ".join(f"{v:g}x" for v in speeds)
            + ". tick() is called with dt * speed, so cost is not comparable "
              "across a speed change even at the same mode and N."
        )
    lines.append("")
    lines.append(_HEADER)
    lines.append(_RULE)
    lines.extend(_fmt_record(r) for r in records)

    # Markers go after the table, keyed by the same t column, so a step change
    # in the rows above has a cause the model can point at by timestamp.
    markers = [m for m in (telemetry.get("markers") or []) if m.get("kind") != "session"]
    if markers:
        lines.append("")
        lines.append("Toggle markers (t in the same timebase as the table):")
        for m in markers:
            lines.append(
                f"  t={m.get('t', 0.0):.1f}  {m.get('kind')}: "
                f"{m.get('from') or '(start)'} -> {m.get('to')}"
            )

    return "\n".join(lines)


def build_user_prompt(
    fps: float,
    frame_time_ms: float,
    entity_count: int,
    collision_mode: str,
    memory_mode: str,
    code_snippet: str,
    telemetry: dict[str, Any] | None = None,
    aura_snippet: str | None = None,
    # Keyword-compatible default for the same reason _mode_tag has one: a client
    # built before this axis existed was, by construction, running Uniform.
    spawn_distribution: str = "Uniform",
) -> str:
    parts = [
        "Current Configuration:\n"
        f"  FPS: {fps:.1f}\n"
        f"  Sim Frame Time: {frame_time_ms:.2f} ms\n"
        f"  Active Entities: {entity_count}\n"
        f"  Collision Mode: {collision_mode}\n"
        f"  Memory Layout: {memory_mode}\n"
        f"  Spawn Distribution: {spawn_distribution}"
    ]

    table = format_telemetry(telemetry)
    if table:
        # Findings first, table still in full below so the model can check them.
        # Wrapped: a schema change should cost the block, not the whole analysis.
        try:
            findings = render(analyze(telemetry))
        except Exception:
            findings = ""
        if findings:
            parts.append(findings)
        parts.append(table)
    else:
        # Said explicitly. Otherwise the model has no way to distinguish a run
        # with no history from a bug that dropped the series, and it will
        # confidently generalise from the single instantaneous sample above.
        parts.append(
            "No telemetry series was captured for this request; only the single "
            "instantaneous sample above is available. Weight the recommendation "
            "accordingly and report confidence as low unless the code itself is "
            "conclusive."
        )

    # The distribution is named in the heading but the snippet is unchanged by
    # it, and saying so is load-bearing: it forecloses the reading where the
    # model attributes a cost difference across a distribution marker to some
    # branch it assumes exists in code it has not been shown.
    parts.append(
        f"Active Code Snippet ({collision_mode} / {memory_mode}, running on a "
        f"{spawn_distribution} world -- the source below is identical under both "
        f"distributions):\n```cpp\n{code_snippet}\n```"
    )

    if aura_snippet:
        parts.append(
            "Aura pulse, which runs identically in both collision modes and "
            f"fires once every 2.0 s:\n```cpp\n{aura_snippet}\n```"
        )

    # Restated at the end as well as in the system prompt: the JSON-only rule is
    # the one instruction whose violation costs the whole response, and the last
    # thing in the context is the thing most reliably obeyed.
    parts.append(
        "Diagnose the bottleneck and show the optimized refactoring. Reply with "
        "the JSON object described in the system prompt and nothing else - no "
        "fence, no preamble. diagnosis and expectedImpact are written in Korean. "
        "Do not put a predicted percentage or FPS figure in expectedImpact."
    )
    return "\n\n".join(parts)
