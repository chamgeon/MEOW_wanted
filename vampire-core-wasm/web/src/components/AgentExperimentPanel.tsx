import { useEffect, useState } from 'react';
import type { CollisionMode, MemoryMode, SpawnDist } from '../App';
import type { PerfLogger } from '../logging/PerfLogger';
import { usePopupWindow, PopupWindowContent } from './PopupWindow';

interface Timing { medianMs: number; p99Ms: number; samples: number }
interface Benchmark {
  entities: number;
  distribution: SpawnDist;
  baseline: Timing;
  candidate: Timing;
  speedup: number;
}
interface Candidate {
  algorithm: string;
  memoryMode?: MemoryMode;
  runMode?: CollisionMode;
  sourceKind?: 'existing' | 'generated';
  status: string;
  reason?: string | null;
  diff?: string;
  benchmarks?: Benchmark[];
  correctness?: Array<{ entities: number; distribution: string; passed: boolean; maxPositionDelta?: number }>;
}
interface Job {
  jobId: string;
  status: string;
  sourceHash?: string;
  reason?: string;
  advisorMode?: string;
  aiAdvice?: string | null;
  plannedCandidates?: string[];
  baseline?: { algorithm: string; memory: string; distribution: string };
  candidates: Candidate[];
  winner?: string | null;
  winnerMemory?: MemoryMode | null;
  fastestMeasured?: { algorithm: string; memoryMode: MemoryMode; eligible: boolean } | null;
  diff?: string | null;
  limitations?: string;
  observations?: { records: number; entityMin: number; entityMax: number; fpsMedian?: number;
    simMedianMs: number; windowP99MedianMs: number; frameMedianMs: number;
    speedMultiplier?: number; benchmarkFrameDtMs?: number; longFrames?: number;
    framesTotal?: number; lowSampleWindows?: number };
}

const TERMINAL = new Set(['ready', 'no_improvement', 'failed', 'interrupted']);

