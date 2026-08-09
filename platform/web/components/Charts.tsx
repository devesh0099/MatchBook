'use client';

// Charts, on recharts.
//
// Bundled, never fetched — the event runs on nodes we control and the editor
// has to work if the room's network does not.
//
// PALETTE. Three series, and one of them is deliberately achromatic: p50 is the
// ranked number and takes --app-ink, with p95 and p99 in the design's blue and
// accent. Run through scripts/validate_palette.js rather than eyeballed:
//
//   light  #201e1d / #1f4f8f / #ec3013   CVD ΔE 20.9 · normal 22.8 · all ≥3:1
//   dark   #ece9e9 / #7fb0ee / #ff7156   CVD ΔE 19.1 · normal 21.7 · all ≥3:1
//
// Both report FAIL on two checks, and both failures are the ink series: it sits
// outside the categorical lightness band and under the chroma floor, because it
// is a neutral, not a hue. Those checks exist to stop someone picking a hue too
// dark or too grey to read AS a hue in a set of three; a deliberate
// ink-plus-two-hues set is not what they are aimed at. Everything that governs
// whether the lines can be told apart passes with room to spare — ΔE ~20
// against a floor of 8 — and identity never rests on colour alone: every series
// is in the legend and named in its tooltip.
//
// Colours are CSS custom properties, so both themes come from one definition
// and the charts follow the theme toggle without re-rendering through JS.

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
import type { Submission } from '@/lib/api';

const SERIES = {
  p50: 'var(--app-ink)',
  p95: 'var(--s-bench)',
  p99: 'var(--color-accent)',
} as const;

const GRID = 'var(--app-line)';
const FAINT = 'var(--app-ink-3)';
const MUTED = 'var(--app-ink-2)';

export function fmtNs(v: number): string {
  if (!isFinite(v)) return '—';
  if (v >= 1_000_000) return `${(v / 1_000_000).toFixed(1)}ms`;
  if (v >= 1000) return `${(v / 1000).toFixed(1)}µs`;
  if (v >= 100) return `${v.toFixed(0)}ns`;
  return `${v.toFixed(1)}ns`;
}

const axis = {
  stroke: FAINT,
  fontSize: 11,
  tickLine: false,
  fontFamily: 'ui-monospace, Menlo, monospace',
} as const;

const tooltipStyle = {
  contentStyle: {
    background: 'var(--app-bg)',
    border: '1px solid var(--app-rule)',
    borderRadius: 0,
    fontSize: 11.5,
    fontFamily: 'ui-monospace, Menlo, monospace',
    color: 'var(--app-ink)',
  },
  labelStyle: { color: MUTED, marginBottom: 4 },
  itemStyle: { color: 'var(--app-ink)' },
} as const;

const legendStyle = {
  fontSize: 11,
  fontFamily: 'ui-monospace, Menlo, monospace',
  color: MUTED,
} as const;

function Plot({ height, children }: { height: number; children: React.ReactElement }) {
  return (
    <div style={{ width: '100%', height }}>
      <ResponsiveContainer width="100%" height="100%">
        {children}
      </ResponsiveContainer>
    </div>
  );
}

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
  height = 300,
}: {
  percentiles: Percentile[];
  probeCostNs?: number | null;
  height?: number;
}) {
  if (!percentiles || percentiles.length < 4) return null;

  // x is 1/(1-Percentile), computed by the harness from the same column the
  // library prints, so the frontend plots the library's numbers rather than
  // re-deriving them.
  const data = percentiles
    // p50 leftward is body, not tail; the plot starts where the axis is
    // meaningful. p100 has no finite 1/(1-p), so it is omitted rather than
    // given a position it does not have.
    .filter((d) => d.p >= 50 && d.p < 100 && (d.inv ?? 0) >= 2)
    .map((d) => ({ x: d.inv as number, ns: d.ns, p: d.p, count: d.count ?? 0 }));
  if (data.length < 4) return null;

  // Explicit ticks at the percentiles people actually reason about.
  //
  // An earlier formatter derived the label from the tick value with
  // 100 - 100/x, which collapses to "100.00" for everything past p99.99 — so
  // the axis printed the same label twice. Percentile and position are now
  // stated together instead of one being recovered from the other.
  const maxX = data[data.length - 1].x;
  const TICKS: { at: number; label: string }[] = [
    { at: 2, label: 'p50' },
    { at: 20, label: 'p95' },
    { at: 100, label: 'p99' },
    { at: 1000, label: 'p99.9' },
    { at: 10000, label: 'p99.99' },
    { at: 100000, label: 'p99.999' },
  ];
  const shown = TICKS.filter((t) => t.at <= maxX);
  const tickLabel = new Map(shown.map((t) => [t.at, t.label]));

  return (
    <Plot height={height}>
      <LineChart data={data} margin={{ top: 8, right: 16, bottom: 20, left: 4 }}>
        <CartesianGrid stroke={GRID} strokeDasharray="0" vertical={false} />
        <XAxis
          dataKey="x"
          type="number"
          scale="log"
          domain={[2, maxX]}
          ticks={shown.map((t) => t.at)}
          tickFormatter={(v: number) => tickLabel.get(v) ?? ''}
          {...axis}
        />
        <YAxis scale="log" domain={['auto', 'auto']} tickFormatter={fmtNs} width={58} {...axis} />
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
            stroke={FAINT}
            strokeDasharray="5 4"
            label={{
              value: `probe ${fmtNs(probeCostNs)}`,
              position: 'insideTopRight',
              fill: FAINT,
              fontSize: 10,
            }}
          />
        )}
        <Line
          type="monotone"
          dataKey="ns"
          stroke={SERIES.p50}
          strokeWidth={2.5}
          dot={false}
          isAnimationActive={false}
          name="latency"
        />
      </LineChart>
    </Plot>
  );
}

