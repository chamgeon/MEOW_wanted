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

import analyze

SYSTEM_PROMPT = """\
You are a senior C++ game engine programmer and systems performance architect.
You specialize in Data-Oriented Design (DOD), CPU cache optimization, SIMD vectorization,
and spatial partitioning algorithms (QuadTree, BVH, spatial hashing).

You will receive:
- A computed findings block: marker A/B comparisons, spike ratios, aura
  correlation, renderer share and budget pressure, all calculated by the server
  from the series. These are arithmetic, already done and already checked - do
  not recompute them, do not contradict them, and do not restate a trend in
  vaguer terms than the block states it. They are measurements only; the block
  deliberately draws no conclusion about which mode to use, and that judgement
  is yours to make from them plus the code. When a finding says a comparison is
  uncontrolled or absent, that constraint is binding: do not manufacture the
  comparison it says the data does not support.
- A 1 Hz telemetry series from a live WebAssembly run: per-second FPS, C++ tick
  time as a distribution (mean/p50/p95/p99/max), total JS frame time, entity
  count, and the collision + memory mode and spawn distribution that were
  active for that second
- Markers recording exactly when a mode, the distribution, or the entity count
  was toggled
- The active C++ source, read from the repository at request time: the real
  functions, with exact file paths and line numbers. Cite them. A claim about
  the kernel should name the line it is about. Note that the repulsion function
  contains BOTH memory layouts behind an `if (memoryMode_ == ...)` branch - only
  the branch matching the active memory mode ran, so do not attribute cost to
  the other one. If a fallback notice says only a paraphrased snippet was
  available, do NOT cite line numbers and do not claim to have read the file.
- Modes: Collision [BruteForce | QuadTree], Memory [AoS | SoA]
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
  than any single number. The findings block has already done this comparison
  and excluded the sample that straddles the toggle; use its numbers rather than
  reading the two spans off the table yourself.

Your response is a JSON object matching the schema you were given. There is no
prose wrapper, no markdown headings, and no code fences anywhere in it.

Field by field:

- diagnosis: the precise architectural bottleneck (cache misses, O-complexity,
  branch mispredictions, memory bandwidth). Name the mechanism and the line it
  lives on. Under 150 words.

- evidence: the specific numbers the diagnosis rests on, one claim per entry,
  each quoting a figure from the computed findings or the table. An entry that
  contains no number is not evidence and does not belong here.

- patch: the edit you want made, or null if the right answer is a mode change
  rather than a code change. This is applied by machine, so:
    * file must be one of the paths shown to you, copied exactly.
    * old_str must be copied VERBATIM from the source above -- byte for byte,
      including indentation and comments. It is matched literally against the
      file on disk. If it does not appear exactly once, the patch is rejected
      and your work is discarded, so quote enough surrounding lines to be
      unique and do not retype from memory, reformat, or tidy it.
    * new_str is what replaces it. Compilable C++ consistent with the
      surrounding code, using only names that already exist in the files shown.
    * Prefer one surgical edit over a rewrite of a whole function. A large
      old_str is more likely to be misquoted, and a misquoted patch is worth
      nothing at all.
    * Return null rather than inventing an edit. A null patch with a good
      diagnosis is a useful answer; a patch that does not apply is not.

- expected_impact: quantified -- cache-line utilisation, algorithmic
  complexity, expected ms or FPS change -- and reasoned from the numbers in
  evidence rather than asserted.

- recommendation: the configuration to run, machine-parsed to drive a runtime
  mode swap. collision and memory must be exactly one of the listed values.
  Set confidence to low when the findings block says the window holds no
  controlled comparison, and say why in reason.

Two standing rules:
- Do not propose a change the source already makes. You are reading the real
  file, so recommending an allocation be hoisted when it is already hoisted, or
  a check added that is already there, is a factual error about code you were
  shown.
- Do not manufacture a comparison the findings block says the data does not
  support.
"""

