import { useEffect, useState } from 'react';
import type { PerfSample, CollisionMode, MemoryMode, SpawnDist } from '../App';
import { TELEMETRY_WINDOW_S } from '../logging/PerfLogger';
import type { PerfLogger } from '../logging/PerfLogger';
import Markdown from './Markdown';

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
/** The model's three answers, still separate. The server also weaves them into
 *  one markdown string for backwards compatibility, but the report shows them
 *  as three distinct things -- a diagnosis, a refactoring, and its expected
 *  effect -- so it reads them from here and never re-splits that markdown.
 *  Absent when the model's reply was not parseable JSON. */
interface AnalysisSections {
  diagnosis?:      string | null;
  snippet?:        string | null;
  expectedImpact?: string | null;
}

interface Recommendation {
  collision?:  CollisionMode | null;
  memory?:     MemoryMode    | null;
  confidence?: string        | null;
  reason?:     string        | null;
}

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
  const [sections, setSections] = useState<AnalysisSections | null>(null);
  const [rec,      setRec]      = useState<Recommendation | null>(null);
  const [loading,  setLoading]  = useState(false);
  // The report is fetched as a job rather than a single long request: the
  // model call outlives what the edge proxy in front of the API will hold
  // open, so submit and collection are separate round trips.
  const [jobId,    setJobId]    = useState('');
  // The report used to live in its own window because it did not fit a 480 px
  // sidebar. The sidebar is now the wide column of the two-column layout, so
  // the report reads fine in place -- and keeping it in the page means the
  // numbers it quotes sit next to the HUD that produced them. Dismissing it
  // only hides it: re-opening must not cost another round trip to the model.
  const [reportOpen, setReportOpen] = useState(true);

  const snippet = SNIPPETS[collisionMode][memoryMode];

  // The size of the window that is actually posted, not the size of the
  // session. These two diverge after the first 30 seconds, and this number is
  // printed in three places that all claim to describe the request -- the
  // button, the report subheading and the telemetry badge -- so reading it off
  // the whole log made all three overstate the evidence by however long the
  // demo had been running.
  //
  // The endpoint accepts a request with no telemetry (that was the original
  // contract), so the button stays usable during the first second of a run --
  // it just sends less evidence, and says so in the prompt.
  const recordCount = logger.windowCount(TELEMETRY_WINDOW_S);

  const runAnalysis = async () => {
    if (!stats) return;
    setReportOpen(true);
    setLoading(true);
    setAnalysis('');
    setSections(null);
    setRec(null);
    try {
      const res = await fetch('/api/analysis-jobs', {
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
      const data: { jobId: string } = await res.json();
      // loading stays true: the effect below owns it until the report lands.
      setJobId(data.jobId);
    } catch (e: unknown) {
      setAnalysis(`Error: ${e instanceof Error ? e.message : 'unreachable backend'}`);
      setLoading(false);
    }
  };

  useEffect(() => {
    if (!jobId) return;
    let disposed = false;
    const settle = (finish: () => void) => {
      if (disposed) return;
      finish();
      setLoading(false);
      setJobId('');
    };
    const poll = async () => {
      try {
        const response = await fetch(`/api/analysis-jobs/${jobId}`);
        if (!response.ok) throw new Error(await response.text());
        const data = await response.json();
        if (data.status === 'ready') {
          settle(() => {
            setAnalysis(data.analysis);
            setSections(data.sections ?? null);
            setRec(data.recommendation ?? null);
          });
        } else if (data.status === 'failed') {
          settle(() => setAnalysis(`Error: ${data.error ?? 'analysis failed'}`));
        }
      } catch (cause) {
        settle(() => setAnalysis(`Error: ${cause instanceof Error ? cause.message : 'unreachable backend'}`));
      }
    };
    void poll();
    const timer = window.setInterval(() => void poll(), 1500);
    return () => { disposed = true; window.clearInterval(timer); };
  }, [jobId]);

  // Shown separately from the prose so the verdict is legible at a glance, and
  // so it is obvious when the agent's pick differs from what is running now --
  // that disagreement is the whole point of the panel.
  const agrees = rec !== null
    && (!rec.collision || rec.collision === collisionMode)
    && (!rec.memory    || rec.memory    === memoryMode);

  const hasResult = loading || !!analysis || !!rec || !!sections;

  return (
    <section className="panel code-panel">
      <div className="panel-head">
        {/* The spawn distribution is named here even though it changes nothing
            in the snippet the report carries, because the panel's claim is
            "this is what is producing the numbers" -- and at fixed source, the
            world is half of that. */}
        <h3 className="panel-title">
          Active Code Path &mdash; {collisionMode} / {memoryMode}{' '}
          <span className="panel-title-dist">on {spawnDist}</span>
        </h3>
        <button type="button" className="btn btn-accent" onClick={runAnalysis} disabled={loading || !stats}>
          {loading ? 'Analyzing...' : `AI Analyze (${recordCount}s)`}
        </button>
      </div>

      {hasResult && (
        <div className="report">
          <div className="report-head">
            <div>
              <h4 className="report-heading">AI Analyze &mdash; 활성 코드 경로 진단</h4>
              <p className="report-sub">
                {collisionMode} / {memoryMode} · {spawnDist} · 최근 {TELEMETRY_WINDOW_S}s 텔레메트리 {recordCount}개 구간
              </p>
            </div>
            <button type="button" className="btn btn-ghost" onClick={() => setReportOpen(o => !o)}>
              {reportOpen ? '창 닫기' : '결과 열기'}
            </button>
          </div>
          {reportOpen && (
            <AnalysisReport
              loading={loading} analysis={analysis} sections={sections} rec={rec} agrees={agrees}
              collisionMode={collisionMode} memoryMode={memoryMode} spawnDist={spawnDist}
              stats={stats} snippet={snippet} recordCount={recordCount}
            />
          )}
        </div>
      )}
    </section>
  );
}

/** The report body: badges, the parsed verdict, the prose, and the snippet
 *  that was sent with the request. */
function AnalysisReport({ loading, analysis, sections, rec, agrees, collisionMode, memoryMode, spawnDist,
  stats, snippet, recordCount }: {
  loading:       boolean;
  analysis:      string;
  sections:      AnalysisSections | null;
  rec:           Recommendation | null;
  agrees:        boolean;
  collisionMode: CollisionMode;
  memoryMode:    MemoryMode;
  spawnDist:     SpawnDist;
  stats?:        PerfSample;
  snippet:       string;
  recordCount:   number;
}) {
  return (
    <div className="analysis-report">
      {stats && (
        <div className="report-badges">
          <span className="badge">Alive: {stats.entities.toLocaleString()}</span>
          <span className="badge">FPS: {stats.fps.toFixed(1)}</span>
          <span className="badge">Sim: {stats.simMs.toFixed(2)} ms</span>
          <span className="badge">Frame: {stats.frameMs.toFixed(2)} ms</span>
          <span className="badge">{collisionMode} / {memoryMode} · {spawnDist}</span>
          <span className="badge">telemetry {recordCount}s</span>
        </div>
      )}
      {loading && (
        <div className="report-loading" role="status">
          <span className="spinner" aria-hidden="true" />
          <p>LLM 에이전트가 런타임 병목과 C++ 코드를 분석 중입니다…</p>
        </div>
      )}
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
      {/* Three cards, not one blob. The diagnosis, the refactoring and its
          expected effect answer different questions and get compared against
          different things -- the telemetry above, the running code path, and
          the recommendation bar -- so stacking them under one "AI Analysis"
          heading made the reader do the splitting. */}
      {sections ? (
        <div className="analysis-sections">
          {sections.diagnosis && (
            <section className="analysis analysis-diagnosis">
              <h4>병목 진단</h4>
              <Markdown text={sections.diagnosis} />
            </section>
          )}
          {sections.snippet && (
            <section className="analysis analysis-code">
              <h4>최적화된 C++ 스니펫</h4>
              <pre className="code-snippet"><code>{sections.snippet}</code></pre>
            </section>
          )}
          {sections.expectedImpact && (
            <section className="analysis analysis-impact">
              <h4>예상 효과</h4>
              <Markdown text={sections.expectedImpact} />
            </section>
          )}
        </div>
      ) : analysis && (
        /* No sections means the model ignored the JSON contract, so `analysis`
           is raw text of unknown shape. Rendered rather than dumped: literal
           `##` and backticks on screen read as a broken report. */
        <section className="analysis">
          <h4>AI Analysis</h4>
          <Markdown text={analysis} />
        </section>
      )}
      {(analysis || rec || sections) && (
        <details className="report-source">
          <summary>분석에 함께 보낸 코드 요약</summary>
          <pre className="code-snippet"><code>{snippet}</code></pre>
        </details>
      )}
    </div>
  );
}