export default function AgentExperimentPanel({ logger, collisionMode, memoryMode, distribution, speedMultiplier, onApplyRecommendation }: {
  logger: PerfLogger;
  collisionMode: CollisionMode;
  memoryMode: MemoryMode;
  distribution: SpawnDist;
  speedMultiplier: number;
  onApplyRecommendation: (collision: CollisionMode, memory: MemoryMode) => void;
}) {
  const [jobId, setJobId] = useState(() => sessionStorage.getItem('optimizationJobId') || '');
  const [job, setJob] = useState<Job | null>(null);
  const [error, setError] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const [currentSourceHash, setCurrentSourceHash] = useState('');

  // Six candidate combinations, each with a benchmark table and possibly a
  // diff. That is a document, and the sidebar was clipping it to a 440 px
  // scroll box -- so it gets its own window.
  const popup = usePopupWindow('vc_optimize', 'AI Optimization Experiment - Vampire-Core Wasm');

  useEffect(() => {
    let disposed = false;
    void fetch('/api/agent-meta').then(response => response.json())
      .then((meta: { sourceHash: string }) => { if (!disposed) setCurrentSourceHash(meta.sourceHash); })
      .catch(() => { if (!disposed) setCurrentSourceHash(''); });
    return () => { disposed = true; };
  }, []);

  useEffect(() => {
    if (!jobId) return;
    let disposed = false;
    const poll = async () => {
      try {
        const response = await fetch(`/api/optimization-jobs/${jobId}`);
        if (!response.ok) throw new Error(await response.text());
        const report: Job = await response.json();
        if (!disposed) { setJob(report); setError(''); }
      } catch (cause) {
        if (!disposed) setError(cause instanceof Error ? cause.message : 'Result unavailable');
      }
    };
    void poll();
    const interval = window.setInterval(() => {
      if (!job || !TERMINAL.has(job.status)) void poll();
    }, 1800);
    return () => { disposed = true; window.clearInterval(interval); };
  }, [jobId, job?.status]);

  const start = async () => {
    // Opened before the first await, while this click still counts as user
    // activation -- the experiment takes tens of seconds and the window would
    // be blocked if it waited for the response.
    popup.open();
    setSubmitting(true);
    setError('');
    try {
      const telemetry = logger.window(30);
      const comparable = telemetry.records.filter(r => r.collisionMode === collisionMode &&
        r.memoryMode === memoryMode && r.distribution === distribution &&
        r.speedMultiplier === speedMultiplier && r.frames >= 1);
      if (comparable.length < 3) throw new Error('현재 모드에서 최소 3개 성능 로그 구간을 수집해 주세요.');
      const metaResponse = await fetch('/api/agent-meta');
      if (!metaResponse.ok) throw new Error('서버 소스 버전을 확인할 수 없습니다.');
      const meta: { sourceHash: string } = await metaResponse.json();
      setCurrentSourceHash(meta.sourceHash);
      const response = await fetch('/api/optimization-jobs', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ collisionMode, memoryMode, distribution, speedMultiplier,
          sourceHash: meta.sourceHash, telemetry }),
      });
      if (!response.ok) throw new Error(await response.text());
      const created: { jobId: string } = await response.json();
      sessionStorage.setItem('optimizationJobId', created.jobId);
      setJob(null);
      setJobId(created.jobId);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : '분석 요청에 실패했습니다.');
    } finally {
      setSubmitting(false);
    }
  };

  const running = submitting || (!!jobId && (!job || !TERMINAL.has(job.status)));
  const hasResult = running || !!job || !!error;

  const report = (
    <ExperimentReport
      job={job} error={error} running={running} currentSourceHash={currentSourceHash}
      collisionMode={collisionMode} memoryMode={memoryMode} distribution={distribution}
      speedMultiplier={speedMultiplier} onApplyRecommendation={onApplyRecommendation}
    />
  );

  return (
    <section className="agent-experiment" aria-label="Verified optimization agent">
      <div className="agent-head">
        <h3>AI Optimization Experiment</h3>
        <div className="panel-actions">
          {/* Re-open, not re-run: closing the window must not cost another
              multi-minute experiment. */}
          {hasResult && !popup.isOpen && !popup.blocked && (
            <button type="button" className="ghost-btn" onClick={() => popup.open()}>결과 창 열기</button>
          )}
          {popup.isOpen && (
            <button type="button" className="ghost-btn" onClick={popup.close}>결과 창 닫기</button>
          )}
          <button type="button" onClick={() => void start()} disabled={running}>
            {running ? '실험 중…' : 'Optimize'}
          </button>
        </div>
      </div>
      <p className="agent-note">현재 {collisionMode}/{memoryMode} · {distribution} · {speedMultiplier}×를 기준으로 다른 3개 알고리즘 × AoS/SoA, 총 6개 조합을 비교합니다.</p>
      {/* The sidebar keeps only the line you want while the game is still
          running: which job, and whether it is still working. */}
      {job && <div role="status" className="agent-status">작업 {job.jobId.slice(0, 8)} · {job.status}</div>}
      {popup.blocked
        ? <>
            <p className="agent-error" role="alert">
              팝업이 차단되어 결과를 이 패널에 표시합니다. 브라우저에서 이 사이트의 팝업을 허용하면 새 창으로 열립니다.
            </p>
            {hasResult && report}
          </>
        : hasResult && (
            <p className="agent-note">
              {popup.isOpen ? '실험 결과는 새 창에 표시됩니다.' : '결과 창이 닫혔습니다. "결과 창 열기"로 다시 볼 수 있습니다.'}
            </p>
          )}
      <PopupWindowContent
        handle={popup}
        heading="AI Optimization Experiment - 검증된 후보 비교"
        subheading={`기준 ${collisionMode}/${memoryMode} · ${distribution} · ${speedMultiplier}×`}
      >
        {report}
      </PopupWindowContent>
    </section>
  );
}

/** The full experiment report: advice, observations, per-candidate benchmarks
 *  and diffs. Lives in the popup; rendered inline only as a popup-blocked
 *  fallback. */
