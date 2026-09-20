import { useEffect, useRef, useState, useCallback } from 'react';
import GameCanvas from './components/GameCanvas';
import MetricGraph from './components/MetricGraph';
import CodePanel from './components/CodePanel';
import AgentExperimentPanel from './components/AgentExperimentPanel';
import { PerfLogger, downloadNdjson } from './logging/PerfLogger';
import type { PerfRecord } from './logging/PerfLogger';

export type CollisionMode = 'BruteForce' | 'QuadTree' | 'UniformGrid' | 'SpatialHash';
export type MemoryMode    = 'AoS' | 'SoA';

// The scenario axis. Collision and Memory choose which code runs; this chooses
// what it runs on -- and it is the only one of the three that can make the
// QuadTree the wrong answer.
export type SpawnDist = 'Uniform' | 'Clustered';

export interface PerfSample {
  time:        number;
  fps:         number;   // browser frame rate, measured in JS
  simMs:       number;   // C++ tick cost, measured in the engine
  frameMs:     number;   // total JS frame budget used
  entities:    number;
  kills:       number;   // cumulative, since init
}

export interface WasmModule {
  EngineCore:    new () => EngineHandle;
  CollisionMode: { BruteForce: number; QuadTree: number; UniformGrid: number; SpatialHash: number };
  MemoryMode:    { AoS: number; SoA: number };
  SpawnDistribution: { Uniform: number; Clustered: number };
  HEAPF32:       Float32Array;
}

export interface EngineStats {
  fps:               number;
  frameTimeMs:       number;
  lastTickMs:        number;
  aliveEnemies:      number;
  activeProjectiles: number;
  kills:             number;
  auraPulses:        number;
}

export interface EngineHandle {
  init:             (n: number) => void;
  tick:             (dt: number) => void;
  setCollisionMode: (m: number) => void;
  setMemoryMode:    (m: number) => void;
  setEnemyCount:    (n: number) => void;
  /** Rebuilds the world -- home points are per-entity state, so this is a
   *  reset, not a live parameter change. The canvas re-reads the heap pointers
   *  every frame, so the reallocation it causes is safe mid-run. */
  setSpawnDistribution: (d: number) => void;
  setPlayerInput:   (dx: number, dy: number) => void;
  getEntityCount:   () => number;
  getPosXPtr:       () => number;
  getPosYPtr:       () => number;
  getPlayerX:       () => number;
  getPlayerY:       () => number;
  getAuraRadius:    () => number;
  getAuraPhase:     () => number;
  getAuraFlash:     () => number;
  getKills:         () => number;
  getAliveCount:    () => number;
  /** Raw cost of the tick that just ran, unsmoothed. Cheap scalar call --
   *  safe to read every frame, unlike getStats() which marshals an object. */
  getLastTickMs:    () => number;
  getStats:         () => EngineStats;
}

declare global {
  interface Window { CoreEngineModule?: () => Promise<WasmModule>; }
}

const HISTORY_MAX = 120;

// The HUD and the charts update at this rate, NOT at frame rate.
//
// The previous version called setHistory() inside the rAF callback, so every
// single frame re-rendered the whole React tree and both recharts SVGs. On a
// 10,000-entity run that React work cost more than the C++ simulation it was
// supposed to be measuring -- the profiler HUD was the bottleneck it claimed
// to be profiling. 5 Hz is well past what a human reads off a number and
// costs essentially nothing.
const STATS_HZ       = 5;
const STATS_INTERVAL = 1000 / STATS_HZ;

