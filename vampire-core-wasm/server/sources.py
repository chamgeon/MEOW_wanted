"""Real C++ source extraction for the profiler agent.

WHY THIS EXISTS

The frontend posts a `codeSnippet` drawn from a hand-written table in
web/src/components/CodePanel.tsx. That table is a teaching aid: about sixteen
lines per configuration, sized to fit the panel without scrolling, with the
inner loop body replaced by a comment. It is the right thing to show a human
watching the demo and the wrong thing to hand a model that is being asked to
diagnose a bottleneck, because the parts it elides are exactly the parts that
cost time -- the liveness branch in the innermost loop, the per-entity clamp,
the tree rebuild's interaction with dead entities. An agent reading the
paraphrase can only return generic advice, and can credit itself for fixing
things the real code never did wrong.

So the panel keeps its snippet and the agent gets the file. Two roles, two
artifacts, one of which is now ground truth.

WHY EXTRACTION IS BY SYMBOL AND NOT BY LINE RANGE

Line ranges are correct exactly until someone edits the file above them, and
then they are silently wrong -- the prompt still fills, the model still answers,
and it answers about the wrong function. Brace matching from a symbol costs
thirty lines here and cannot drift.

WHY THE WHOLE FUNCTION, BRANCHES AND ALL

EngineCore::updateRepulsionQuadTree contains both the SoA and the AoS path
behind `if (memoryMode_ == MemoryMode::SoA)`. Cutting the dead branch would
produce a snippet that does not exist in the repository, and the patch the agent
proposes has to apply to the file as written. The branch is labelled in the
header line instead, so the model knows which side ran without being shown a
doctored function.

ASCII ONLY, for the reason given at the top of prompts.py: this output is
rendered into a prompt that is echoed through a cp949 console.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path

# server/ sits next to core/, so the repo root is one level up. Overridable
# because a containerised backend may mount the tree somewhere else -- and when
# it is mounted nowhere, collect() returns nothing and the caller falls back to
# the client's snippet rather than failing the request.
CORE_ROOT = Path(os.environ.get("CORE_ROOT", Path(__file__).resolve().parent.parent))

# Total line budget for the bundle. The repulsion kernel plus its container
# header plus the tree is already ~350 lines; past roughly this point the source
# starts crowding out the telemetry table, and the table is where the diagnosis
# is actually made. Chunks are dropped lowest-priority-first, and the drop is
# stated in the output so the model knows the bundle is partial.
BUDGET_LINES = int(os.environ.get("SOURCE_BUDGET_LINES", "560"))


@dataclass(frozen=True)
class Chunk:
    """One extracted region, carrying the address it came from.

    `path` is repo-relative: the agent quotes it back in a patch header, and an
    absolute path from the server's filesystem would be meaningless to anyone
    applying that patch.
    """

    path:  str
    label: str
    start: int   # 1-based, inclusive
    end:   int   # 1-based, inclusive
    text:  str
    note:  str = ""

    def render(self) -> str:
        # start == 0 marks a chunk whose text is not a contiguous region of the
        # file (see extract_constants). Emitting a range for it would be a
        # fabricated citation in a bundle whose header promises exact ones, and
        # the model would quote it back in a patch that cannot apply.
        where = self.path if self.start == 0 else f"{self.path}:{self.start}-{self.end}"
        head  = f"// ==== {self.label}  [{where}] ===="
        if self.note:
            head += f"\n// {self.note}"
        return f"{head}\n{self.text}"


# ---------------------------------------------------------------------------
# Brace matching
# ---------------------------------------------------------------------------

def _blank_noncode(src: str) -> str:
    """Return `src` with comments and literals replaced by spaces of equal
    length, so brace counting sees only code while every offset still lines up
    with the original text.

    Overkill for this codebase today -- there are no braces inside strings in
    core/ -- but a single `"{"` added to a log message later would otherwise
    truncate a function at a brace that is not there, and the symptom would be
    a prompt containing half a kernel, which reads as plausible.
    """
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                if src[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                if i + 1 < n:
                    out[i + 1] = " "
                i += 2
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\":
                    out[i] = " "
                    i += 1
                if i < n and src[i] != "\n":
                    out[i] = " "
                i += 1
            if i < n:
                out[i] = " "
                i += 1
        else:
            i += 1
    return "".join(out)


def _leading_comment(lines: list[str], first: int) -> int:
    """Index of the first line of the comment block directly above `first`.

    The comments in core/ carry the design rationale -- why alive is uint8_t and
    not bool, why the aura is a linear scan in both modes -- and an agent that
    cannot see them will propose changes those comments already rule out. Rule
    lines (`// -----`) are treated as the top border of the block and dropped,
    since they separate sections rather than describing the function.
    """
    i = first
    while i > 0:
        prev = lines[i - 1].strip()
        if not prev.startswith("//"):
            break
        if set(prev.replace("//", "").strip()) <= {"-"} and len(prev) > 6:
            break
        i -= 1
    return i


def extract_symbol(rel_path: str, symbol: str, label: str = "", note: str = "") -> Chunk | None:
    """Extract the definition of `symbol` by brace matching.

    Returns None rather than raising: a missing file or a renamed function is a
    reason to send less context, never a reason to fail an analysis request that
    still has thirty seconds of perfectly good telemetry in it.
    """
    full = CORE_ROOT / rel_path
    try:
        src = full.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None

    code  = _blank_noncode(src)
    lines = src.splitlines()
    pat   = re.compile(r"\b" + re.escape(symbol) + r"\s*\(")

    for m in pat.finditer(code):
        # Walk to the body. A ';' first means this was a declaration or a call,
        # not the definition we want, so keep looking.
        j = code.find("{", m.end())
        s = code.find(";", m.end())
        if j == -1 or (s != -1 and s < j):
            continue

        depth = 0
        end   = -1
        for k in range(j, len(code)):
            if code[k] == "{":
                depth += 1
            elif code[k] == "}":
                depth -= 1
                if depth == 0:
                    end = k
                    break
        if end == -1:
            continue

        first = src.count("\n", 0, m.start())
        last  = src.count("\n", 0, end)
        first = _leading_comment(lines, first)
        return Chunk(
            path=rel_path,
            label=label or symbol,
            start=first + 1,
            end=last + 1,
            text="\n".join(lines[first:last + 1]),
            note=note,
        )
    return None


def extract_file(rel_path: str, label: str = "", note: str = "") -> Chunk | None:
    """Whole file. Used for QuadTree.{h,cpp}, which are small enough that
    picking methods out of them would cost more context than it saves."""
    full = CORE_ROOT / rel_path
    try:
        src = full.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    lines = src.splitlines()
    return Chunk(rel_path, label or rel_path, 1, len(lines), "\n".join(lines), note)


def extract_constants(rel_path: str = "core/include/Config.h") -> Chunk | None:
    """Just the `constexpr` lines from Config.h.

    The full header is mostly prose explaining why the aggro radius is 320, which
    is excellent for a human reader and not what the agent needs. It needs the
    numbers, because REPULSION_RADIUS against WORLD_WIDTH is what decides how
    many neighbours a query returns, and that ratio is half of any honest
    argument about whether the tree pays for itself.
    """
    full = CORE_ROOT / rel_path
    try:
        src = full.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None

    kept = [ln.rstrip() for ln in src.splitlines() if "constexpr" in ln]
    if not kept:
        return None
    return Chunk(
        path=rel_path,
        label="Config constants (constexpr lines only)",
        start=0,
        end=0,
        text="namespace Config {\n" + "\n".join(kept) + "\n}",
        note="Extracted lines, not contiguous source; explanatory comments removed.",
    )


# ---------------------------------------------------------------------------
# What to send for a given configuration
# ---------------------------------------------------------------------------
#
# Priority is dropped-last-first when the bundle exceeds BUDGET_LINES. The
# ordering is by evidence value per line, not by how central the code feels:
# the constants are near the top because they are twenty lines that change how
# every other number is read, and EngineCore::tick is near the bottom because it
# is long and mostly tells the model things the telemetry already told it.

_ALWAYS = [
    (10, "constants", None),
    (30, "core/src/EngineCore.cpp", "EngineCore::fireAura"),
    (70, "core/src/EngineCore.cpp", "EngineCore::tick"),
]

_BY_COLLISION = {
    "BruteForce": [
        (0, "core/src/EngineCore.cpp", "EngineCore::updateRepulsionBruteForce"),
    ],
    "QuadTree": [
        (0,  "core/src/EngineCore.cpp", "EngineCore::updateRepulsionQuadTree"),
        (40, "core/src/QuadTree.cpp",   None),
        (50, "core/include/QuadTree.h", None),
    ],
}

_BY_MEMORY = {
    "SoA": [
        (20, "core/include/EnemySoA.h", None),
        (60, "core/src/EngineCore.cpp", "EnemyContainerSoA::update"),
    ],
    "AoS": [
        (20, "core/include/EnemyAoS.h", None),
        (60, "core/src/EngineCore.cpp", "EnemyContainerAoS::update"),
    ],
}

_NOTES = {
    "EngineCore::updateRepulsionBruteForce":
        "Contains BOTH memory layouts behind `if (memoryMode_ == MemoryMode::SoA)`.",
    "EngineCore::updateRepulsionQuadTree":
        "Contains BOTH memory layouts behind `if (memoryMode_ == MemoryMode::SoA)`.",
    "EngineCore::fireAura":
        "Fires every Config::AURA_INTERVAL (2.0 s); linear scan in both collision modes by design.",
}


def collect(collision_mode: str, memory_mode: str) -> list[Chunk]:
    """The source the agent should see for one running configuration."""
    plan = list(_ALWAYS)
    plan += _BY_COLLISION.get(collision_mode, [])
    plan += _BY_MEMORY.get(memory_mode, [])

    chunks: list[tuple[int, Chunk]] = []
    for prio, path, symbol in plan:
        if path == "constants":
            c = extract_constants()
        elif symbol:
            c = extract_symbol(path, symbol, note=_NOTES.get(symbol, ""))
        else:
            c = extract_file(path)
        if c is not None:
            chunks.append((prio, c))

    chunks.sort(key=lambda pc: pc[0])

    kept: list[Chunk] = []
    total = 0
    for _, c in chunks:
        n = c.text.count("\n") + 1
        if kept and total + n > BUDGET_LINES:
            continue
        kept.append(c)
        total += n
    return kept


def render(chunks: list[Chunk], collision_mode: str, memory_mode: str,
           spawn_distribution: str) -> str:
    """The bundle as it appears in the prompt.

    The header states the provenance explicitly. Without it the model has no way
    to tell this from the paraphrased snippet the endpoint used to receive, and
    the difference matters: against real source it may cite line numbers and
    propose a patch, against a paraphrase it should not.
    """
    if not chunks:
        return ""

    total = sum(c.text.count("\n") + 1 for c in chunks)
    head = [
        f"Active C++ source, read from the repository at analysis time "
        f"({collision_mode} / {memory_mode}, {spawn_distribution} world).",
        "This is the code as written, not an excerpt prepared for reading: file "
        "paths and line numbers are exact and may be cited in your answer.",
        f"{len(chunks)} regions, {total} lines.",
        "",
    ]
    body = "\n\n".join(c.render() for c in chunks)
    return "\n".join(head) + "```cpp\n" + body + "\n```"


def cited(chunks: list[Chunk]) -> list[str]:
    """`path:start-end` for each region, for the API response. Lets a caller show
    what was actually read instead of asserting that something was."""
    return [c.path if c.start == 0 else f"{c.path}:{c.start}-{c.end}" for c in chunks]


# ---------------------------------------------------------------------------
# Patch verification
# ---------------------------------------------------------------------------
#
# WHY AN ANCHORED REPLACEMENT AND NOT A UNIFIED DIFF
#
# Asking the model for a unified diff sounds like the obvious move -- it is the
# format patches come in -- but a diff's @@ hunk headers encode line numbers and
# line counts, and those are arithmetic the model has to get right about a file
# it is reading rather than editing. When it gets them wrong the diff still looks
# like a diff; it just fails to apply, and the failure surfaces at `git apply`
# long after the response was rendered and believed.
#
# An old_str/new_str pair carries no arithmetic. It is either present in the file
# exactly once or it is not, and the server can settle that question in a line of
# code before the response is ever returned. The real unified diff is then
# generated HERE, from the verified replacement, where the line numbers are
# computed rather than recalled.
#
# So the model is asked for the thing it is reliable at (quoting code it can see)
# and the server does the thing it is reliable at (counting lines).

import difflib
from dataclasses import field as _field


@dataclass(frozen=True)
class VerifiedPatch:
    """The result of checking a proposed edit against the file on disk.

    `applies` is the only field a caller should branch on. Everything else is
    there to explain a rejection to a human, because a patch that does not apply
    is a thing the demo should SAY, not hide -- an optimizer that silently drops
    its own failed suggestions looks more reliable than it is.
    """

    file:        str
    symbol:      str
    applies:     bool
    reason:      str
    diff:        str = ""
    occurrences: int = 0
    start_line:  int = 0


def allowed_files(collision_mode: str, memory_mode: str) -> set[str]:
    """Paths the agent is permitted to propose an edit to, for this run.

    Scoped to the configuration rather than to the whole repo: the model was
    shown these files and no others, so an edit to anything else is by
    definition not grounded in what it read. This is the same reasoning that
    validates the recommendation block against the engine's enums in main.py --
    a value that will be acted on gets checked against what actually exists,
    because an unvalidated path turns a malformed response into a filesystem
    write somewhere nobody is looking.
    """
    plan = list(_ALWAYS)
    plan += _BY_COLLISION.get(collision_mode, [])
    plan += _BY_MEMORY.get(memory_mode, [])
    return {path for _, path, _ in plan if path != "constants"}


def verify_patch(rel_path: str, old_str: str, new_str: str,
                 symbol: str = "",
                 collision_mode: str = "", memory_mode: str = "") -> VerifiedPatch:
    """Check that `old_str` identifies exactly one site in `rel_path`, and if so
    render the unified diff the replacement would produce."""
    def no(reason: str, **kw) -> VerifiedPatch:
        return VerifiedPatch(rel_path, symbol, False, reason, **kw)

    if not rel_path or not old_str:
        return no("the response did not name a file and an anchor to replace")

    if collision_mode or memory_mode:
        allowed = allowed_files(collision_mode, memory_mode)
        if rel_path not in allowed:
            return no(f"{rel_path} was not among the files shown for this "
                      f"configuration ({', '.join(sorted(allowed))}), so an edit "
                      f"to it is not grounded in the source that was read")

    # Reject traversal before touching the filesystem. CORE_ROOT is a real
    # directory and rel_path arrives from a model response, which is the exact
    # shape of input that should never be joined to a path unchecked.
    full = (CORE_ROOT / rel_path).resolve()
    try:
        full.relative_to(CORE_ROOT.resolve())
    except ValueError:
        return no(f"{rel_path} resolves outside the core tree")

    try:
        src = full.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        return no(f"{rel_path} could not be read ({e.__class__.__name__})")

    count = src.count(old_str)
    if count == 0:
        return no("the quoted original text does not appear in the file; the "
                  "model rewrote it rather than quoting it verbatim",
                  occurrences=0)
    if count > 1:
        return no(f"the quoted original text appears {count} times, so the edit "
                  f"site is ambiguous; it needs more surrounding context to be "
                  f"unique", occurrences=count)

    if old_str == new_str:
        return no("the replacement is identical to the original", occurrences=1)

    updated = src.replace(old_str, new_str, 1)
    start   = src.count("\n", 0, src.index(old_str)) + 1
    diff = "".join(difflib.unified_diff(
        src.splitlines(keepends=True),
        updated.splitlines(keepends=True),
        fromfile=f"a/{rel_path}",
        tofile=f"b/{rel_path}",
        n=3,
    ))
    return VerifiedPatch(rel_path, symbol, True, "applies cleanly",
                         diff=diff, occurrences=1, start_line=start)