function ExperimentReport({ job, error, running, currentSourceHash, collisionMode, memoryMode,
  distribution, speedMultiplier, onApplyRecommendation }: {
  job:               Job | null;
  error:             string;
  running:           boolean;
  currentSourceHash: string;
  collisionMode:     CollisionMode;
  memoryMode:        MemoryMode;
  distribution:      SpawnDist;
  speedMultiplier:   number;
  onApplyRecommendation: (collision: CollisionMode, memory: MemoryMode) => void;
}) {
  return (
    <div className="experiment-report">
      <div className="report-badges">
        <span className="badge">기준 {collisionMode} / {memoryMode}</span>
        <span className="badge">{distribution}</span>
        <span className="badge">{speedMultiplier}×</span>
        {job && <span className="badge">job {job.jobId.slice(0, 8)}</span>}
        {job && <span className="badge">{job.status}</span>}
      </div>
      {error && <p className="agent-error" role="alert">{error}</p>}
      {running && (
        <div className="report-loading" role="status">
          <span className="spinner" aria-hidden="true" />
          <p>후보 6개 조합을 빌드하고 동일 조건에서 벤치마크하는 중입니다…</p>
        </div>
      )}
      {job?.aiAdvice && <p className="agent-advice">{job.aiAdvice}</p>}
      {job?.advisorMode && <p className="agent-note">Advisor: {job.advisorMode}</p>}
      {job?.observations && <p className="agent-note">
        분석 입력: 최근 30초 중 동일 설정의 {job.observations.records}개 로그 구간 · 적 {job.observations.entityMin.toLocaleString()}–{job.observations.entityMax.toLocaleString()}명
        · FPS 중앙값 {job.observations.fpsMedian ?? '—'} · Sim 중앙값 {job.observations.simMedianMs} ms
        · 1초 구간 p99 중앙값 {job.observations.windowP99MedianMs} ms · Frame 중앙값 {job.observations.frameMedianMs} ms
        · 배속 {job.observations.speedMultiplier ?? 1}× · 프레임 표본 {job.observations.framesTotal ?? '—'}개
        · 벤치마크 프레임 간격 {job.observations.benchmarkFrameDtMs ?? '—'} ms
      </p>}
      {!!job?.observations?.lowSampleWindows &&
        <p className="agent-note">저FPS 구간 {job.observations.lowSampleWindows}개는 프레임 표본이 3개 미만이라 p99 신뢰도가 낮습니다.</p>}
      {job?.candidates?.length ? (
        <div className="agent-candidates">
          <p className="agent-note">원본: {job.baseline?.algorithm ?? collisionMode}/{job.baseline?.memory ?? memoryMode} · 후보는 같은 원본과 비교</p>
          {job.candidates.map(candidate => (
            <div className="agent-candidate" key={`${candidate.algorithm}-${candidate.memoryMode ?? 'legacy'}`}>
              <strong>{candidate.algorithm}/{candidate.memoryMode ?? memoryMode}
                {job.winner === candidate.algorithm && job.winnerMemory === candidate.memoryMode ? ' · 추천' : ''}
              </strong>
              <span>{candidate.status}</span>
              {candidate.sourceKind === 'existing' && <p>게임에 이미 있는 코드 경로이므로 별도 코드 diff는 없습니다.</p>}
              {candidate.reason && <p>{candidate.reason}</p>}
              {candidate.correctness?.length ? (
                <p>동작 검사: {candidate.correctness.every(check => check.passed) ? '통과' :
                  `불일치 · ${candidate.correctness.filter(check => !check.passed).map(check =>
                    `${check.entities}/${check.distribution} (위치 차이 ${check.maxPositionDelta ?? '?'})`).join(', ')}`}
                </p>
              ) : null}
              {candidate.benchmarks?.length ? (
                <table>
                  <thead><tr><th>조건</th><th>원본</th><th>후보</th><th>배속</th></tr></thead>
                  <tbody>{candidate.benchmarks.map(row => (
                    <tr key={`${candidate.algorithm}-${candidate.memoryMode}-${row.entities}-${row.distribution}`}>
                      <td>{row.entities} / {row.distribution}</td>
                      <td>{row.baseline.medianMs} ms</td>
                      <td>{row.candidate.medianMs} ms</td>
                      <td>{row.speedup}×</td>
                    </tr>
                  ))}</tbody>
                </table>
              ) : null}
              {candidate.diff && <details><summary>코드 diff</summary><pre>{candidate.diff}</pre></details>}
            </div>
          ))}
        </div>
      ) : null}
      {job && TERMINAL.has(job.status) && (
        <p className="agent-result">
          {job.winner ? `동작과 성능 기준을 통과한 추천: ${job.winner}/${job.winnerMemory ?? memoryMode}. 전후 수치를 확인하세요.` :
            job.status === 'no_improvement' ? '검증된 개선 후보가 없어 원본을 유지합니다.' : job.reason}
        </p>
      )}
      {/* Lives in the popup but drives the game in the opener window: the
          handler belongs to the parent React tree, so the portal costs nothing
          extra here. */}
      {job?.winner && job.winnerMemory && job.status === 'ready' &&
        job.sourceHash === currentSourceHash &&
        job.candidates.some(candidate => candidate.algorithm === job.winner &&
          candidate.memoryMode === job.winnerMemory && candidate.sourceKind === 'existing') &&
        <button type="button" className="apply-btn"
          onClick={() => onApplyRecommendation(job.winner as CollisionMode, job.winnerMemory as MemoryMode)}>
          추천 설정으로 게임 전환
        </button>}
      {job?.winner && job.winnerMemory && <p className="agent-note">메모리 모드가 바뀌면 같은 시드로 게임 세계를 다시 시작합니다.</p>}
      {job?.fastestMeasured && !job.fastestMeasured.eligible &&
        <p className="agent-note">측정상 가장 빠른 {job.fastestMeasured.algorithm}/{job.fastestMeasured.memoryMode}는 검증 기준을 통과하지 못해 추천하지 않습니다.</p>}
      {job?.limitations && <p className="agent-note">{job.limitations}</p>}
    </div>
  );
}
