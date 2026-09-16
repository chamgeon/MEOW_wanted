import { useState } from 'react';
import type { PerfSample, CollisionMode, MemoryMode, SpawnDist } from '../App';
import type { PerfLogger } from '../logging/PerfLogger';

interface Props {
  stats?:        PerfSample;
  collisionMode: CollisionMode;
  memoryMode:    MemoryMode;
  /** Which world the kernel is running against. Not a code path -- the snippet
   *  below is byte-identical in both -- but without it the agent sees two
   *  wildly different query costs for the same source and has no way to
   *  attribute the difference to anything but noise. */
  spawnDist:     SpawnDist;
  /** Source of the telemetry series posted with the analysis request. The
   *  single-sample HUD numbers below are kept for backwards compatibility with
   *  the original endpoint contract, but the series is what actually lets the
   *  agent tell a slow kernel from a spiky one. */
  logger:        PerfLogger;
}

/** The machine-parsed half of the optimizer's reply. Every field is optional
 *  because the server returns nulls rather than guesses when the block fails
 *  to parse -- acting on a fabricated mode would be worse than acting on none. */
interface Recommendation {
  collision?:  CollisionMode | null;
  memory?:     MemoryMode    | null;
  confidence?: string        | null;
  reason?:     string        | null;
}

// How much history travels with an analysis request.
//
// Thirty seconds is roughly the span in which a user toggles a mode, watches
// the number move, and toggles back -- so the window usually contains both
// sides of a comparison plus the marker that separates them. A full session
// would be mostly redundant and would push the prompt into territory where the
// model starts skimming rather than reading.
const TELEMETRY_WINDOW_S = 30;

const SNIPPETS: Record<CollisionMode, Record<MemoryMode, string>> = {
  BruteForce: {
    SoA: `// SoA Brute-Force  O(N^2) repulsion
for (int i = 0; i < n; ++i) {
  float fx = 0, fy = 0;
  for (int j = 0; j < n; ++j) {  // hot inner loop
    float dx = posX[i] - posX[j]; // contiguous reads
    float dy = posY[i] - posY[j];
    float d2 = dx*dx + dy*dy;
    if (d2 < sepDistSq && d2 > 1e-6f) {
      float d = sqrtf(d2);
      float f = (sep-d)/sep * force;
      fx += dx/d*f; fy += dy/d*f;
    }
  }
  posX[i] += fx*dt; posY[i] += fy*dt;
}`,
    AoS: `// AoS Brute-Force  O(N^2) repulsion
for (int i = 0; i < n; ++i) {
  Vector2D force{};
  for (int j = 0; j < n; ++j) {  // cache-unfriendly struct stride
    Vector2D d  = e[i].position - e[j].position;
    float    d2 = d.lengthSq();
    if (d2 < sepDistSq && d2 > 1e-6f) {
      float dist = sqrtf(d2);
      float f = (sep-dist)/sep * REPULSION_FORCE;
      force += d / dist * f;
    }
  }
  e[i].position += force * dt;
}`,
  },
  QuadTree: {
    SoA: `// SoA QuadTree  O(N log N) repulsion
quadTree.clear();
for (int i = 0; i < n; ++i)
  quadTree.insert({posX[i], posY[i], i});

for (int i = 0; i < n; ++i) {
  AABB range{posX[i], posY[i], sep, sep};
  quadTree.query(range, neighbors); // ~log N
  float fx = 0, fy = 0;
  for (int j : neighbors) {
    float dx = posX[i]-posX[j];
    float dy = posY[i]-posY[j];
    // apply repulsion only to nearby set
  }
  posX[i] += fx*dt; posY[i] += fy*dt;
}`,
    AoS: `// AoS QuadTree  O(N log N) repulsion
quadTree.clear();
for (int i = 0; i < n; ++i)
  quadTree.insert({e[i].position.x, e[i].position.y, i});

for (int i = 0; i < n; ++i) {
  AABB range{e[i].position.x, e[i].position.y, sep, sep};
  quadTree.query(range, neighbors);
  Vector2D force{};
  for (int j : neighbors) {
    Vector2D d = e[i].position - e[j].position;
    // apply repulsion only to nearby set
  }
  e[i].position += force * dt;
}`,
  },
  UniformGrid: {
    SoA: `// 설명용 요약: UniformGrid / SoA (실행 소스 원문 아님)
// x/y 좌표는 posX[], posY[] 배열에 각각 저장
for (int i = 0; i < n; ++i) grid[cell(posX[i], posY[i])].add(i);
for (int i = 0; i < n; ++i) {
  auto neighbors = grid.nearby(posX[i], posY[i]);
  // 인접 셀의 적만 검사하고, 이동한 적의 셀을 갱신
  applyRepulsion(i, neighbors);
}`,
    AoS: `// 설명용 요약: UniformGrid / AoS (실행 소스 원문 아님)
// 각 Enemy 구조체에 위치와 상태를 함께 저장
for (int i = 0; i < n; ++i) grid[cell(enemies[i].position)].add(i);
for (int i = 0; i < n; ++i) {
  auto neighbors = grid.nearby(enemies[i].position);
  applyRepulsion(i, neighbors);
}`,
  },
  SpatialHash: {
    SoA: `// 설명용 요약: SpatialHash / SoA (실행 소스 원문 아님)
// 사용 중인 셀만 해시 테이블에 저장
for (int i = 0; i < n; ++i) cells[hash(posX[i], posY[i])].add(i);
for (int i = 0; i < n; ++i) {
  auto neighbors = adjacentHashedCells(posX[i], posY[i]);
  applyRepulsion(i, neighbors);
}`,
    AoS: `// 설명용 요약: SpatialHash / AoS (실행 소스 원문 아님)
for (int i = 0; i < n; ++i) cells[hash(enemies[i].position)].add(i);
for (int i = 0; i < n; ++i) {
  auto neighbors = adjacentHashedCells(enemies[i].position);
  applyRepulsion(i, neighbors);
}`,
  },
};

