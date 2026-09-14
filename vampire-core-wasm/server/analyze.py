"""Deterministic analysis of the telemetry window, computed before the prompt.

WHY THIS EXISTS

The table in prompts.py is thirty rows wide enough to need column alignment, and
the system prompt asks the model to find trends in it: compare across markers,
notice when p99 pulls away from the mean, check whether the renderer is the real
cost. Those are arithmetic, and arithmetic is the one part of this job a language
model does least reliably and a server does for free. Worse, when it gets a sum
slightly wrong the answer still reads like a diagnosis -- there is no error, just
a confident paragraph built on a number nobody computed.

So the server computes the findings and states them above the table. The table
stays, because a finding the model cannot check against the rows is a finding it
has to take on faith, and because the shape of a series carries things no fixed
set of statistics will catch.

WHAT IS DELIBERATELY NOT DONE HERE

No conclusions. Every function in this file reports a measurement and the sample
count behind it; none of them says which mode to use. The division is the point:
the server owns what is true about the series, the model owns what it means for
the code. A findings block that said "switch to BruteForce" would be a heuristic
masquerading as evidence, and the first time the heuristic was wrong it would be
wrong in the prompt, where the model would have no way to tell.

ASCII only, per the note at the top of prompts.py.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

# Below this many samples on either side, a comparison is reported but labelled.
# Three seconds is not a benchmark; it is, however, often all a demo run has, and
# suppressing it entirely would hide the only comparison in the window.
MIN_CONFIDENT_SAMPLES = 4

# p99/mean above this is called out as a spike shape rather than a uniform cost.
# 2.0 is deliberately low: at 60 Hz a window holds ~60 frames, so p99 is roughly
# the worst frame, and a worst frame at twice the mean is already the difference
# between a smooth run and a visible hitch.
SPIKE_RATIO = 2.0


@dataclass(frozen=True)
class Finding:
    label: str
    text:  str

    def render(self) -> str:
        return f"- {self.label}: {self.text}"


def _sim(r: dict[str, Any]) -> dict[str, Any]:
    return r.get("simMs") or {}


def _frm(r: dict[str, Any]) -> dict[str, Any]:
    return r.get("frameMs") or {}


def _n(count: int, noun: str = "sample") -> str:
    """'1 sample' / '4 samples'. Trivial, but this text goes into a prompt and
    'over 1 samples' is the kind of seam that makes generated context read as
    generated."""
    return f"{count} {noun}" + ("" if count == 1 else "s")


def _mean(vals: list[float]) -> float:
    return sum(vals) / len(vals) if vals else 0.0


def _tag(r: dict[str, Any]) -> str:
    c = "QT" if r.get("collisionMode") == "QuadTree" else "BF"
    d = "C" if r.get("distribution") == "Clustered" else "U"
    return f"{c}/{r.get('memoryMode', '?')}/{d}"


def _pct(before: float, after: float) -> str:
    """Relative change, worded so the direction cannot be misread.

    Percent change on a near-zero baseline is noise amplified into a headline --
    'sim mean 0.01 -> 0.40 ms (+3900%)' is arithmetically correct and completely
    misleading -- so below a tenth of a millisecond the absolute numbers are left
    to speak for themselves.
    """
    if before < 0.1:
        return "baseline too small for a meaningful ratio"
    delta = (after - before) / before * 100.0
    return f"{delta:+.0f}%"


# ---------------------------------------------------------------------------
# Segments: runs of consecutive records sharing one configuration
# ---------------------------------------------------------------------------

def _key(r: dict[str, Any]) -> tuple:
    return (r.get("collisionMode"), r.get("memoryMode"), r.get("distribution"))


def segments(records: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    """Split into consecutive same-configuration runs.

    Consecutive, not grouped: a run that toggles to BruteForce and back produces
    two separate QuadTree segments, and merging them would average across a span
    in which something else was running. They are different experiments that
    happen to share a label.
    """
    out: list[list[dict[str, Any]]] = []
    for r in records:
        if out and _key(out[-1][-1]) == _key(r):
            out[-1].append(r)
        else:
            out.append([r])
    return out


# ---------------------------------------------------------------------------
# Findings
# ---------------------------------------------------------------------------

def compare_markers(records: list[dict[str, Any]],
                    markers: list[dict[str, Any]]) -> list[Finding]:
    """A/B the seconds either side of each toggle.

    This is the strongest evidence in the log -- same machine, same build,
    seconds apart -- and it is also the easiest thing to get subtly wrong, in
    three ways this function handles explicitly:

    1. The record whose window straddles the marker contains frames from both
       configurations. It is excluded rather than assigned to a side, because
       assigning it puts a blend of both modes into one of the two columns and
       biases the comparison toward whichever side is faster.
    2. The span either side is bounded by the ADJACENT markers, not by the whole
       window. Reaching past a second toggle would compare A against a mixture
       of B and C while labelling the result A-vs-B.
    3. A toggle that coincides with a change in entity count or distribution is
       not a controlled comparison at all. It is still reported -- it may be the
       only thing in the window -- but the confound is named in the same
       sentence, so the model cannot read it as clean evidence.
    """
    usable = [m for m in markers if m.get("kind") not in ("session", "pause")]
    if not usable or not records:
        return []

    # Pause markers bound a span even though they are not compared across: the
    # frames after a pause belong to a different stretch of wall time, and the
    # logger has already thrown away the partial window.
    bounds = sorted(m.get("t", 0.0) for m in markers if m.get("kind") != "session")

    # Toggles that land in the same instant are ONE experiment, not several.
    # Emitting a finding per marker would report the same two spans two or three
    # times with different labels, and each copy would name the others as
    # confounds -- so a simultaneous mode+count change would read as three
    # independent pieces of evidence that all happen to disagree with each other.
    by_t: dict[float, list[dict[str, Any]]] = {}
    for m in usable:
        by_t.setdefault(round(m.get("t", 0.0), 3), []).append(m)

    findings: list[Finding] = []
    for t in sorted(by_t):
        group = by_t[t]
        kinds = {g.get("kind") for g in group}
        treatment = "; ".join(
            f"{g.get('kind')} {g.get('from') or '(start)'} -> {g.get('to')}"
            for g in group)
        lo = max([b for b in bounds if b < t], default=float("-inf"))
        hi = min([b for b in bounds if b > t], default=float("inf"))

        before = [r for r in records if lo < r.get("t", 0.0) <= t]
        after  = [r for r in records if t < r.get("t", 0.0) < hi]
        # Drop the straddling sample: its window opened before the toggle.
        if after:
            after = after[1:]

        if not before or not after:
            findings.append(Finding(
                f"Toggle at t={t:.1f}",
                f"{treatment}; no usable samples on "
                f"{'both sides' if not before and not after else 'one side'} "
                f"of it in this window, so it cannot be compared."))
            continue

        b_mean = _mean([_sim(r).get("mean", 0.0) for r in before])
        a_mean = _mean([_sim(r).get("mean", 0.0) for r in after])
        b_p95  = _mean([_sim(r).get("p95", 0.0) for r in before])
        a_p95  = _mean([_sim(r).get("p95", 0.0) for r in after])
        b_fps  = _mean([r.get("fps", 0.0) for r in before])
        a_fps  = _mean([r.get("fps", 0.0) for r in after])

        # Confounds, named rather than silently averaged over.
        notes: list[str] = []
        b_n = {r.get("entities") for r in before}
        a_n = {r.get("entities") for r in after}
        # An axis named in the treatment is what is being tested; only an
        # UNANNOUNCED change in it is a confound. Calling the deliberate variable
        # a confound would label every entity-count sweep as uncontrolled, which
        # is the opposite of what it is.
        if len(b_n | a_n) > 1 and "entityCount" not in kinds:
            notes.append(f"entity count moved ({sorted(b_n)} -> {sorted(a_n)}) "
                         f"without a marker, so this is not a controlled comparison")
        b_d = {r.get("distribution") for r in before}
        a_d = {r.get("distribution") for r in after}
        if len(b_d | a_d) > 1 and "distribution" not in kinds:
            notes.append("spawn distribution also changed across this span")
        if len(kinds) > 1:
            notes.append(f"{len(kinds)} axes changed at once ({', '.join(sorted(kinds))}), "
                         f"so the effect cannot be attributed to any one of them")
        if min(len(before), len(after)) < MIN_CONFIDENT_SAMPLES:
            notes.append(f"only {len(before)}/{len(after)} samples either side")

        held = ""
        if not notes:
            n = next(iter(b_n), "?")
            held = f"N held at {n}, distribution held at {next(iter(b_d), '?')}; "

        text = (f"{treatment}. "
                f"{held}"
                f"sim mean {b_mean:.2f} -> {a_mean:.2f} ms ({_pct(b_mean, a_mean)}), "
                f"sim p95 {b_p95:.2f} -> {a_p95:.2f} ms, "
                f"fps {b_fps:.1f} -> {a_fps:.1f}. "
                f"{_n(len(before))} before, {len(after)} after "
                f"(the sample straddling the toggle is excluded).")
        if notes:
            text += " CAVEAT: " + "; ".join(notes) + "."
        findings.append(Finding(f"Comparison at t={t:.1f}", text))

    return findings


def spike_shape(records: list[dict[str, Any]]) -> list[Finding]:
    """Per segment, how far the tail sits above the mean.

    Reported per segment rather than over the whole window because a window that
    spans a toggle has two populations in it, and the ratio of a bimodal mixture
    describes neither of them.
    """
    findings: list[Finding] = []
    for seg in segments(records):
        means = [_sim(r).get("mean", 0.0) for r in seg]
        mean  = _mean(means)
        if mean <= 0.0:
            continue
        p99 = _mean([_sim(r).get("p99", 0.0) for r in seg])
        mx  = _mean([_sim(r).get("max", 0.0) for r in seg])
        ratio = p99 / mean
        verdict = ("tail well above the mean, consistent with periodic stalls "
                   "rather than a uniformly expensive kernel"
                   if ratio >= SPIKE_RATIO else
                   "tail close to the mean, consistent with a uniformly "
                   "expensive kernel rather than periodic stalls")
        findings.append(Finding(
            f"Spike shape, {_tag(seg[0])} at N={seg[0].get('entities', 0)}",
            f"sim mean {mean:.2f} ms, p99 {p99:.2f} ms ({ratio:.2f}x mean), "
            f"max {mx:.2f} ms ({mx / mean:.2f}x mean) over {_n(len(seg))}. "
            f"{verdict[0].upper()}{verdict[1:]}."))
    return findings


def aura_correlation(records: list[dict[str, Any]]) -> list[Finding]:
    """Does the worst frame of a second get worse when the aura fired in it?

    The aura is the one cost in the tick with a known period (2.0 s), so it is
    the one suspect that can be tested rather than argued about. Windows are 1 s,
    so roughly half of them contain a pulse; if sim max is systematically higher
    in the ones that do, the pulse is the stall. If it is not, the system prompt's
    standing suspicion of the aura is wrong for this run and should be dropped --
    which is worth saying, because an unfalsified suspect gets blamed.

    Computed PER SEGMENT. Pooling the window would compare pulse-bearing seconds
    of one configuration against pulse-free seconds of another, so a toggle whose
    timing happened to align with the 2 s pulse parity would manufacture a
    correlation out of the mode change -- and the resulting number would be
    largest exactly when the two costs are most different, which is when it is
    most likely to be believed.
    """
    findings: list[Finding] = []
    for seg in segments(records):
        with_p    = [r for r in seg if (r.get("auraPulses") or 0) > 0]
        without_p = [r for r in seg if (r.get("auraPulses") or 0) == 0]
        if len(with_p) < 2 or len(without_p) < 2:
            continue

        a = _mean([_sim(r).get("max", 0.0) for r in with_p])
        b = _mean([_sim(r).get("max", 0.0) for r in without_p])
        if b <= 0.0:
            continue

        ratio = a / b
        verdict = ("the aura pulse accounts for the spikes"
                   if ratio >= 1.25 else
                   "the spikes are NOT explained by the aura pulse; look at the "
                   "per-tick tree rebuild or an allocation in the hot loop instead")
        findings.append(Finding(
            f"Aura correlation, {_tag(seg[0])}",
            f"sim max averages {a:.2f} ms in the {len(with_p)} windows containing "
            f"an aura pulse vs {b:.2f} ms in the {len(without_p)} without "
            f"({ratio:.2f}x). On this evidence {verdict}."))
    return findings


def renderer_share(records: list[dict[str, Any]]) -> list[Finding]:
    """How much of the frame is not simulation.

    The system prompt already tells the model to say so plainly when the renderer
    dominates. Computing it removes the judgement call: 'frame p95 far above sim
    p95' is a phrase, and this is a number.

    Per segment, for the same reason as everything else here: the renderer's cost
    is roughly constant while the sim's is not, so pooling a cheap configuration
    with an expensive one reports a share that was never true in either. Across a
    QuadTree-to-BruteForce toggle the pooled figure understates the renderer's
    share before the toggle and overstates the sim's after it.
    """
    findings: list[Finding] = []
    for seg in segments(records):
        sim = _mean([_sim(r).get("mean", 0.0) for r in seg])
        frm = _mean([_frm(r).get("mean", 0.0) for r in seg])
        if frm <= 0.0:
            # frameMs.mean is not in the prompt table, only p95, so a window can
            # legitimately arrive without it. Saying nothing beats guessing.
            continue
        gap = frm - sim
        share = gap / frm * 100.0
        verdict = ("the simulation is the minority of frame cost; C++ optimisation "
                   "has limited headroom here"
                   if share >= 50.0 else
                   "the simulation dominates frame cost")
        findings.append(Finding(
            f"Renderer share, {_tag(seg[0])}",
            f"frame mean {frm:.2f} ms vs sim mean {sim:.2f} ms, so {gap:.2f} ms "
            f"({share:.0f}%) is draw and browser overhead. On this evidence "
            f"{verdict}."))
    return findings


def budget_pressure(records: list[dict[str, Any]]) -> list[Finding]:
    """The headline number: how often the 60 FPS budget was actually missed."""
    frames = sum(r.get("frames", 0) for r in records)
    over   = sum(r.get("longFrames", 0) for r in records)
    if frames <= 0:
        return []
    return [Finding(
        "Budget",
        f"{over} of {frames} frames exceeded the budget ({over / frames * 100:.1f}%) "
        f"across {_n(len(records))}.")]


def analyze(telemetry: dict[str, Any] | None) -> list[Finding]:
    records = (telemetry or {}).get("records") or []
    markers = (telemetry or {}).get("markers") or []
    if not records:
        return []

    findings: list[Finding] = []
    findings += compare_markers(records, markers)

    segs = segments(records)
    if len(segs) == 1:
        # Said out loud. A single-configuration window is the common case when a
        # user hits Analyze without toggling anything, and the system prompt asks
        # the model to compare across a marker -- so without this line it is being
        # told to do something the data does not support, and a model asked for a
        # comparison will find one.
        s = segs[0]
        findings.append(Finding(
            "No controlled comparison",
            f"every sample in this window ran one configuration "
            f"({_tag(s[0])} at N={s[0].get('entities', 0)}), so nothing here is an "
            f"A/B. Any claim that one mode beats another is an argument from the "
            f"code, not from this series; report it at low confidence and say so."))

    findings += spike_shape(records)
    findings += aura_correlation(records)
    findings += renderer_share(records)
    findings += budget_pressure(records)
    return findings


def render(findings: list[Finding]) -> str:
    if not findings:
        return ""
    head = ("Computed findings (arithmetic done by the server over the series "
            "below, not estimated by eye). These are measurements, not "
            "conclusions -- the diagnosis is yours:")
    return head + "\n" + "\n".join(f.render() for f in findings)