// ---------------------------------------------------------------- timeline

export interface TimelinePoint {
  t: number;       // 0..1 through the timed region
  event: number;   // first event index of this window
  p50_ns: number;
  p95_ns: number;
  p99_ns: number;
}

/**
 * Latency against time WITHIN the median run.
 *
 * Each point is the p50/p95/p99 of one window of events, so this shows how the
 * engine behaved as the run progressed — a p99 that climbs is an allocator
 * growing or a level that never gets cleaned up, and a flat p50 with a spiky
 * p99 is a different animal from both being flat. One number per run cannot say
 * any of that.
 */
export function RunTimeline({ points, height = 230 }: { points: TimelinePoint[]; height?: number }) {
  if (!points || points.length < 3) return null;

  const data = points.map((d) => ({
    pct: Math.round(d.t * 100),
    p50: d.p50_ns,
    p95: d.p95_ns,
    p99: d.p99_ns,
  }));

  return (
    <Plot height={height}>
      <LineChart data={data} margin={{ top: 8, right: 14, bottom: 8, left: 4 }}>
        <CartesianGrid stroke={GRID} vertical={false} />
        <XAxis dataKey="pct" tickFormatter={(v: number) => `${v}%`} {...axis} />
        <YAxis scale="log" domain={['auto', 'auto']} tickFormatter={fmtNs} width={54} {...axis} />
        <Tooltip
          {...tooltipStyle}
          formatter={((v: unknown, n: unknown) => [fmtNs(Number(v)), String(n)]) as never}
          labelFormatter={(l) => `${l}% through the run`}
        />
        <Legend wrapperStyle={legendStyle} />
        <Line type="monotone" dataKey="p50" name="p50" stroke={SERIES.p50} strokeWidth={2.2} dot={false} isAnimationActive={false} />
        <Line type="monotone" dataKey="p95" name="p95" stroke={SERIES.p95} strokeWidth={1.8} dot={false} isAnimationActive={false} />
        <Line type="monotone" dataKey="p99" name="p99" stroke={SERIES.p99} strokeWidth={1.8} dot={false} isAnimationActive={false} />
      </LineChart>
    </Plot>
  );
}

// ---------------------------------------------------------------- history

/**
 * p50, p95 and p99 across a participant's own benchmarked submissions.
 *
 * One log axis, never two. They are the same measure at different percentiles,
 * so they belong on one scale — and p99 sits well above p50, which on a linear
 * axis would flatten the ranked line into the floor.
 */
export function HistorySeries({
  submissions,
  highlightId,
  height = 230,
}: {
  submissions: Submission[];
  highlightId?: number;
  height?: number;
}) {
  const points = submissions
    .filter((s) => s.p50_ns != null)
    .slice()
    .sort((a, b) => a.id - b.id)
    .map((s) => ({
      id: s.id,
      at: s.created_at
        ? new Date(s.created_at).toLocaleTimeString('en-GB', {
            hour: '2-digit',
            minute: '2-digit',
            hour12: false,
          })
        : `#${s.id}`,
      p50: s.p50_ns as number,
      p95: s.p95_ns,
      p99: s.p99_ns,
    }));

  if (points.length < 2) return null;

  return (
    <Plot height={height}>
      <LineChart data={points} margin={{ top: 8, right: 14, bottom: 8, left: 4 }}>
        <CartesianGrid stroke={GRID} vertical={false} />
        <XAxis dataKey="at" {...axis} />
        <YAxis scale="log" domain={['auto', 'auto']} tickFormatter={fmtNs} width={54} {...axis} />
        <Tooltip
          {...tooltipStyle}
          formatter={((v: unknown, n: unknown) => [fmtNs(Number(v)), String(n)]) as never}
          labelFormatter={((_l: unknown, p: readonly { payload?: { id: number; at: string } }[]) => {
            const d = p?.[0]?.payload;
            return d ? `#${d.id} · ${d.at}` : '';
          }) as never}
        />
        <Legend wrapperStyle={legendStyle} />
        {highlightId != null && points.some((p) => p.id === highlightId) && (
          // No text label: it collides with the plot's top edge, and the
          // tooltip already names the submission.
          <ReferenceLine
            x={points.find((p) => p.id === highlightId)!.at}
            stroke={FAINT}
            strokeDasharray="4 4"
          />
        )}
        <Line type="monotone" dataKey="p50" name="p50" stroke={SERIES.p50} strokeWidth={2.2} dot={{ r: 3.5 }} isAnimationActive={false} />
        <Line type="monotone" dataKey="p95" name="p95" stroke={SERIES.p95} strokeWidth={1.8} dot={{ r: 3 }} isAnimationActive={false} connectNulls />
        <Line type="monotone" dataKey="p99" name="p99" stroke={SERIES.p99} strokeWidth={1.8} dot={{ r: 3 }} isAnimationActive={false} connectNulls />
      </LineChart>
    </Plot>
  );
}