// The aura runs in both modes and is a flat linear scan by design, so it is
// part of the cost the agent is being asked to explain. Shipping the snippet
// stops it from attributing aura time to the repulsion kernel above.
const AURA_SNIPPET = `// Aura pulse, fired every Config::AURA_INTERVAL (2.0 s).
// Deliberately a linear scan in BOTH collision modes: reusing the QuadTree
// would read pre-repulsion positions and make the two modes disagree on the
// kill set, which would break the AoS/SoA equivalence check.
void EngineCore::fireAura() {
  const float rSq = Config::AURA_RADIUS * Config::AURA_RADIUS;
  for (int i = 0; i < n; ++i) {
    if (!alive[i]) continue;
    float dx = posX[i] - playerPos_.x;
    float dy = posY[i] - playerPos_.y;
    float d2 = dx*dx + dy*dy;
    if (d2 > rSq) continue;
    health[i] -= Config::AURA_DAMAGE;
    // knockback along the normalised (dx, dy)
    if (health[i] <= 0.f) alive[i] = 0;
  }
}`;

export default function CodePanel({ stats, collisionMode, memoryMode, spawnDist, logger }: Props) {
  const [analysis, setAnalysis] = useState('');
  const [rec,      setRec]      = useState<Recommendation | null>(null);
  const [loading,  setLoading]  = useState(false);

  const snippet = SNIPPETS[collisionMode][memoryMode];

  // The endpoint accepts a request with no telemetry (that was the original
  // contract), so the button stays usable during the first second of a run --
  // it just sends less evidence, and says so in the prompt.
  const recordCount = logger.getRecords().length;

  const runAnalysis = async () => {
    if (!stats) return;
    setLoading(true);
    setAnalysis('');
    setRec(null);
    try {
      const res = await fetch('/api/optimize', {
        method:  'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          fps: stats.fps, frameTimeMs: stats.simMs,
          entityCount: stats.entities, collisionMode, memoryMode,
          spawnDistribution: spawnDist,
          codeSnippet: snippet,
          auraSnippet: AURA_SNIPPET,
          telemetry: logger.window(TELEMETRY_WINDOW_S),
        }),
      });
      if (!res.ok) throw new Error(await res.text());
      const data = await res.json();
      setAnalysis(data.analysis);
      setRec(data.recommendation ?? null);
    } catch (e: unknown) {
      setAnalysis(`Error: ${e instanceof Error ? e.message : 'unreachable backend'}`);
    } finally {
      setLoading(false);
    }
  };

  // Shown separately from the prose so the verdict is legible at a glance, and
  // so it is obvious when the agent's pick differs from what is running now --
  // that disagreement is the whole point of the panel.
  const agrees = rec !== null
    && (!rec.collision || rec.collision === collisionMode)
    && (!rec.memory    || rec.memory    === memoryMode);

  return (
    <div className="code-panel">
      <div className="code-panel-header">
        {/* The spawn distribution is named here even though it changes nothing
            in the snippet below, because the panel's claim is "this is what is
            producing the numbers" -- and at fixed source, the world is half
            of that. */}
        <h3>Active Code Path &mdash; {collisionMode} / {memoryMode} <span className="code-panel-dist">on {spawnDist}</span></h3>
        <button onClick={runAnalysis} disabled={loading || !stats}>
          {loading ? 'Analyzing...' : `AI Analyze (${recordCount}s)`}
        </button>
      </div>
      <pre className="code-snippet"><code>{snippet}</code></pre>
      <p className="agent-note">위 코드는 설명용 요약입니다. 게임은 빌드된 WASM의 실제 C++ 코드를 실행합니다.</p>
      {rec && (
        <div className={`recommendation ${agrees ? 'rec-agree' : 'rec-differ'}`}>
          <strong>
            Recommends: {rec.collision ?? collisionMode} / {rec.memory ?? memoryMode}
          </strong>
          {rec.confidence && <span className="rec-conf">{rec.confidence} confidence</span>}
          {agrees
            ? <span className="rec-note">matches the running configuration</span>
            : <span className="rec-note">differs from what is running</span>}
          {rec.reason && <p className="rec-reason">{rec.reason}</p>}
        </div>
      )}
      {analysis && (
        <div className="analysis">
          <h4>AI Analysis</h4>
          <pre>{analysis}</pre>
        </div>
      )}
    </div>
  );
}
