// ---------------------------------------------------------------------------
// PerfLogger -- 1 Hz structured telemetry for the AI optimizer agent.
//
// WHY THIS EXISTS, AND WHY IT LOOKS LIKE THIS
//
// The HUD already shows numbers, but a HUD number is a bad input to an
// optimizer. The agent's job is to pick a strategy -- swap the algorithm,
// change the layout, restructure the kernel -- and each of those has a
// different signature in the data:
//
//   * a mean alone cannot separate "steadily 12 ms" from "8 ms with a 40 ms
//     spike every second". The first wants a cheaper kernel; the second wants
//     the allocation or the tree rebuild that causes the spike removed. Ship a
//     mean and the agent has no way to tell which problem it is looking at, so
//     percentiles and max go in every record.
//
//   * the answer depends on N. "QuadTree wins" is true at 10,000 entities and
//     false at 500, where the tree build costs more than the pairs it skips.
//     So entity count travels with every sample, never as a single header
//     value that a later slider move would silently invalidate.
//
//   * a number is meaningless without knowing which code path produced it, so
//     the active collision and memory mode are stamped per record, and every
//     toggle is recorded as a marker with a timestamp. That is what lets the
//     agent attribute a step change in ms to the toggle that caused it instead
//     of guessing.
//
// Sampling is per frame; emission is 1 Hz. Per-frame emission would make the
// log a function of frame rate (fast machines produce more evidence than slow
// ones, which is backwards) and would drown the model in noise. One record per
// second, each summarising every frame in that second, keeps the series
// comparable across machines and small enough to paste into a prompt.
// ---------------------------------------------------------------------------

export type CollisionMode = 'BruteForce' | 'QuadTree';
export type MemoryMode    = 'AoS' | 'SoA';

/**
 * Spawn distribution -- the scenario axis, as opposed to the two mode axes.
 *
 * It belongs in the telemetry for the same reason entity count does, only more
 * so: collision mode and memory mode describe WHICH CODE ran, and distribution
 * describes WHAT IT RAN ON. A QuadTree frame time without it is unattributable,
 * because the same measurement supports opposite conclusions depending on how
 * the field was arranged -- and an agent asked to recommend a spatial structure
 * from a log that omits the spatial arrangement is being asked to guess.
 */
export type SpawnDistribution = 'Uniform' | 'Clustered';

/** Distribution of a per-frame cost over one 1 Hz window, in milliseconds. */
export interface Dist {
  mean: number;
  p50:  number;
  p95:  number;
  p99:  number;
  max:  number;
}

export interface PerfRecord {
  seq:       number;
  t:         number;   // seconds since the logger started
  wallClock: string;   // ISO 8601, so a log can be correlated with anything else

  fps:    number;      // rAF callbacks observed / wall time in the window
  frames: number;      // raw callback count, so fps can be sanity-checked

  /** C++ EngineCore::tick only -- the thing the optimizer can actually change. */
  simMs: Dist;
  /** tick + canvas draw. The gap between this and simMs is the renderer's share. */
  frameMs: Dist;

  entities:   number;  // alive at the end of the window
  slots:      number;  // container capacity == the population the respawner holds
  kills:      number;  // in this window
  killsTotal: number;  // since init
  auraPulses: number;  // in this window

  collisionMode: CollisionMode;
  memoryMode:    MemoryMode;
  distribution:  SpawnDistribution;

  /** Frames whose total cost exceeded the 60 FPS budget. The headline number. */
  longFrames: number;
  budgetMs:   number;
}

export type MarkerKind =
  | 'session'
  | 'collisionMode'
  | 'memoryMode'
  | 'entityCount'
  | 'distribution'
  | 'pause';

/**
 * A discontinuity in the series. Without these the agent sees frame time step
 * from 3 ms to 60 ms with no explanation and has to infer the cause from the
 * shape of the curve.
 */
export interface Marker {
  t:    number;
  kind: MarkerKind;
  from: string;
  to:   string;
}

export interface SessionMeta {
  /**
   * Identifies one continuous telemetry session, and is stamped on EVERY line
   * of the export rather than only on the meta line.
   *
   * That looks redundant inside a single file, and inside a single file it is.
   * It stops being redundant the moment the agent team concatenates two runs --
   * which is the entire reason a run_id column was asked for. A row that only
   * inherits its run from a header line cannot survive a `cat a.ndjson
   * b.ndjson`, and the failure is silent: the rows still parse, they just
   * quietly belong to the wrong run.
   *
   * Scoped to the PerfLogger instance, NOT to a toggle. Switching collision
   * mode mid-run is the single most valuable thing in this log -- it is a
   * controlled A/B on one machine -- so the two sides must share a run_id for
   * anyone to be able to pair them. reset() is what starts a new run.
   */
  runId:               string;
  startedAt:           string;
  userAgent:           string;
  hardwareConcurrency: number;
  /** Chrome-only; undefined elsewhere. Reported as-is rather than guessed. */
  deviceMemoryGB?:     number;
  budgetMs:            number;
  intervalMs:          number;
}