export default function App() {
  const [collisionMode, setCollisionMode] = useState<CollisionMode>('QuadTree');
  const [memoryMode,    setMemoryMode]    = useState<MemoryMode>('SoA');
  const [spawnDist,     setSpawnDist]     = useState<SpawnDist>('Uniform');
  const [enemyCount,    setEnemyCount]    = useState(5000);
  const [enemyDraft,    setEnemyDraft]    = useState(5000);
  const [speedMultiplier, setSpeedMultiplier] = useState(1);
  const [safetyNotice, setSafetyNotice] = useState('');
  const [history,       setHistory]       = useState<PerfSample[]>([]);
  const [wasmReady,     setWasmReady]     = useState(false);
  const [logRecord,     setLogRecord]     = useState<PerfRecord | null>(null);
  const [logCount,      setLogCount]      = useState(0);
  const [paused,        setPaused]        = useState(false);

  const engineRef    = useRef<EngineHandle | null>(null);
  const moduleRef    = useRef<WasmModule | null>(null);
  const canvasApiRef = useRef<{ draw: () => void } | null>(null);
  const keysRef      = useRef<Set<string>>(new Set());
  const rafRef       = useRef<number>(0);
  const lastTimeRef  = useRef<number>(0);

  // The rAF effect only depends on [wasmReady], so its closure would capture
  // whatever modes were active when it was created and stamp every telemetry
  // record with them forever. A ref is the only thing the loop can read that
  // the toggles can also write.
  const modeRef = useRef({
    collision: 'QuadTree' as CollisionMode,
    memory:    'SoA'      as MemoryMode,
    dist:      'Uniform'  as SpawnDist,
    speed:     1,
  });

  // Same reason as modeRef: the loop's closure is built once, at [wasmReady],
  // and would capture paused === false forever. Tearing the loop down and
  // rebuilding it on every pause would work but throws away lastTimeRef and the
  // stats window on each toggle, so the ref is both cheaper and more correct.
  const pausedRef = useRef(false);

  // One logger for the whole session. Created lazily rather than as a field
  // initialiser so React's double-invoked render in StrictMode does not
  // silently throw away a logger that already has history in it.
  const loggerRef = useRef<PerfLogger | null>(null);
  if (loggerRef.current === null) loggerRef.current = new PerfLogger();

  // Frame accounting lives in refs so accumulating it never triggers a render.
  const frameCountRef  = useRef(0);
  const frameMsAccRef  = useRef(0);
  const lastStatsRef   = useRef(0);

  useEffect(() => {
    if (!window.CoreEngineModule) return;
    window.CoreEngineModule().then(mod => {
      moduleRef.current = mod;
      const engine = new mod.EngineCore();
      engine.init(enemyCount);
      engineRef.current = engine;
      setWasmReady(true);
    });
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  // Surface each 1 Hz record. This is a 1 Hz setState, not a per-frame one, so
  // it costs nothing next to the simulation.
  useEffect(() => {
    const logger = loggerRef.current!;
    return logger.subscribe(rec => {
      setLogRecord(rec);
      setLogCount(logger.getRecords().length);
    });
  }, []);

  useEffect(() => {
    const onDown = (e: KeyboardEvent) => keysRef.current.add(e.key);
    const onUp   = (e: KeyboardEvent) => keysRef.current.delete(e.key);
    window.addEventListener('keydown', onDown);
    window.addEventListener('keyup',   onUp);
    return () => { window.removeEventListener('keydown', onDown); window.removeEventListener('keyup', onUp); };
  }, []);

  // ONE requestAnimationFrame loop for the whole app.
  //
  // Simulation and rendering used to run in two independent rAF loops, which
  // meant the canvas could draw a half-updated world, and the browser had two
  // callbacks competing for the same frame budget. Ticking and drawing in a
  // fixed order in a single callback removes both problems.
  useEffect(() => {
    if (!wasmReady) return;
    const logger = loggerRef.current!;

    const loop = (ts: number) => {
      const frameStart = performance.now();
      const dt = lastTimeRef.current ? Math.min((ts - lastTimeRef.current) / 1000, 0.05) : 0.016;
      lastTimeRef.current = ts;

      // Paused: keep the rAF callback alive so resuming is instant, but do not
      // tick, draw, or sample.
      //
      // lastTimeRef is still advanced above, so the first frame after resume
      // gets a one-frame dt rather than the whole paused span. The 0.05 clamp
      // would have hidden that anyway -- but by silently simulating a 50 ms
      // step, which at 5,000 entities is a visible teleport of the whole field.
      //
      // Nothing is drawn either. The canvas keeps its last frame (its width and
      // height attributes are static, so React re-renders never clear it), and
      // that frame is exactly the state after the last tick -- so the paused
      // picture is the true one rather than a re-render of it.
      if (pausedRef.current) {
        rafRef.current = requestAnimationFrame(loop);
        return;
      }

      const engine = engineRef.current;
      if (engine) {
        let dx = 0, dy = 0;
        const k = keysRef.current;
        if (k.has('ArrowLeft')  || k.has('a') || k.has('A')) dx -= 1;
        if (k.has('ArrowRight') || k.has('d') || k.has('D')) dx += 1;
        if (k.has('ArrowUp')    || k.has('w') || k.has('W')) dy -= 1;
        if (k.has('ArrowDown')  || k.has('s') || k.has('S')) dy += 1;
        engine.setPlayerInput(dx, dy);

        engine.tick(dt * modeRef.current.speed);
        canvasApiRef.current?.draw();

        const frameEnd = performance.now();
        const frameMs  = frameEnd - frameStart;

        frameCountRef.current += 1;
        frameMsAccRef.current += frameMs;

        // Every frame feeds the logger; the logger itself emits at 1 Hz. Only
        // two cheap scalar reads happen here -- getStats() marshals a whole
        // object across the embind boundary and is deliberately left to the
        // 5 Hz block below.
        logger.sample(frameEnd, engine.getLastTickMs(), frameMs);

        if (ts - lastStatsRef.current >= STATS_INTERVAL) {
          const elapsed = ts - lastStatsRef.current;
          const frames  = frameCountRef.current;
          const s       = engine.getStats();

          // At most 200 ms stale by the time the logger flushes at 1 Hz, which
          // is irrelevant for counters that describe a whole second anyway.
          logger.setContext({
            collisionMode: modeRef.current.collision,
            memoryMode:    modeRef.current.memory,
            distribution:  modeRef.current.dist,
            speedMultiplier: modeRef.current.speed,
            slots:         engine.getEntityCount(),
            entities:      s.aliveEnemies,
            killsTotal:    s.kills,
            auraPulses:    s.auraPulses,
          });

          // Real frame rate comes from counting rAF callbacks against the wall
          // clock. The engine's own number is simulation headroom and cannot
          // see the canvas, the browser, or a missed vsync.
          const sample: PerfSample = {
            time:     ts,
            fps:      frames > 0 ? (frames * 1000) / elapsed : 0,
            simMs:    s.frameTimeMs,
            frameMs:  frames > 0 ? frameMsAccRef.current / frames : 0,
            entities: s.aliveEnemies,
            kills:    s.kills,
          };

          lastStatsRef.current  = ts;
          frameCountRef.current = 0;
          frameMsAccRef.current = 0;

          setHistory(prev => {
            const next = [...prev, sample];
            return next.length > HISTORY_MAX ? next.slice(-HISTORY_MAX) : next;
          });
        }
      }
      rafRef.current = requestAnimationFrame(loop);
    };

    lastTimeRef.current  = 0;
    lastStatsRef.current = performance.now();
    rafRef.current = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(rafRef.current);
  }, [wasmReady]);

  // Re-base the 5 Hz accumulators. Split out of resetHistory because
  // pause/resume needs exactly this and nothing more: fps here is frames
  // divided by wall time, and the paused span is wall time containing no
  // frames, so the first post-resume sample would otherwise report a frame rate
  // that no configuration ever ran at.
  const resetStatsWindow = useCallback(() => {
    frameCountRef.current = 0;
    frameMsAccRef.current = 0;
    lastStatsRef.current  = performance.now();
  }, []);

  // Toggling a mode also clears the chart, so it shows the new regime
  // immediately instead of a 120-sample tail of the old one. Pause does NOT do
  // this -- the configuration did not change, so the samples on either side of
  // a pause belong to the same series.
  //
  // The telemetry log is deliberately NOT cleared by either: the optimizer's
  // most useful evidence is the before/after pair around a toggle, and a marker
  // records exactly where the boundary is.
  const resetHistory = useCallback(() => {
    setHistory([]);
    resetStatsWindow();
  }, [resetStatsWindow]);

  // Pause is a demo affordance first -- freeze a 10,000-entity field and point
  // at it -- but it is also the only way to read the HUD while the numbers are
  // not moving. It must not fabricate telemetry to do either: see
  // PerfLogger.resumeAfterGap().
  //
  // pausedRef, not the `paused` state, is the source of truth here. A
  // functional setPaused(prev => ...) would be the idiomatic way to avoid a
  // stale closure, but React invokes that updater twice under StrictMode -- and
  // it is the only place the marker could be pushed from, so the log would grow
  // two pause markers per click and resumeAfterGap() would run twice. The ref
  // is written exactly once per click.
  const togglePause = useCallback(() => {
    const next = !pausedRef.current;
    pausedRef.current = next;
    setPaused(next);

    const logger = loggerRef.current!;
    logger.mark('pause', next ? 'running' : 'paused', next ? 'paused' : 'running');
    if (!next) {
      logger.resumeAfterGap();
      resetStatsWindow();
    }
  }, [resetStatsWindow]);

  // Space / P. Deliberately a separate listener from the movement keys, which
  // only maintain a Set and must stay allocation-free.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.repeat) return;                       // holding the key is one toggle
      if (e.code !== 'Space' && e.key !== 'p' && e.key !== 'P') return;
      // Space scrolls the page by default, and if the Pause button still holds
      // focus from an earlier click the browser also synthesises a click on it
      // -- so the toggle would fire twice and appear to do nothing at all.
      e.preventDefault();
      togglePause();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [togglePause]);

  const changeCollision = useCallback((next: CollisionMode) => {
    if (next === collisionMode) return;
    if (next === 'BruteForce' && enemyCount > 10000) {
      setSafetyNotice('BruteForce는 10,000명까지 선택할 수 있습니다. 100,000명 실험은 공간 분할 모드를 사용하세요.');
      return;
    }
    setSafetyNotice('');
    setCollisionMode(next);
    modeRef.current.collision = next;
    loggerRef.current!.mark('collisionMode', collisionMode, next);
    const mod = moduleRef.current;
    if (engineRef.current && mod)
      engineRef.current.setCollisionMode(mod.CollisionMode[next]);
    resetHistory();
  }, [collisionMode, enemyCount, resetHistory]);

  const toggleMemory = useCallback(() => {
    const next: MemoryMode = memoryMode === 'SoA' ? 'AoS' : 'SoA';
    setMemoryMode(next);
    modeRef.current.memory = next;
    loggerRef.current!.mark('memoryMode', memoryMode, next);
    const mod = moduleRef.current;
    if (engineRef.current && mod) {
      engineRef.current.setMemoryMode(next === 'AoS' ? mod.MemoryMode.AoS : mod.MemoryMode.SoA);
      // The inactive layout is not advanced each tick. Start the new layout
      // from the same seed instead of displaying an old, frozen world state.
      engineRef.current.init(engineRef.current.getEntityCount());
    }
    resetHistory();
  }, [memoryMode, resetHistory]);

  // Unlike the two mode toggles, this one rebuilds the world: home points are
  // per-entity state, so switching distribution re-spawns every enemy and
  // zeroes the kill counter. resetHistory() is therefore not cosmetic here --
  // without it the graph would average the new scenario's cost against the old
  // one's and the flip we are trying to show would be smeared out.
  const toggleDistribution = useCallback(() => {
    const next: SpawnDist = spawnDist === 'Uniform' ? 'Clustered' : 'Uniform';
    setSpawnDist(next);
    if (next === 'Clustered' && enemyCount >= 50000)
      setSafetyNotice('고밀도 50,000명 이상은 한 프레임에 수백 ms~1초 이상 걸릴 수 있습니다.');
    modeRef.current.dist = next;
    loggerRef.current!.mark('distribution', spawnDist, next);
    const mod = moduleRef.current;
    if (engineRef.current && mod)
      engineRef.current.setSpawnDistribution(
        next === 'Clustered' ? mod.SpawnDistribution.Clustered : mod.SpawnDistribution.Uniform,
      );
    resetHistory();
  }, [spawnDist, enemyCount, resetHistory]);

  const changeEnemyCount = useCallback((n: number) => {
    if (n === enemyCount) return;
    const engine = engineRef.current;
    const mod = moduleRef.current;
    if (n > 10000 && modeRef.current.collision === 'BruteForce' && engine && mod) {
      loggerRef.current!.mark('collisionMode', 'BruteForce', 'UniformGrid');
      modeRef.current.collision = 'UniformGrid';
      setCollisionMode('UniformGrid');
      engine.setCollisionMode(mod.CollisionMode.UniformGrid);
      setSafetyNotice('10,000명을 넘겨 BruteForce에서 UniformGrid로 전환했습니다.');
    } else {
      setSafetyNotice(n >= 50000
        ? '50,000명 이상에서는 프레임이 크게 느려질 수 있습니다. BruteForce는 10,000명으로 제한됩니다.'
        : '');
    }
    loggerRef.current!.mark('entityCount', String(enemyCount), String(n));
    setEnemyCount(n);
    setEnemyDraft(n);
    engine?.setEnemyCount(n);
    resetHistory();
  }, [enemyCount, resetHistory]);

  const changeSpeed = useCallback((next: number) => {
    if (next === speedMultiplier) return;
    loggerRef.current!.mark('speedMultiplier', String(speedMultiplier), String(next));
    modeRef.current.speed = next;
    setSpeedMultiplier(next);
    loggerRef.current!.resumeAfterGap();
    resetHistory();
  }, [speedMultiplier, resetHistory]);

  const applyRecommendation = useCallback((collision: CollisionMode, memory: MemoryMode) => {
    const engine = engineRef.current;
    const mod = moduleRef.current;
    if (!engine || !mod) return;
    if (collision === 'BruteForce' && engine.getEntityCount() > 10000) {
      setSafetyNotice('10,000명을 초과한 상태에서는 BruteForce 추천을 적용할 수 없습니다.');
      return;
    }
    if (collision !== modeRef.current.collision) {
      loggerRef.current!.mark('collisionMode', modeRef.current.collision, collision);
      modeRef.current.collision = collision;
      setCollisionMode(collision);
      engine.setCollisionMode(mod.CollisionMode[collision]);
    }
    const memoryChanged = memory !== modeRef.current.memory;
    if (memoryChanged) {
      loggerRef.current!.mark('memoryMode', modeRef.current.memory, memory);
      modeRef.current.memory = memory;
      setMemoryMode(memory);
      engine.setMemoryMode(mod.MemoryMode[memory]);
    }
    if (memoryChanged) engine.init(engine.getEntityCount());
    resetHistory();
  }, [resetHistory]);

  const latest = history[history.length - 1];

  return (
    <div className="app">
      {/* Two columns. Left is the thing being measured -- the field, the two
          charts, and the 1 Hz log. Right is everything that describes or
          changes it. The old full-width header put the controls as far from
          the panels that react to them as the layout allowed. */}
      <main className="app-grid">
        <section className="stage-col">
          <div className="panel stage">
            {/* The overlay is a sibling of the canvas rather than something
                drawn into it: drawing "PAUSED" through the 2D context would
                mean the canvas no longer holds the last simulated frame, so
                resuming would flash the text for one frame until the next draw
                overwrote it. */}
            <div className="stage-canvas">
              <GameCanvas engine={engineRef} module={moduleRef} wasmReady={wasmReady} apiRef={canvasApiRef} />
              {paused && (
                <div className="stage-paused">
                  <span className="stage-paused-tag">PAUSED</span>
                  <span className="stage-paused-note">simulation and telemetry stopped</span>
                </div>
              )}
            </div>
            <div className="stage-help">
              <span><kbd>W</kbd><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd> move</span>
              <span><kbd>Space</kbd> pause</span>
              <span className="stage-help-aura">Aura pulses every 2.00 s &mdash; enemies inside the ring take damage</span>
            </div>
          </div>

          <MetricGraph history={history} />
          <TelemetryBar record={logRecord} count={logCount} onDownload={() => downloadNdjson(loggerRef.current!)} />
        </section>

        <aside className="side-col">
          {latest && (
            <div className="hud">
              <span className={latest.fps < 30 ? 'hud-bad' : 'hud-good'}>FPS: {latest.fps.toFixed(1)}</span>
              <span title="C++ EngineCore::tick only">Sim: {latest.simMs.toFixed(2)} ms</span>
              <span title="tick + canvas draw, per frame">Frame: {latest.frameMs.toFixed(2)} ms</span>
              <span>Alive: {latest.entities.toLocaleString()}</span>
              <span className="hud-kills" title="Enemies destroyed by the 2 s aura pulse">
                Kills: {latest.kills.toLocaleString()}
              </span>
              {/* The figures to the left are the last LIVE window, not a reading
                  taken while paused -- nothing is sampled while paused.
                  Labelling that is the difference between a frozen HUD and a
                  wrong one. */}
              {paused && <span className="hud-paused" title="Values are from the last running second">PAUSED</span>}
            </div>
          )}

          <div className="panel title-card">
            <h1>Vampire-Core Wasm <span className="subtitle">실시간 대규모 시뮬레이션 최적화</span></h1>
            <label className="ctrl-label">
              Enemies
              <input type="range" min={500} max={100000} step={500} value={enemyDraft}
                disabled={!wasmReady} title="손을 놓으면 적 수가 적용됩니다"
                onChange={e => setEnemyDraft(Number(e.target.value))}
                onPointerUp={e => changeEnemyCount(Number(e.currentTarget.value))}
                onKeyUp={e => changeEnemyCount(Number(e.currentTarget.value))}
                onBlur={e => changeEnemyCount(Number(e.currentTarget.value))} />
              <span className="ctrl-val">{enemyDraft.toLocaleString()}</span>
            </label>
          </div>

          <div className="panel control-card">
            <label className="ctrl-label" htmlFor="collision-mode">
              Collision:
              <select id="collision-mode" className="mode-select" value={collisionMode}
                disabled={!wasmReady}
                onChange={e => changeCollision(e.target.value as CollisionMode)}>
                <option value="BruteForce" disabled={enemyCount > 10000}>BruteForce</option>
                <option value="QuadTree">QuadTree</option>
                <option value="UniformGrid">UniformGrid</option>
                <option value="SpatialHash">SpatialHash</option>
              </select>
            </label>
            <label className="ctrl-label" htmlFor="simulation-speed">
              Speed:
              <select id="simulation-speed" className="mode-select" value={speedMultiplier}
                disabled={!wasmReady} onChange={e => changeSpeed(Number(e.target.value))}>
                <option value={0.25}>0.25×</option>
                <option value={0.5}>0.5×</option>
                <option value={1}>1×</option>
                <option value={2}>2×</option>
                <option value={4}>4×</option>
              </select>
            </label>
            <button className={`toggle-btn ${memoryMode === 'SoA' ? 'on' : ''}`} onClick={toggleMemory}>
              Memory: <strong>{memoryMode}</strong>
            </button>
          </div>

          <div className="panel control-card">
            {/* Deliberately NOT styled with the same "on" highlight as the two
                mode buttons. Those have a fast path and a slow path, so the
                accent means "the good one". Neither distribution is the good
                one -- which is the whole point of the axis. */}
            <button
              className={`toggle-btn dist-btn ${spawnDist === 'Clustered' ? 'clustered' : ''}`}
              onClick={toggleDistribution}
              disabled={!wasmReady}
              title="Clustered packs the world into 5 blobs; Uniform spreads it evenly. This is what decides whether the QuadTree is worth its build cost."
            >
              Spawn: <strong>{spawnDist}</strong>
            </button>
            <button
              className={`toggle-btn pause-btn ${paused ? 'paused' : ''}`}
              onClick={togglePause}
              disabled={!wasmReady}
              title="Space or P"
            >
              {paused ? 'Resume' : 'Pause'} <kbd>Space</kbd>
            </button>
          </div>

          {safetyNotice && <p className="hud-warn" role="status">{safetyNotice}</p>}
          {!wasmReady && (
            <p className="hud-warn">
              Wasm not loaded &mdash; build core first (<code>make wasm</code>, or <code>.\build-wasm.ps1</code> on Windows)
            </p>
          )}

          <AgentExperimentPanel logger={loggerRef.current!} collisionMode={collisionMode}
            memoryMode={memoryMode} distribution={spawnDist} speedMultiplier={speedMultiplier}
            onApplyRecommendation={applyRecommendation} />
          <CodePanel
            stats={latest}
            collisionMode={collisionMode}
            memoryMode={memoryMode}
            spawnDist={spawnDist}
            logger={loggerRef.current!}
          />
        </aside>
      </main>
    </div>
  );
}

/**
 * A readout of what the logger last wrote, so the log is visible rather than
 * an invisible side effect. p95 sits next to the mean on purpose: the gap
 * between them is the single most diagnostic number on this screen.
 */
function TelemetryBar({ record, count, onDownload }: {
  record:     PerfRecord | null;
  count:      number;
  onDownload: () => void;
}) {
  return (
    <section className="panel telemetry">
      <div className="panel-head">
        <h3 className="panel-title">Logger &mdash; 1 Hz</h3>
        <button className="btn btn-ghost" onClick={onDownload} disabled={count === 0}>
          Download .ndjson ({count})
        </button>
      </div>
      {/* Two independent label/value grids rather than one four-column grid:
          the timing figures read down the left and the world figures down the
          right, which is how the two groups are actually compared. */}
      {record ? (
        <div className="telemetry-grid">
          <dl>
            <dt>t</dt>            <dd>{record.t.toFixed(1)} s</dd>
            <dt>fps</dt>          <dd>{record.fps.toFixed(1)}</dd>
            <dt>sim mean/p95</dt> <dd>{record.simMs.mean.toFixed(2)} / {record.simMs.p95.toFixed(2)} ms</dd>
            <dt>sim p99/max</dt>  <dd>{record.simMs.p99.toFixed(2)} / {record.simMs.max.toFixed(2)} ms</dd>
            <dt>frame p95</dt>    <dd>{record.frameMs.p95.toFixed(2)} ms</dd>
          </dl>
          <dl>
            <dt>entities</dt>    <dd>{record.entities.toLocaleString()} / {record.slots.toLocaleString()}</dd>
            <dt>game speed</dt>  <dd>{record.speedMultiplier}×</dd>
            <dt>kills /s</dt>    <dd>{record.kills}</dd>
            <dt>over budget</dt>
            <dd className={record.longFrames > 0 ? 'hud-bad' : 'hud-good'}>
              {record.longFrames}/{record.frames} frames
            </dd>
          </dl>
        </div>
      ) : (
        <p className="telemetry-empty">Collecting first second&hellip;</p>
      )}
    </section>
  );
}
