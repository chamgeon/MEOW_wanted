import { memo } from 'react';
import { LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer, CartesianGrid } from 'recharts';
import type { PerfSample } from '../App';

interface Props { history: PerfSample[]; }

// memo() because this renders two SVG charts. App samples stats at 5 Hz rather
// than 60, so this is cheap now, but memo keeps an unrelated App state change
// (a slider drag, say) from redrawing both charts for nothing.
function MetricGraph({ history }: Props) {
  const data = history.map((s, i) => ({
    t:     i,
    fps:   +s.fps.toFixed(1),
    sim:   +s.simMs.toFixed(2),
    frame: +s.frameMs.toFixed(2),
  }));

  return (
    <div className="metric-graph">
      <h3>Browser FPS</h3>
      <ResponsiveContainer width="100%" height={90}>
        <LineChart data={data} margin={{ top: 2, right: 4, left: 0, bottom: 2 }}>
          <CartesianGrid strokeDasharray="3 3" stroke="#222" />
          <XAxis dataKey="t" hide />
          <YAxis domain={[0, 120]} width={28} stroke="#555" tick={{ fontSize: 10 }} />
          <Tooltip contentStyle={{ background: '#12121a', border: '1px solid #333', fontSize: 11 }} />
          <Line type="monotone" dataKey="fps" stroke="#4caf50" dot={false} isAnimationActive={false} strokeWidth={1.5} />
        </LineChart>
      </ResponsiveContainer>

      {/* Both series on one axis: the gap between them IS the story. "sim" is
          the C++ kernel, "frame" is everything the browser spends per frame.
          Toggling BruteForce/QuadTree should move "sim" by an order of
          magnitude while "frame" barely shifts until sim becomes dominant. */}
      <h3 style={{ marginTop: '0.4rem' }}>Sim vs Frame (ms)</h3>
      <ResponsiveContainer width="100%" height={90}>
        <LineChart data={data} margin={{ top: 2, right: 4, left: 0, bottom: 2 }}>
          <CartesianGrid strokeDasharray="3 3" stroke="#222" />
          <XAxis dataKey="t" hide />
          <YAxis width={28} stroke="#555" tick={{ fontSize: 10 }} />
          <Tooltip contentStyle={{ background: '#12121a', border: '1px solid #333', fontSize: 11 }} />
          <Line type="monotone" dataKey="frame" stroke="#666"    dot={false} isAnimationActive={false} strokeWidth={1} />
          <Line type="monotone" dataKey="sim"   stroke="#f5a623" dot={false} isAnimationActive={false} strokeWidth={1.5} />
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}

export default memo(MetricGraph);
