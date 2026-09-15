import { useEffect, useState } from 'react';
import type { CollisionMode, MemoryMode, PerfSample } from '../App';

interface Props {
  history: PerfSample[];
  collisionMode: CollisionMode;
  memoryMode: MemoryMode;
}

interface Timing { medianMs: number; p99Ms: number; samples: number }
interface Report {
  jobId: string;
  status: string;
  reason?: string;
  diagnosis?: string;
  generator?: string;
  advisorMode?: string;
  aiAdvice?: string;
  diff?: string;
  correctness?: { aliveMatch: boolean; maxPositionDelta: number; tolerance: number; passed: boolean };
  benchmarks?: Array<{ entities: number; baseline: Timing; candidate: Timing; speedup: number }>;
  artifactUrl?: string;
}

export default function OptimizationPanel({ history, collisionMode, memoryMode }: Props) {
  const [jobId, setJobId] = useState(new URLSearchParams(window.location.search).get('job') || '');
  const [report, setReport] = useState<Report | null>(null);
  const [error, setError] = useState('');

  useEffect(() => {
    if (!jobId) return;
    let disposed = false;
    const poll = async () => {
      try {
        const response = await fetch(`/api/optimization-jobs/${jobId}`);
        if (!response.ok) throw new Error(await response.text());
        const data: Report = await response.json();
        if (!disposed) setReport(data);
      } catch (cause) {
        if (!disposed) setError(cause instanceof Error ? cause.message : 'Could not fetch job');
      }
    };
    void poll();
    const timer = window.setInterval(() => void poll(), 1500);
    return () => { disposed = true; window.clearInterval(timer); };
  }, [jobId]);

  const busy = report?.status === 'queued' || report?.status === 'building';
  const start = async () => {
    setError('');
    setReport(null);
    setJobId('');
    try {
      const response = await fetch('/api/optimization-jobs', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ collisionMode, memoryMode, samples: history }),
      });
      if (!response.ok) throw new Error(await response.text());
      const data: { jobId: string } = await response.json();
      setJobId(data.jobId);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : 'Could not start job');
    }
  };

  return (
    <section className="optimization-panel" aria-label="Verified optimization experiment">
      <div className="optimization-heading">
        <h3>Verified optimization experiment</h3>
        <button onClick={start} disabled={busy || history.length === 0}>
          {busy ? 'Analyzing…' : collisionMode === 'BruteForce' && memoryMode === 'SoA' ? 'Analyze, build & compare' : 'Analyze bottleneck'}
        </button>
      </div>
      <p className="optimization-hint">
        {collisionMode === 'BruteForce' && memoryMode === 'SoA'
          ? 'Analyze this path, then build and compare the reviewed QuadTree candidate.'
          : `Analyze the actual ${collisionMode}/${memoryMode} implementation and recent performance history. Suggestions are hypotheses; no candidate is built for this mode yet.`}
      </p>
      {error && <p className="optimization-error" role="alert">{error}</p>}
      {report && <p className="optimization-status" role="status">Status: {report.status}{report.reason ? ` — ${report.reason}` : ''}</p>}
      {report?.diagnosis && <p className="optimization-diagnosis">{report.diagnosis}</p>}
      {report?.advisorMode && <p>Advisor: {report.advisorMode}</p>}
      {report?.aiAdvice && <p className="optimization-diagnosis">{report.aiAdvice}</p>}
      {report?.correctness && <p>Short-run world-state check: {report.correctness.passed ? 'passed' : 'failed'} · max position difference {report.correctness.maxPositionDelta.toFixed(3)} / tolerance {report.correctness.tolerance}</p>}
      {report?.benchmarks && (
        <div className="optimization-table-wrap">
          <p className="optimization-hint">Pilot WASM timings from newly spawned enemies. A dense late-game swarm may behave differently; p99 uses a small sample.</p>
          <table className="optimization-table">
            <thead><tr><th>Enemies</th><th>Before median / p99</th><th>After median / p99</th><th>Speedup</th></tr></thead>
            <tbody>{report.benchmarks.map(row => (
              <tr key={row.entities}>
                <td>{row.entities.toLocaleString()}</td>
                <td>{row.baseline.medianMs} / {row.baseline.p99Ms} ms</td>
                <td>{row.candidate.medianMs} / {row.candidate.p99Ms} ms</td>
                <td>{row.speedup}×</td>
              </tr>
            ))}</tbody>
          </table>
        </div>
      )}
      {report?.artifactUrl && (
        <p className="optimization-preview">
          Candidate WASM built and retained for review.{' '}
          {!new URLSearchParams(window.location.search).has('job') && (
            <button onClick={() => { window.location.href = `/?job=${report.jobId}`; }}>Preview candidate in game</button>
          )}
        </p>
      )}
      {report?.diff && <details><summary>Actual C++ diff ({report.generator})</summary><pre>{report.diff}</pre></details>}
    </section>
  );
}