# ---------------------------------------------------------------------------
# The response schema, enforced by output_config.format rather than by asking
# politely for JSON in the prompt.
#
# Every field is required, because the strict json_schema format has no notion
# of an optional key -- absence is expressed as an explicit null in a nullable
# type, which is a distinction worth keeping: "the model chose not to propose a
# patch" and "the model forgot the field" should not look the same to the parser.
#
# collision/memory/confidence are constrained by enum here AND validated again in
# main.py. That is not redundant. The enum makes a malformed value unlikely; the
# server-side check makes acting on one impossible, and it is the server-side
# check that protects the embind call, where a bad mode string is a wasm crash
# rather than a bad answer.
# ---------------------------------------------------------------------------
OUTPUT_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "diagnosis":       {"type": "string"},
        "evidence":        {"type": "array", "items": {"type": "string"}},
        "expected_impact": {"type": "string"},
        "patch": {
            "type": ["object", "null"],
            "properties": {
                "file":      {"type": "string"},
                "symbol":    {"type": "string"},
                "old_str":   {"type": "string"},
                "new_str":   {"type": "string"},
                "rationale": {"type": "string"},
            },
            "required": ["file", "symbol", "old_str", "new_str", "rationale"],
            "additionalProperties": False,
        },
        "recommendation": {
            "type": "object",
            "properties": {
                "collision":  {"type": "string", "enum": ["BruteForce", "QuadTree"]},
                "memory":     {"type": "string", "enum": ["AoS", "SoA"]},
                "confidence": {"type": "string", "enum": ["high", "medium", "low"]},
                "reason":     {"type": "string"},
            },
            "required": ["collision", "memory", "confidence", "reason"],
            "additionalProperties": False,
        },
    },
    "required": ["diagnosis", "evidence", "expected_impact", "patch", "recommendation"],
    "additionalProperties": False,
}


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
    """
    c = "QT" if collision == "QuadTree" else "BF"
    d = "C" if distribution == "Clustered" else "U"
    return f"{c}/{memory}/{d}"


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

    records = telemetry.get("records") or []
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
    lines.append("mode/dst is collision/memory/distribution: QT|BF, SoA|AoS, U=Uniform C=Clustered.")
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
    # Real source, rendered by sources.render(). When present it REPLACES both
    # the client's code_snippet and aura_snippet rather than joining them: the
    # bundle already contains fireAura and the real kernel, and showing the
    # model two versions of the same function would make it spend its budget
    # reconciling them -- and the paraphrase would lose, so the only effect is
    # cost. Empty string and None both mean "extraction produced nothing".
    source_bundle: str | None = None,
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
        # Findings first, table second. The table is evidence the model can check
        # the findings against; putting it first would mean thirty rows of
        # numbers arrive before anything says what to look for in them.
        computed = analyze.render(analyze.analyze(telemetry))
        if computed:
            parts.append(computed)
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

    if source_bundle:
        # The distribution is named in the bundle header but the source is
        # unchanged by it, and saying so is load-bearing: it forecloses the
        # reading where the model attributes a cost difference across a
        # distribution marker to some branch it assumes exists in code it has
        # not been shown.
        parts.append(source_bundle)
        parts.append(
            "The source above is identical under both spawn distributions; "
            "there is no distribution-dependent branch in it. A cost difference "
            "across a distribution marker is the spatial field changing, not the "
            "code."
        )
    else:
        # Stated, not silently degraded. The system prompt tells the model to
        # cite line numbers; if extraction failed it is about to be handed a
        # paraphrase with no line numbers in it, and a model that has been asked
        # for citations and given none will invent them.
        parts.append(
            "NOTE: the repository source was not available to this request, so "
            "the block below is the frontend's illustrative paraphrase, not the "
            "code as written. Its inner loops are abbreviated and some bodies "
            "are replaced by comments. Do not cite line numbers from it, do not "
            "claim to have read the file, and treat the absence of a detail as "
            "unknown rather than as evidence the code omits it."
        )
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
