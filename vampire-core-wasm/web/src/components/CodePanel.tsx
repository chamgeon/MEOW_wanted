import { useState } from 'react';
import type { PerfSample, CollisionMode, MemoryMode } from '../App';

interface Props {
  stats?:        PerfSample;
  collisionMode: CollisionMode;
  memoryMode:    MemoryMode;
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
};

export default function CodePanel({ stats, collisionMode, memoryMode }: Props) {
  const [analysis, setAnalysis] = useState('');
  const [loading,  setLoading]  = useState(false);

  const snippet = SNIPPETS[collisionMode][memoryMode];

  const runAnalysis = async () => {
    if (!stats) return;
    setLoading(true);
    setAnalysis('');
    try {
      const res = await fetch('/api/optimize', {
        method:  'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          fps: stats.fps, frameTimeMs: stats.simMs,
          entityCount: stats.entities, collisionMode, memoryMode,
          codeSnippet: snippet,
        }),
      });
      if (!res.ok) throw new Error(await res.text());
      const data = await res.json();
      setAnalysis(data.analysis);
    } catch (e: unknown) {
      setAnalysis(`Error: ${e instanceof Error ? e.message : 'unreachable backend'}`);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="code-panel">
      <div className="code-panel-header">
        <h3>Active Code Path &mdash; {collisionMode} / {memoryMode}</h3>
        <button onClick={runAnalysis} disabled={loading || !stats}>
          {loading ? 'Analyzing...' : 'AI Analyze'}
        </button>
      </div>
      <pre className="code-snippet"><code>{snippet}</code></pre>
      {analysis && (
        <div className="analysis">
          <h4>AI Analysis</h4>
          <pre>{analysis}</pre>
        </div>
      )}
    </div>
  );
}
