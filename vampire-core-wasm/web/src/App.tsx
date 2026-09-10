import { useEffect, useRef, useState, useCallback } from 'react';
import GameCanvas from './components/GameCanvas';
import MetricGraph from './components/MetricGraph';
import CodePanel from './components/CodePanel';

export type CollisionMode = 'BruteForce' | 'QuadTree';
export type MemoryMode    = 'AoS' | 'SoA';

export interface PerfSample {
  time:        number;
  fps:         number;   // browser frame rate, measured in JS
  simMs:       number;   // C++ tick cost, measured in the engine
  frameMs:     number;   // total JS frame budget used
  entities:    number;
}

export interface WasmModule {
  EngineCore:    new () => EngineHandle;
  CollisionMode: { BruteForce: number; QuadTree: number };
  MemoryMode:    { AoS: number; SoA: number };
  HEAPF32:       Float32Array;
}

export interface EngineHandle {
  init:             (n: number) => void;
  tick:             (dt: number) => void;
  setCollisionMode: (m: number) => void;
  setMemoryMode:    (m: number) => void;
  setEnemyCount:    (n: number) => void;
  setPlayerInput:   (dx: number, dy: number) => void;
  getEntityCount:   () => number;
  getPosXPtr:       () => number;
  getPosYPtr:       () => number;
  getStats:         () => { fps: number; frameTimeMs: number; aliveEnemies: number; activeProjectiles: number };
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
  const [enemyCount,    setEnemyCount]    = useState(5000);
  const [history,       setHistory]       = useState<PerfSample[]>([]);
  const [wasmReady,     setWasmReady]     = useState(false);

  const engineRef   = useRef<EngineHandle | null>(null);
  const moduleRef   = useRef<WasmModule | null>(null);
  const canvasApiRef = useRef<{ draw: () => void } | null>(null);
  const keysRef     = useRef<Set<string>>(new Set());
  const rafRef      = useRef<number>(0);
  const lastTimeRef = useRef<number>(0);

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

    const loop = (ts: number) => {
      const frameStart = performance.now();
      const dt = lastTimeRef.current ? Math.min((ts - lastTimeRef.current) / 1000, 0.05) : 0.016;
      lastTimeRef.current = ts;

      const engine = engineRef.current;
      if (engine) {
        let dx = 0, dy = 0;
        const k = keysRef.current;
        if (k.has('ArrowLeft')  || k.has('a')) dx -= 1;
        if (k.has('ArrowRight') || k.has('d')) dx += 1;
        if (k.has('ArrowUp')    || k.has('w')) dy -= 1;
        if (k.has('ArrowDown')  || k.has('s')) dy += 1;
        engine.setPlayerInput(dx, dy);

        engine.tick(dt);
        canvasApiRef.current?.draw();

        frameCountRef.current += 1;
        frameMsAccRef.current += performance.now() - frameStart;

        if (ts - lastStatsRef.current >= STATS_INTERVAL) {
          const elapsed = ts - lastStatsRef.current;
          const frames  = frameCountRef.current;
          const s       = engine.getStats();

          // Real frame rate comes from counting rAF callbacks against the wall
          // clock. The engine's own number is simulation headroom and cannot
          // see the canvas, the browser, or a missed vsync.
          const sample: PerfSample = {
            time:     ts,
            fps:      frames > 0 ? (frames * 1000) / elapsed : 0,
            simMs:    s.frameTimeMs,
            frameMs:  frames > 0 ? frameMsAccRef.current / frames : 0,
            entities: s.aliveEnemies,
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

  // Toggling a mode clears the history so the chart shows the new regime
  // immediately instead of a 120-sample tail of the old one.
  const resetHistory = useCallback(() => {
    setHistory([]);
    frameCountRef.current = 0;
    frameMsAccRef.current = 0;
    lastStatsRef.current  = performance.now();
  }, []);

  const toggleCollision = useCallback(() => {
    const next: CollisionMode = collisionMode === 'QuadTree' ? 'BruteForce' : 'QuadTree';
    setCollisionMode(next);
    const mod = moduleRef.current;
    if (engineRef.current && mod)
      engineRef.current.setCollisionMode(
        next === 'BruteForce' ? mod.CollisionMode.BruteForce : mod.CollisionMode.QuadTree,
      );
    resetHistory();
  }, [collisionMode, resetHistory]);

  const toggleMemory = useCallback(() => {
    const next: MemoryMode = memoryMode === 'SoA' ? 'AoS' : 'SoA';
    setMemoryMode(next);
    const mod = moduleRef.current;
    if (engineRef.current && mod)
      engineRef.current.setMemoryMode(next === 'AoS' ? mod.MemoryMode.AoS : mod.MemoryMode.SoA);
    resetHistory();
  }, [memoryMode, resetHistory]);

  const changeEnemyCount = useCallback((n: number) => {
    setEnemyCount(n);
    engineRef.current?.setEnemyCount(n);
    resetHistory();
  }, [resetHistory]);

  const latest = history[history.length - 1];

  return (
    <div className="app">
      <header className="app-header">
        <h1>Vampire-Core Wasm <span className="subtitle">Performance Tech Demo</span></h1>
        <div className="controls">
          <label className="ctrl-label">
            Enemies:
            <input type="range" min={500} max={10000} step={500} value={enemyCount}
              onChange={e => changeEnemyCount(Number(e.target.value))} />
            <span className="ctrl-val">{enemyCount.toLocaleString()}</span>
          </label>
          <button className={`toggle-btn ${collisionMode === 'QuadTree' ? 'on' : ''}`} onClick={toggleCollision}>
            Collision: <strong>{collisionMode}</strong>
          </button>
          <button className={`toggle-btn ${memoryMode === 'SoA' ? 'on' : ''}`} onClick={toggleMemory}>
            Memory: <strong>{memoryMode}</strong>
          </button>
        </div>
        {latest && (
          <div className="hud">
            <span className={latest.fps < 30 ? 'hud-bad' : 'hud-good'}>FPS: {latest.fps.toFixed(1)}</span>
            <span title="C++ EngineCore::tick only">Sim: {latest.simMs.toFixed(2)} ms</span>
            <span title="tick + canvas draw, per frame">Frame: {latest.frameMs.toFixed(2)} ms</span>
            <span>Alive: {latest.entities.toLocaleString()}</span>
          </div>
        )}
        {!wasmReady && (
          <div className="hud hud-warn">Wasm not loaded &mdash; build core first (<code>make wasm</code>)</div>
        )}
      </header>
      <main className="app-body">
        <GameCanvas engine={engineRef} module={moduleRef} wasmReady={wasmReady} apiRef={canvasApiRef} />
        <aside className="sidebar">
          <MetricGraph history={history} />
          <CodePanel stats={latest} collisionMode={collisionMode} memoryMode={memoryMode} />
        </aside>
      </main>
    </div>
  );
}
