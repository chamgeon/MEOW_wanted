import { memo } from 'react';
import { LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer, CartesianGrid } from 'recharts';
import type { PerfSample } from '../App';

interface Props { history: PerfSample[]; }

const GRID   = '#232b36';
const AXIS   = '#4d5766';
const TOOLTIP = { background: '#0d1117', border: '1px solid #232b36', borderRadius: 6, fontSize: 11 };

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
    <section className="panel metric-graph">
      {/* Each chart is a flex row that owns a share of whatever height the
          stage column has left over, so the charts grow with the window
          instead of sitting at a fixed 90 px. ResponsiveContainer needs a
          definite parent height, which is what .chart-box provides. */}
      <div className="chart-block">
        <h3 className="panel-title">Browser FPS</h3>
        <div className="chart-box">
          <ResponsiveContainer width="100%" height="100%">
            <LineChart data={data} margin={{ top: 4, right: 6, left: 0, bottom: 2 }}>
              <CartesianGrid strokeDasharray="3 3" stroke={GRID} />
              <XAxis dataKey="t" hide />
              <YAxis domain={[0, 120]} width={30} stroke={AXIS} tick={{ fontSize: 10, fill: AXIS }} />
              <Tooltip contentStyle={TOOLTIP} />
              <Line type="monotone" dataKey="fps" stroke="#3fb950" dot={false} isAnimationActive={false} strokeWidth={1.5} />
            </LineChart>
          </ResponsiveContainer>
        </div>
      </div>

      {/* Both series on one axis: the gap between them IS the story. "sim" is
          the C++ kernel, "frame" is everything the browser spends per frame.
          Toggling BruteForce/QuadTree should move "sim" by an order of
          magnitude while "frame" barely shifts until sim becomes dominant. */}
      <div className="chart-block">
        <h3 className="panel-title">Sim vs Frame (ms)</h3>
        <div className="chart-box">
          <ResponsiveContainer width="100%" height="100%">
            <LineChart data={data} margin={{ top: 4, right: 6, left: 0, bottom: 2 }}>
              <CartesianGrid strokeDasharray="3 3" stroke={GRID} />
              <XAxis dataKey="t" hide />
              <YAxis width={30} stroke={AXIS} tick={{ fontSize: 10, fill: AXIS }} />
              <Tooltip contentStyle={TOOLTIP} />
              <Line type="monotone" dataKey="frame" stroke="#8b949e" dot={false} isAnimationActive={false} strokeWidth={1} />
              <Line type="monotone" dataKey="sim"   stroke="#f5a623" dot={false} isAnimationActive={false} strokeWidth={1.5} />
            </LineChart>
          </ResponsiveContainer>
        </div>
      </div>
    </section>
  );
}

export default memo(MetricGraph);
