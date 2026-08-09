'use client';

// Charts, on recharts.
//
// Bundled, never fetched — the event runs on three nodes we control and the
// editor has to work if the room's network does not.
//
// Palette: categorical slots 1-3 stepped for this dark surface (#12151a) and
// validated all-pairs rather than eyeballed — worst CVD deltaE 9.4, worst
// normal-vision 20.9, all three above 3:1 contrast. Identity never rests on
// colour alone: every series is in the legend and in the tooltip by name.

import {
  CartesianGrid,
  Legend,
  Line,
  LineChart,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts';

const SERIES = {
  p50: '#3987e5', // blue   — the ranked number
  p95: '#d95926', // orange
  p99: '#199e70', // aqua
} as const;

const GRID = '#232932';
const FAINT = '#5d6674';
const MUTED = '#8b95a5';
const SURFACE = '#12151a';

export function fmtNs(v: number): string {
  if (!isFinite(v)) return '—';
  if (v >= 1_000_000) return `${(v / 1_000_000).toFixed(1)}ms`;
  if (v >= 1000) return `${(v / 1000).toFixed(1)}µs`;
  if (v >= 100) return `${v.toFixed(0)}ns`;
  return `${v.toFixed(1)}ns`;
}

const axis = { stroke: FAINT, fontSize: 10, tickLine: false } as const;

function Frame({
  title,
  note,
  height,
  children,
}: {
  title: string;
  note?: string;
  height: number;
  children: React.ReactElement;
}) {
  return (
    <figure style={{ margin: 0 }}>
      <figcaption style={{ marginBottom: 6 }}>
        <div style={{ fontSize: 12.5, fontWeight: 600 }}>{title}</div>
        {note && (
          <div style={{ fontSize: 11.5, color: FAINT, marginTop: 2, lineHeight: 1.45 }}>{note}</div>
        )}
      </figcaption>
      <div style={{ width: '100%', height }}>
        <ResponsiveContainer width="100%" height="100%">
          {children}
        </ResponsiveContainer>
      </div>
    </figure>
  );
}

const tooltipStyle = {
  contentStyle: {
    background: SURFACE,
    border: `1px solid ${GRID}`,
    borderRadius: 6,
    fontSize: 11.5,
    fontFamily: 'ui-monospace, Menlo, monospace',
  },
  labelStyle: { color: MUTED, marginBottom: 4 },
} as const;

// ---------------------------------------------------------------- HdrHistogram

/// One row of the HdrHistogram CLASSIC distribution, straight from
/// hdr_percentiles_print's columns.
export interface Percentile {
  p: number;       // Percentile, 0..100
  ns: number;      // Value (highest_equivalent_value)
  count?: number;  // TotalCount — cumulative samples at or below
  inv?: number;    // 1/(1-Percentile) — the x axis of the standard plot
}

/**
 * The canonical HdrHistogram percentile distribution.
 *
 * x is 1/(1-p) on a log scale, which is what makes this plot the standard one:
 * p90, p99, p99.9 and p99.99 land at even spacing, so the tail gets as much
 * room as the body. A linear percentile axis buries everything past p99 in the
 * last few pixels, and the tail is where an order book's allocator and level
 * cleanup actually show up.
 */
export function HdrHistogram({
  percentiles,
  probeCostNs,
}: {
  percentiles: Percentile[];
  probeCostNs?: number | null;
}) {
  if (!percentiles || percentiles.length < 4) return null;

  // x is 1/(1-Percentile), computed by the harness from the same column the
  // library prints, so the frontend plots the library's numbers rather than
  // re-deriving them.
  const data = percentiles
    .filter((d) => d.p < 100 && (d.inv ?? 0) > 0)
    .map((d) => ({ x: d.inv as number, ns: d.ns, p: d.p, count: d.count ?? 0 }));
  if (data.length < 4) return null;

  const maxX = data[data.length - 1].x;
  const ticks: number[] = [];
  for (let t = 1; t <= maxX * 1.001; t *= 10) ticks.push(t);

  return (
    <Frame
      title="Latency distribution (HdrHistogram)"
      note="The percentile distribution from the run whose p50 is your score, plotted the standard way: x is 1/(1-percentile) on a log scale, so each step right is one more nine and the tail gets the same room as the body."
      height={230}
    >
      <LineChart data={data} margin={{ top: 6, right: 14, bottom: 22, left: 4 }}>
        <CartesianGrid stroke={GRID} strokeDasharray="0" vertical={false} />
        <XAxis
          dataKey="x"
          type="number"
          scale="log"
          domain={[1, 'dataMax']}
          ticks={ticks}
          tickFormatter={(v: number) => (v === 1 ? 'p0' : `p${(100 - 100 / v).toFixed(v >= 1000 ? 2 : v >= 100 ? 1 : 0)}`)}
          {...axis}
          label={{ value: 'percentile', position: 'insideBottom', offset: -12, fill: FAINT, fontSize: 10 }}
        />
        <YAxis
          scale="log"
          domain={['auto', 'auto']}
          tickFormatter={fmtNs}
          width={54}
          {...axis}
        />
        <Tooltip
          {...tooltipStyle}
          formatter={((v: unknown) => [fmtNs(Number(v)), 'latency']) as never}
          labelFormatter={((_l: unknown, p: readonly { payload?: Percentile }[]) => {
            const d = p?.[0]?.payload;
            if (!d) return '';
            const pct = d.p >= 99.99 ? d.p.toFixed(4) : d.p.toFixed(2);
            return `p${pct}  ·  ${(d.count ?? 0).toLocaleString()} samples at or below`;
          }) as never}
        />
        {probeCostNs != null && probeCostNs > 0 && (
          <ReferenceLine
            y={probeCostNs}
            stroke={MUTED}
            strokeDasharray="3 3"
            label={{
              value: `probe ${fmtNs(probeCostNs)}`,
              position: 'insideTopRight',
              fill: MUTED,
              fontSize: 10,
            }}
          />
        )}
        <Line
          type="monotone"
          dataKey="ns"
          stroke={SERIES.p50}
          strokeWidth={2}
          dot={false}
          isAnimationActive={false}
          name="latency"
        />
      </LineChart>
    </Frame>
  );
}

// ---------------------------------------------------------------- per-run

export interface RunPoint {
  p50_ns: number;
  p95_ns?: number | null;
  p99_ns?: number | null;
  discarded?: boolean;
}

/**
 * p50, p95 and p99 across the ranked runs of one submission.
 *
 * The headline score is the median of the p50 line. Seeing all nine is what
 * distinguishes a stable engine from one whose first run costs 3x — same
 * reported number, different thing.
 */
export function PerRunSeries({ runs, medianNs }: { runs: RunPoint[]; medianNs?: number | null }) {
  if (!runs || runs.length < 2) return null;

  const data = runs
    .filter((r) => !r.discarded)
    .map((r, i) => ({
      run: i + 1,
      p50: r.p50_ns,
      p95: r.p95_ns ?? null,
      p99: r.p99_ns ?? null,
    }));
  if (data.length < 2) return null;

  return (
    <Frame
      title="Across the ranked runs"
      note="The same stream, measured repeatedly. Your score is the median of the p50 line — a lone high point is usually a cold run, which the median absorbs."
      height={220}
    >
      <LineChart data={data} margin={{ top: 6, right: 14, bottom: 18, left: 4 }}>
        <CartesianGrid stroke={GRID} vertical={false} />
        <XAxis
          dataKey="run"
          {...axis}
          label={{ value: 'run', position: 'insideBottom', offset: -8, fill: FAINT, fontSize: 10 }}
        />
        <YAxis scale="log" domain={['auto', 'auto']} tickFormatter={fmtNs} width={54} {...axis} />
        <Tooltip
          {...tooltipStyle}
          formatter={((v: unknown, n: unknown) => [fmtNs(Number(v)), String(n)]) as never}
          labelFormatter={(l) => `run ${l}`}
        />
        <Legend wrapperStyle={{ fontSize: 11, color: MUTED }} />
        {medianNs != null && (
          <ReferenceLine
            y={medianNs}
            stroke={SERIES.p50}
            strokeDasharray="4 4"
            label={{
              value: `score ${fmtNs(medianNs)}`,
              position: 'insideTopLeft',
              fill: SERIES.p50,
              fontSize: 10,
            }}
          />
        )}
        <Line type="monotone" dataKey="p50" name="p50 (ranked)" stroke={SERIES.p50} strokeWidth={2} dot={{ r: 3 }} isAnimationActive={false} />
        <Line type="monotone" dataKey="p95" name="p95" stroke={SERIES.p95} strokeWidth={2} dot={{ r: 3 }} isAnimationActive={false} connectNulls />
        <Line type="monotone" dataKey="p99" name="p99" stroke={SERIES.p99} strokeWidth={2} dot={{ r: 3 }} isAnimationActive={false} connectNulls />
      </LineChart>
    </Frame>
  );
}

// ---------------------------------------------------------------- history

export interface HistoryPoint {
  id: number;
  p50: number;
  p95: number | null;
  p99: number | null;
}

/**
 * p50, p95 and p99 across a participant's own benchmarked submissions.
 *
 * One log axis, never two. They are the same measure at different percentiles,
 * so they belong on one scale — and p99 sits an order of magnitude above p50,
 * which on a linear axis would flatten the ranked line into the floor.
 */
export function HistorySeries({ points }: { points: HistoryPoint[] }) {
  if (!points || points.length < 2) return null;

  return (
    <Frame
      title="Your latency across submissions"
      note="Oldest to newest, benchmarked submissions only. p50 is the ranked number; p95 and p99 are reported colour."
      height={240}
    >
      <LineChart data={points} margin={{ top: 6, right: 14, bottom: 18, left: 4 }}>
        <CartesianGrid stroke={GRID} vertical={false} />
        <XAxis
          dataKey="id"
          tickFormatter={(v: number) => `#${v}`}
          {...axis}
          label={{ value: 'submission', position: 'insideBottom', offset: -8, fill: FAINT, fontSize: 10 }}
        />
        <YAxis scale="log" domain={['auto', 'auto']} tickFormatter={fmtNs} width={54} {...axis} />
        <Tooltip
          {...tooltipStyle}
          formatter={((v: unknown, n: unknown) => [fmtNs(Number(v)), String(n)]) as never}
          labelFormatter={(l) => `submission #${l}`}
        />
        <Legend wrapperStyle={{ fontSize: 11, color: MUTED }} />
        <Line type="monotone" dataKey="p50" name="p50 (ranked)" stroke={SERIES.p50} strokeWidth={2} dot={{ r: 3.5 }} isAnimationActive={false} />
        <Line type="monotone" dataKey="p95" name="p95" stroke={SERIES.p95} strokeWidth={2} dot={{ r: 3.5 }} isAnimationActive={false} connectNulls />
        <Line type="monotone" dataKey="p99" name="p99" stroke={SERIES.p99} strokeWidth={2} dot={{ r: 3.5 }} isAnimationActive={false} connectNulls />
      </LineChart>
    </Frame>
  );
}