export interface TelemetrySession {
  meta:    SessionMeta;
  markers: Marker[];
  records: PerfRecord[];
}

export interface LoggerContext {
  collisionMode: CollisionMode;
  memoryMode:    MemoryMode;
  distribution:  SpawnDistribution;
  slots:         number;
  entities:      number;
  killsTotal:    number;
  auraPulses:    number;
}

interface Options {
  /** Emission period. 1 Hz per the spec. */
  intervalMs?: number;
  /** Ring-buffer depth. 600 records == 10 minutes, plenty for one demo run. */
  capacity?:   number;
  /** Per-frame budget a 60 FPS target implies. */
  budgetMs?:   number;
}

const DEFAULT_INTERVAL = 1000;
const DEFAULT_CAPACITY = 600;
const DEFAULT_BUDGET   = 1000 / 60;

/** Three decimals. Sub-microsecond digits are measurement noise, not signal,
 *  and they triple the size of the payload the model has to read. */
const r3 = (v: number) => Math.round(v * 1000) / 1000;

/** crypto.randomUUID needs a secure context, which `vite dev` over plain http
 *  on a LAN address is not. The fallback is not cryptographically anything --
 *  it only has to be unlikely to collide across a handful of demo runs. */
function newRunId(): string {
  // Cast rather than relying on lib.dom: randomUUID is absent from the Crypto
  // type in older TS lib versions, and a build that fails on a type technicality
  // is a worse outcome than a feature-detected call.
  const c = typeof crypto !== 'undefined'
    ? (crypto as Crypto & { randomUUID?: () => string })
    : undefined;
  if (c && typeof c.randomUUID === 'function') return c.randomUUID();
  return `run-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

/**
 * Nearest-rank percentiles over one window. Sorting ~60 numbers once a second
 * is free; the alternative -- a streaming estimator -- buys nothing at this
 * scale and would make the numbers approximate for no reason.
 */
function distOf(samples: number[]): Dist {
  const n = samples.length;
  if (n === 0) return { mean: 0, p50: 0, p95: 0, p99: 0, max: 0 };

  const a = samples.slice().sort((x, y) => x - y);
  let sum = 0;
  for (let i = 0; i < n; ++i) sum += a[i];

  const at = (q: number) => a[Math.min(n - 1, Math.max(0, Math.ceil(q * n) - 1))];
  return {
    mean: r3(sum / n),
    p50:  r3(at(0.50)),
    p95:  r3(at(0.95)),
    p99:  r3(at(0.99)),
    max:  r3(a[n - 1]),
  };
}

export class PerfLogger {
  private readonly intervalMs: number;
  private readonly capacity:   number;
  private readonly budgetMs:   number;

  private readonly meta:    SessionMeta;
  private readonly records: PerfRecord[] = [];
  private readonly markers: Marker[]     = [];

  // Per-window accumulators. Plain arrays reused across windows, because
  // allocating two fresh arrays a second would hand the GC work whose pauses
  // would then show up in the very frameMs series being recorded.
  private simSamples:   number[] = [];
  private frameSamples: number[] = [];
  private longFrames = 0;

  private t0        = 0;
  private windowT0  = 0;
  private seq       = 0;
  private started   = false;

  private killsAtWindowStart      = 0;
  private auraPulsesAtWindowStart = 0;

  private ctx: LoggerContext = {
    collisionMode: 'QuadTree',
    memoryMode:    'SoA',
    distribution:  'Uniform',
    slots:         0,
    entities:      0,
    killsTotal:    0,
    auraPulses:    0,
  };

  private listeners = new Set<(r: PerfRecord) => void>();

  constructor(opts: Options = {}) {
    this.intervalMs = opts.intervalMs ?? DEFAULT_INTERVAL;
    this.capacity   = opts.capacity   ?? DEFAULT_CAPACITY;
    this.budgetMs   = opts.budgetMs   ?? DEFAULT_BUDGET;

    const nav = typeof navigator !== 'undefined' ? navigator : undefined;
    this.meta = {
      runId:               newRunId(),
      startedAt:           new Date().toISOString(),
      userAgent:           nav?.userAgent ?? 'unknown',
      hardwareConcurrency: nav?.hardwareConcurrency ?? 0,
      deviceMemoryGB:      (nav as unknown as { deviceMemory?: number })?.deviceMemory,
      budgetMs:            r3(this.budgetMs),
      intervalMs:          this.intervalMs,
    };
  }

  /** Modes and counts the next record should be stamped with. Cheap; call it
   *  every frame rather than trying to keep a second copy in sync. */
  setContext(ctx: LoggerContext): void {
    this.ctx = ctx;
  }

  /** Record a discontinuity. Called by the toggles, not by the frame loop. */
  mark(kind: MarkerKind, from: string, to: string): void {
    this.markers.push({ t: r3(this.elapsed()), kind, from, to });
  }

  /**
   * Re-base the current window after a span in which no frames were sampled
   * (the app was paused, or the tab was backgrounded).
   *
   * Without this the pause would corrupt exactly one record, and in the most
   * misleading way available. sample() flushes when `now - windowT0` exceeds
   * the interval, so the FIRST frame after a ten-second pause satisfies that
   * condition with one frame in the buffer: the logger would emit a record
   * claiming 0.1 fps, a one-sample p95, and a kills count covering ten seconds
   * of wall time in which nothing moved. An optimizer agent reading that series
   * sees a catastrophic stall in whatever configuration happened to be active
   * and will attribute it to the algorithm.
   *
   * The partial pre-pause window is discarded rather than merged for the same
   * reason: its frames are real, but the elapsed time they would be divided by
   * is not.
   *
   * `t0` is deliberately NOT shifted. Record timestamps stay on the wall clock,
   * so the gap in `t` is visible in the series and lines up with the pause
   * marker that brackets it.
   */
  resumeAfterGap(nowMs: number = performance.now()): void {
    if (!this.started) return;
    this.simSamples.length       = 0;
    this.frameSamples.length     = 0;
    this.longFrames              = 0;
    this.windowT0                = nowMs;
    this.killsAtWindowStart      = this.ctx.killsTotal;
    this.auraPulsesAtWindowStart = this.ctx.auraPulses;
  }

  /**
   * One call per rAF callback, after the tick and the draw.
   *
   * @param nowMs   performance.now() at the end of the frame
   * @param simMs   raw cost of this tick, from EngineCore::getLastTickMs()
   * @param frameMs tick + draw, measured in JS
   */
  sample(nowMs: number, simMs: number, frameMs: number): void {
    if (!this.started) {
      this.started                 = true;
      this.t0                      = nowMs;
      this.windowT0                = nowMs;
      this.killsAtWindowStart      = this.ctx.killsTotal;
      this.auraPulsesAtWindowStart = this.ctx.auraPulses;
      this.markers.push({ t: 0, kind: 'session', from: '', to: 'start' });
    }

    this.simSamples.push(simMs);
    this.frameSamples.push(frameMs);
    if (frameMs > this.budgetMs) ++this.longFrames;

    if (nowMs - this.windowT0 >= this.intervalMs) this.flush(nowMs);
  }

  subscribe(fn: (r: PerfRecord) => void): () => void {
    this.listeners.add(fn);
    return () => { this.listeners.delete(fn); };
  }

  getRecords(): readonly PerfRecord[] { return this.records; }
  getMarkers(): readonly Marker[]     { return this.markers; }
  latest(): PerfRecord | undefined    { return this.records[this.records.length - 1]; }

  /** Full session, for download or for the optimizer payload. */
  toSession(): TelemetrySession {
    return { meta: this.meta, markers: [...this.markers], records: [...this.records] };
  }

  /**
   * The last `seconds` of history plus any marker inside that span.
   *
   * The optimizer gets a window, not the whole session: a five-minute log is
   * mostly redundant, and the tail is what describes the configuration the
   * user is actually looking at right now.
   */
  window(seconds = 30): TelemetrySession {
    // Derived from intervalMs rather than assuming one record per second, so
    // changing the emission rate cannot silently change the window length.
    const n       = Math.max(1, Math.ceil((seconds * 1000) / this.intervalMs));
    const records = this.records.slice(-n);
    const from    = records.length ? records[0].t : 0;
    return {
      meta:    this.meta,
      markers: this.markers.filter(m => m.t >= from),
      records,
    };
  }

  /** One JSON object per line: greppable, appendable, and streamable into a
   *  prompt without re-parsing a giant array.
   *
   *  Field names here follow the agent team's schema, NOT the internal
   *  PerfRecord names -- see toAgentRow() for why the translation lives at this
   *  boundary rather than in the record type. */
  toNdjson(): string {
    const runId = this.meta.runId;
    const lines = [
      JSON.stringify({ type: 'meta', ...toAgentMeta(this.meta) }),
      ...this.markers.map(m => JSON.stringify({ type: 'marker', run_id: runId, ...m })),
      ...this.records.map(r => JSON.stringify({ type: 'sample', ...toAgentRow(r, runId) })),
    ];
    return lines.join('\n') + '\n';
  }

  /** Starts a NEW run: the discarded records and the ones that follow did not
   *  come from the same continuous session, so they must not share a run_id. */
  reset(): void {
    this.meta.runId     = newRunId();
    this.meta.startedAt = new Date().toISOString();
    this.records.length      = 0;
    this.markers.length      = 0;
    this.simSamples.length   = 0;
    this.frameSamples.length = 0;
    this.longFrames          = 0;
    this.seq                 = 0;
    this.started             = false;
  }

  private elapsed(): number {
    return this.started ? (performance.now() - this.t0) / 1000 : 0;
  }

  private flush(nowMs: number): void {
    const elapsedMs = nowMs - this.windowT0;
    const frames    = this.frameSamples.length;

    const rec: PerfRecord = {
      seq:       this.seq++,
      t:         r3((nowMs - this.t0) / 1000),
      wallClock: new Date().toISOString(),

      // Measured, not derived from frame time: this counts callbacks the
      // browser actually delivered, so it sees dropped frames, throttled
      // background tabs and missed vsyncs that a 1000/ms figure cannot.
      fps:    elapsedMs > 0 ? r3((frames * 1000) / elapsedMs) : 0,
      frames,

      simMs:   distOf(this.simSamples),
      frameMs: distOf(this.frameSamples),

      entities:   this.ctx.entities,
      slots:      this.ctx.slots,
      kills:      this.ctx.killsTotal - this.killsAtWindowStart,
      killsTotal: this.ctx.killsTotal,
      auraPulses: this.ctx.auraPulses - this.auraPulsesAtWindowStart,

      collisionMode: this.ctx.collisionMode,
      memoryMode:    this.ctx.memoryMode,
      distribution:  this.ctx.distribution,

      longFrames: this.longFrames,
      budgetMs:   r3(this.budgetMs),
    };

    this.records.push(rec);
    if (this.records.length > this.capacity) this.records.shift();

    this.simSamples.length       = 0;
    this.frameSamples.length     = 0;
    this.longFrames              = 0;
    this.windowT0                = nowMs;
    this.killsAtWindowStart      = this.ctx.killsTotal;
    this.auraPulsesAtWindowStart = this.ctx.auraPulses;

    for (const fn of this.listeners) fn(rec);
  }
}

// ---------------------------------------------------------------------------
// Export shape: the optimizer-agent team's schema.
//
// The team's ingest expects {run_id, collision_mode, memory_layout,
// entity_count, sim_ms, frame_time_ms}. The logger internally calls three of
// those collisionMode, memoryMode and entities, so something has to translate.
//
// WHY THE TRANSLATION LIVES HERE AND NOT IN PerfRecord
//
// Renaming the fields on PerfRecord itself would have been fewer lines, and it
// would have rippled into MetricGraph, CodePanel, App's HUD and the fixed-width
// table in server/prompts.py -- four consumers that have nothing to do with the
// agent team's CSV, changed to satisfy a contract none of them participate in.
// Worse, it would make the export schema and the in-memory schema the same
// object, so the next time the team renames a column, every in-app consumer
// gets dragged along. One mapping function at the boundary keeps the blast
// radius at one file.
//
// A consequence worth stating out loud: the downloaded .ndjson is snake_case,
// while the POST body to /api/optimize (which goes through toSession(), not
// through here) is still camelCase, because server/prompts.py reads it with the
// internal names. Two consumers, two contracts, on purpose -- do NOT "fix" the
// inconsistency by renaming one side without changing its reader, because both
// sides read their payloads defensively with defaults and a rename would
// degrade to columns of zeros rather than to an error.
//
// WHY THE ROW IS FLAT
//
// simMs and frameMs are distributions internally, and a CSV cell cannot hold
// {mean, p50, p95, p99, max}. Their two requested columns therefore carry the
// window MEAN, which is the honest reading of a column named plainly "sim_ms".
// But shipping only the mean would delete the single most diagnostic fact in
// this log -- the gap between mean and p95 is what separates "uniformly
// expensive kernel" from "cheap kernel with a periodic stall", and those want
// opposite fixes. So the percentiles ride along as their own flat columns.
// A consumer that reads exactly the six contract fields gets exactly what it
// asked for; one that reads the whole row gets the shape too.
//
// WHY THE EXTRAS ARE ON THE SAME LINE RATHER THAN A SECOND RECORD TYPE
//
// They describe the same second of the same run. Splitting them into a parallel
// stream would mean any consumer wanting p95 has to join two line types on
// (run_id, seq) -- and a join is a thing that can be got wrong, whereas a wider
// row is not.
// ---------------------------------------------------------------------------

export interface AgentSampleRow {
  // --- the six columns the agent team's CSV ingest reads ---------------------
  run_id:         string;
  collision_mode: CollisionMode;
  memory_layout:  MemoryMode;
  entity_count:   number;
  /** Window mean of C++ EngineCore::tick. See sim_p95/p99/max for the shape. */
  sim_ms:         number;
  /** Window mean of tick + canvas draw. */
  frame_time_ms:  number;

  // --- everything the logger knows that their schema does not ask for -------
  seq:        number;
  t:          number;
  wall_clock: string;

  /** The scenario axis. Absent from their schema because it did not exist when
   *  the schema was written; it is the one field that can make the same
   *  collision_mode the right answer in one row and the wrong answer in the
   *  next, so it must not be dropped on the way out. */
  spawn_distribution: SpawnDistribution;

  fps:    number;
  frames: number;

  sim_p50: number; sim_p95: number; sim_p99: number; sim_max: number;
  frame_p50: number; frame_p95: number; frame_p99: number; frame_max: number;

  slots:       number;
  kills:       number;
  kills_total: number;
  aura_pulses: number;

  /** Frames over the 60 FPS budget, and the budget they were compared against
   *  -- shipped together because the count is meaningless without it. */
  long_frames: number;
  budget_ms:   number;
}

/** One PerfRecord as the agent team's ingest expects to see it. Contract
 *  columns are emitted first so `head -1 file.ndjson | jq keys_unsorted` shows
 *  them up front. */
export function toAgentRow(r: PerfRecord, runId: string): AgentSampleRow {
  return {
    run_id:         runId,
    collision_mode: r.collisionMode,
    memory_layout:  r.memoryMode,
    entity_count:   r.entities,
    sim_ms:         r.simMs.mean,
    frame_time_ms:  r.frameMs.mean,

    seq:        r.seq,
    t:          r.t,
    wall_clock: r.wallClock,

    spawn_distribution: r.distribution,

    fps:    r.fps,
    frames: r.frames,

    sim_p50: r.simMs.p50, sim_p95: r.simMs.p95, sim_p99: r.simMs.p99, sim_max: r.simMs.max,
    frame_p50: r.frameMs.p50, frame_p95: r.frameMs.p95,
    frame_p99: r.frameMs.p99, frame_max: r.frameMs.max,

    slots:       r.slots,
    kills:       r.kills,
    kills_total: r.killsTotal,
    aura_pulses: r.auraPulses,

    long_frames: r.longFrames,
    budget_ms:   r.budgetMs,
  };
}

/** Session meta in the same snake_case as the sample rows. Mixing conventions
 *  within one file is how a consumer ends up reading `meta.budget_ms` as
 *  undefined and silently comparing frame times against zero. */
export function toAgentMeta(m: SessionMeta): Record<string, unknown> {
  return {
    run_id:               m.runId,
    started_at:           m.startedAt,
    user_agent:           m.userAgent,
    hardware_concurrency: m.hardwareConcurrency,
    device_memory_gb:     m.deviceMemoryGB,
    budget_ms:            m.budgetMs,
    interval_ms:          m.intervalMs,
  };
}

/** Trigger a browser download of the full session as NDJSON. */
export function downloadNdjson(logger: PerfLogger, filename?: string): void {
  // run_id in the filename, not just inside the file: the agent team will have
  // several of these in a downloads folder and needs to tell them apart without
  // opening each one. Truncated to the UUID's first block, which is plenty to
  // disambiguate a handful of runs and keeps the name readable.
  const run  = logger.toSession().meta.runId.split('-')[0];
  const name = filename ?? `vampire-core-perf-${run}-${Date.now()}.ndjson`;
  const blob = new Blob([logger.toNdjson()], { type: 'application/x-ndjson' });
  const url  = URL.createObjectURL(blob);
  const a    = document.createElement('a');
  a.href = url;
  a.download = name;
  a.click();
  // Revoking immediately can race the download on some browsers; one frame of
  // slack is enough and the object is tiny.
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
