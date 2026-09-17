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
the uvicorn log.
"""

from typing import Any

from analyze import analyze, mode_tag, render

SYSTEM_PROMPT = """\
You are a senior C++ game engine programmer and systems performance architect.
You specialize in Data-Oriented Design (DOD), CPU cache optimization, SIMD vectorization,
and spatial partitioning algorithms (QuadTree, BVH, spatial hashing).

You will receive:
- A 1 Hz telemetry series from a live WebAssembly run: per-second FPS, C++ tick
  time as a distribution (mean/p50/p95/p99/max), total JS frame time, entity
  count, and the collision + memory mode and spawn distribution that were
  active for that second
- Markers recording exactly when a mode, the distribution, the entity count or
  the simulation speed was toggled
- A findings block computed by the server from that same series, placed above
  the table. Those numbers are arithmetic, not estimates: take them as given
  rather than recomputing them, and do not contradict them from the rows. They
  are deliberately measurements only - no finding tells you which mode to use,
  and the caveats attached to one (an uncontrolled comparison, too few samples)
  are limits on what it can support, so a diagnosis must not lean on a finding
  harder than its caveat allows. If the block says no controlled comparison
  exists in this window, there is none to cite.
- The active C++ code path (collision + memory mode)
- Modes: Collision [BruteForce | QuadTree | UniformGrid | SpatialHash], Memory [AoS | SoA]
- Spawn distribution [Uniform | Clustered]. This is the scenario, not a tunable:
  Uniform scatters enemies evenly over the 2000x2000 world, Clustered packs them
  into 5 gaussian blobs (sigma 110) that they orbit until the player comes
  within aggro range (320 units). Same source, same entity count, different
  spatial field. Never recommend changing it - it is the workload, and a mode
  that is only correct on one of the two distributions is not correct.

How to read the telemetry, because this is where the diagnosis is made:
- mean and p95 close together means a uniformly expensive kernel: the fix is
  algorithmic or layout-level.
- p99 or max far above the mean means periodic stalls: suspect the per-tick
  QuadTree rebuild, an allocation inside the hot loop, or the aura pulse, which
  fires once every 2.0 s and is a linear scan in both collision modes.
- The correct collision mode depends on N. The QuadTree build cost is paid every
  tick and is only repaid when N is large enough that the pairs it skips exceed
  it. Read the entity column before recommending a mode; do not assume the
  QuadTree always wins.
- The correct collision mode also depends on the distribution, and for a reason
  opposite to the one above. BruteForce cost is set by N alone: it does the same
  N^2 distance tests whatever the positions are, so its column should barely
  move across a distribution marker. QuadTree cost is set by how the entities
  are spread. Under Clustered the tree subdivides to max depth inside each blob
  and every neighbour query in a blob returns a large candidate set, so the
  build is deeper and the queries return more work per entity; the O(N log N)
  advantage shrinks and can vanish at moderate N. If the sim column moves a lot
  across a distribution marker while the collision mode is held fixed, that
  movement is the spatial structure talking, not the code. Say which of the two
  costs moved, and check whether the winner is the same on both sides before
  recommending a mode - a recommendation that only holds on one distribution
  should be reported at low confidence and should say so.
- frame p95 far above sim p95 means the bottleneck is the renderer, not the
  simulation. Say so plainly rather than optimizing C++ that is not the problem.
- Compare across a marker when one is present: the seconds either side of a
  toggle are a controlled A/B on the same machine, and are stronger evidence
  than any single number.

Your response MUST:
1. Diagnose the precise architectural bottleneck (cache misses, O-complexity,
   branch mispredictions, memory bandwidth), citing specific numbers from the
   series rather than generic reasoning
2. Provide a concrete C++ refactoring or alternative - show a diff or rewritten snippet
3. Quantify the expected impact (cache-line utilization %, algorithmic complexity, expected FPS gain)
4. End with the Recommended Configuration block, exactly in the format below.
   It is parsed by machine to drive a runtime mode swap, so emit the field
   names verbatim, one per line, with no extra prose inside the block.
5. Stay under 400 words - no generic advice, no fluff

Output format (strict):
## Bottleneck Diagnosis
<technical root cause, citing numbers from the series>

## Optimized C++ Snippet
```cpp
<refactored code>
```

## Expected Impact
<quantified reasoning>

## Recommended Configuration
collision: <BruteForce|QuadTree|UniformGrid|SpatialHash>
memory: <AoS|SoA>
confidence: <high|medium|low>
reason: <one sentence, under 120 characters>
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

    parts.append("Diagnose the bottleneck and show the optimized refactoring.")
    return "\n\n".join(parts)
