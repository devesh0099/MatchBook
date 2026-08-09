'use client';

// Inline SVG, no chart library.
//
// The event runs on three nodes we control and the editor must work if the
// room's network does not, so nothing here loads from anywhere. It also keeps
// every mark spec — stroke widths, marker sizes, the surface ring on
// overlapping dots — under our control rather than a library's defaults.
//
// Palette: categorical slots 1-3 stepped for the dark surface (#12151a),
// validated all-pairs — worst CVD deltaE 9.4, worst normal-vision 20.9, all
// three above 3:1 contrast. Series identity is never carried by colour alone:
// every series is directly labelled and the legend is always present.

import { useState } from 'react';

const SERIES = {
  p50: '#3987e5', // blue   — the ranked number
  p95: '#d95926', // orange
  p99: '#199e70', // aqua
} as const;

const INK = 'var(--text)';
const MUTED = 'var(--muted)';
const FAINT = 'var(--faint)';
const GRID = 'var(--line)';
const SURFACE = 'var(--panel)';

function fmtNs(v: number): string {
  if (v >= 10000) return `${(v / 1000).toFixed(0)}µs`;
  if (v >= 1000) return `${(v / 1000).toFixed(1)}µs`;
  if (v >= 100) return `${v.toFixed(0)}ns`;
  return `${v.toFixed(1)}ns`;
}

function ChartFrame({
  title,
  note,
  children,
}: {
  title: string;
  note?: string;
  children: React.ReactNode;
}) {
  return (
    <figure style={{ margin: 0 }}>
      <figcaption style={{ marginBottom: 8 }}>
        <div style={{ fontSize: 12.5, fontWeight: 600, color: 'var(--text)' }}>{title}</div>
        {note && (
          <div style={{ fontSize: 11.5, color: FAINT, marginTop: 2, lineHeight: 1.45 }}>{note}</div>
        )}
      </figcaption>
      {children}
    </figure>
  );
}

// ---------------------------------------------------------------- distribution

/**
 * The latency distribution, as a percentile curve.
 *
 * x is percentile on a tail-stretched scale — equal spacing between p50, p90,
 * p99, p99.9 — because that is where an order book's interesting behaviour
 * lives. A linear percentile axis squashes everything past p99 into the last
 * few pixels, which is exactly the part worth seeing.
 */
export function DistributionChart({
  percentiles,
  probeCostNs,
}: {
  percentiles: { p: number; ns: number }[];
  probeCostNs?: number | null;
}) {
  const [hover, setHover] = useState<number | null>(null);
  if (!percentiles || percentiles.length < 2) return null;

  const W = 380;
  const H = 190;
  const M = { top: 10, right: 14, bottom: 26, left: 46 };
  const iw = W - M.left - M.right;
  const ih = H - M.top - M.bottom;

  // Tail-stretched: position by -log10(1 - p/100).
  const tx = (p: number) => (p >= 100 ? 4.2 : -Math.log10(1 - p / 100));
  const xs = percentiles.map((d) => tx(d.p));
  const x0 = Math.min(...xs);
  const x1 = Math.max(...xs);
  const X = (p: number) => M.left + ((tx(p) - x0) / (x1 - x0)) * iw;

  const maxNs = Math.max(...percentiles.map((d) => d.ns));
  // Log y: a p50 of 120ns and a p100 of 194µs cannot share a linear axis.
  const ly = (v: number) => Math.log10(Math.max(v, 1));
  const y0 = ly(Math.min(...percentiles.map((d) => d.ns)) * 0.8);
  const y1 = ly(maxNs * 1.15);
  const Y = (v: number) => M.top + ih - ((ly(v) - y0) / (y1 - y0)) * ih;

  const path = percentiles.map((d, i) => `${i ? 'L' : 'M'}${X(d.p)},${Y(d.ns)}`).join('');

  const ticks = [50, 90, 99, 99.9, 100].filter((t) => t >= percentiles[0].p);
  const yTicks: number[] = [];
  for (let e = Math.floor(y0); e <= Math.ceil(y1); e++) yTicks.push(10 ** e);

  return (
    <ChartFrame
      title="Latency distribution"
      note="From the run whose p50 is your score. Percentile axis is tail-stretched — p50 to p99.9 are equally spaced — because that is where the interesting behaviour is."
    >
      <svg width="100%" viewBox={`0 0 ${W} ${H}`} role="img" style={{ display: 'block' }}>
        {yTicks.map((v) => (
          <g key={v}>
            <line x1={M.left} x2={W - M.right} y1={Y(v)} y2={Y(v)} stroke={GRID} strokeWidth="1" />
            <text x={M.left - 6} y={Y(v) + 3} textAnchor="end" fontSize="9" fill={FAINT}>
              {fmtNs(v)}
            </text>
          </g>
        ))}
        {ticks.map((t) => (
          <text key={t} x={X(t)} y={H - 8} textAnchor="middle" fontSize="9" fill={FAINT}>
            p{t}
          </text>
        ))}

        {probeCostNs != null && probeCostNs > 0 && Y(probeCostNs) > M.top && (
          <>
            <line
              x1={M.left}
              x2={W - M.right}
              y1={Y(probeCostNs)}
              y2={Y(probeCostNs)}
              stroke={MUTED}
              strokeWidth="1"
              strokeDasharray="3 3"
            />
            <text x={W - M.right} y={Y(probeCostNs) - 4} textAnchor="end" fontSize="9" fill={MUTED}>
              probe {fmtNs(probeCostNs)}
            </text>
          </>
        )}

        <path d={path} fill="none" stroke={SERIES.p50} strokeWidth="2" strokeLinejoin="round" />

        {percentiles.map((d, i) => (
          <circle
            key={d.p}
            cx={X(d.p)}
            cy={Y(d.ns)}
            r={hover === i ? 5 : 3.5}
            fill={SERIES.p50}
            stroke={SURFACE}
            strokeWidth="2"
            onMouseEnter={() => setHover(i)}
            onMouseLeave={() => setHover(null)}
            style={{ cursor: 'pointer' }}
          />
        ))}

        {hover != null && (
          <text
            x={Math.min(X(percentiles[hover].p) + 8, W - M.right - 60)}
            y={Y(percentiles[hover].ns) - 8}
            fontSize="10"
            fill={INK}
            fontWeight="600"
          >
            p{percentiles[hover].p} · {fmtNs(percentiles[hover].ns)}
          </text>
        )}
      </svg>
    </ChartFrame>
  );
}

// ---------------------------------------------------------------- per-run dots

/**
 * The nine numbers the score is the median of.
 *
 * The headline p50 hides whether it is nine tight runs or seven tight ones
 * carrying a cold outlier — and those mean different things about an engine.
 * The CI band is what makes a tie legible: two submissions whose bands overlap
 * are genuinely indistinguishable, which is the claim the bands on the
 * leaderboard are making.
 */
export function PerRunChart({
  runs,
  medianNs,
  ciLowNs,
  ciHighNs,
}: {
  runs: number[];
  medianNs: number;
  ciLowNs?: number;
  ciHighNs?: number;
}) {
  const [hover, setHover] = useState<number | null>(null);
  if (!runs || runs.length === 0) return null;

  const W = 380;
  const H = 150;
  const M = { top: 12, right: 14, bottom: 24, left: 46 };
  const iw = W - M.left - M.right;
  const ih = H - M.top - M.bottom;

  const lo = Math.min(...runs, ciLowNs ?? Infinity);
  const hi = Math.max(...runs, ciHighNs ?? -Infinity);
  const pad = Math.max((hi - lo) * 0.25, hi * 0.04, 1);
  const Y = (v: number) => M.top + ih - ((v - (lo - pad)) / (hi - lo + 2 * pad)) * ih;
  const X = (i: number) => M.left + ((i + 0.5) / runs.length) * iw;

  const spread = lo > 0 ? ((hi - lo) / lo) * 100 : 0;

  return (
    <ChartFrame
      title={`The ${runs.length} runs behind the score`}
      note={`Score is the median of these. Spread ${spread.toFixed(1)}%. A single high dot is usually a cold run — the median absorbs it, which is why the score is a median.`}
    >
      <svg width="100%" viewBox={`0 0 ${W} ${H}`} role="img" style={{ display: 'block' }}>
        {[lo, medianNs, hi].map((v, i) => (
          <g key={i}>
            <line x1={M.left} x2={W - M.right} y1={Y(v)} y2={Y(v)} stroke={GRID} strokeWidth="1" />
            <text x={M.left - 6} y={Y(v) + 3} textAnchor="end" fontSize="9" fill={FAINT}>
              {fmtNs(v)}
            </text>
          </g>
        ))}

        {ciLowNs != null && ciHighNs != null && ciHighNs > ciLowNs && (
          <rect
            x={M.left}
            y={Y(ciHighNs)}
            width={iw}
            height={Math.max(Y(ciLowNs) - Y(ciHighNs), 1)}
            fill={SERIES.p50}
            opacity="0.12"
          />
        )}

        <line
          x1={M.left}
          x2={W - M.right}
          y1={Y(medianNs)}
          y2={Y(medianNs)}
          stroke={SERIES.p50}
          strokeWidth="2"
        />
        <text x={W - M.right} y={Y(medianNs) - 5} textAnchor="end" fontSize="9.5" fill={INK} fontWeight="600">
          median {fmtNs(medianNs)}
        </text>

        {runs.map((v, i) => (
          <circle
            key={i}
            cx={X(i)}
            cy={Y(v)}
            r={hover === i ? 6 : 4.5}
            fill={SERIES.p50}
            stroke={SURFACE}
            strokeWidth="2"
            onMouseEnter={() => setHover(i)}
            onMouseLeave={() => setHover(null)}
            style={{ cursor: 'pointer' }}
          />
        ))}

        {hover != null && (
          <text
            x={Math.min(X(hover) + 9, W - M.right - 70)}
            y={Y(runs[hover]) - 9}
            fontSize="10"
            fill={INK}
            fontWeight="600"
          >
            run {hover + 1} · {fmtNs(runs[hover])}
          </text>
        )}

        <text x={M.left} y={H - 7} fontSize="9" fill={FAINT}>
          run 1
        </text>
        <text x={W - M.right} y={H - 7} textAnchor="end" fontSize="9" fill={FAINT}>
          run {runs.length}
        </text>
      </svg>
    </ChartFrame>
  );
}

// ---------------------------------------------------------------- time series

export interface HistoryPoint {
  id: number;
  p50: number;
  p95: number | null;
  p99: number | null;
}

/**
 * p50, p95 and p99 across a participant's own benchmarked submissions.
 *
 * One y-axis, never two: p50 and p99 are the same measure at different
 * percentiles, so they belong on one scale. It is log, because p99 sits an
 * order of magnitude above p50 and a linear axis would flatten the p50 line
 * into the floor — which is the line that actually decides the ranking.
 */
export function HistoryChart({ points }: { points: HistoryPoint[] }) {
  const [hover, setHover] = useState<number | null>(null);
  if (!points || points.length < 2) return null;

  const W = 380;
  const H = 200;
  const M = { top: 12, right: 52, bottom: 28, left: 46 };
  const iw = W - M.left - M.right;
  const ih = H - M.top - M.bottom;

  const all = points.flatMap((d) => [d.p50, d.p95, d.p99].filter((v): v is number => v != null));
  const ly = (v: number) => Math.log10(Math.max(v, 1));
  const y0 = ly(Math.min(...all) * 0.75);
  const y1 = ly(Math.max(...all) * 1.3);
  const Y = (v: number) => M.top + ih - ((ly(v) - y0) / (y1 - y0)) * ih;
  const X = (i: number) => M.left + (points.length === 1 ? iw / 2 : (i / (points.length - 1)) * iw);

  const yTicks: number[] = [];
  for (let e = Math.floor(y0); e <= Math.ceil(y1); e++) yTicks.push(10 ** e);

  const series: { key: keyof typeof SERIES; label: string; get: (d: HistoryPoint) => number | null }[] =
    [
      { key: 'p50', label: 'p50', get: (d) => d.p50 },
      { key: 'p95', label: 'p95', get: (d) => d.p95 },
      { key: 'p99', label: 'p99', get: (d) => d.p99 },
    ];

  return (
    <ChartFrame
      title="Your latency across submissions"
      note="Oldest to newest. p50 is the ranked number; p95 and p99 are colour only. One log axis — p99 sits an order of magnitude above p50, and a linear scale would flatten the line that decides the ranking."
    >
      <svg width="100%" viewBox={`0 0 ${W} ${H}`} role="img" style={{ display: 'block' }}>
        {yTicks.map((v) => (
          <g key={v}>
            <line x1={M.left} x2={W - M.right} y1={Y(v)} y2={Y(v)} stroke={GRID} strokeWidth="1" />
            <text x={M.left - 6} y={Y(v) + 3} textAnchor="end" fontSize="9" fill={FAINT}>
              {fmtNs(v)}
            </text>
          </g>
        ))}

        {hover != null && (
          <line
            x1={X(hover)}
            x2={X(hover)}
            y1={M.top}
            y2={M.top + ih}
            stroke={MUTED}
            strokeWidth="1"
            strokeDasharray="3 3"
          />
        )}

        {series.map((s) => {
          const pts = points.map((d, i) => ({ i, v: s.get(d) })).filter((p) => p.v != null);
          if (pts.length < 1) return null;
          const path = pts.map((p, k) => `${k ? 'L' : 'M'}${X(p.i)},${Y(p.v as number)}`).join('');
          const last = pts[pts.length - 1];
          return (
            <g key={s.key}>
              <path d={path} fill="none" stroke={SERIES[s.key]} strokeWidth="2" strokeLinejoin="round" />
              {pts.map((p) => (
                <circle
                  key={p.i}
                  cx={X(p.i)}
                  cy={Y(p.v as number)}
                  r={hover === p.i ? 5 : 3}
                  fill={SERIES[s.key]}
                  stroke={SURFACE}
                  strokeWidth="1.5"
                />
              ))}
              {/* Direct label: identity is never carried by colour alone. */}
              <text
                x={X(last.i) + 6}
                y={Y(last.v as number) + 3}
                fontSize="9.5"
                fill={SERIES[s.key]}
                fontWeight="600"
              >
                {s.label}
              </text>
            </g>
          );
        })}

        {points.map((d, i) => (
          <rect
            key={d.id}
            x={X(i) - iw / (points.length * 2 || 1)}
            y={M.top}
            width={Math.max(iw / points.length, 10)}
            height={ih}
            fill="transparent"
            onMouseEnter={() => setHover(i)}
            onMouseLeave={() => setHover(null)}
          />
        ))}

        <text x={M.left} y={H - 8} fontSize="9" fill={FAINT}>
          oldest
        </text>
        <text x={W - M.right} y={H - 8} textAnchor="end" fontSize="9" fill={FAINT}>
          newest
        </text>
      </svg>

      {hover != null && (
        <div className="mono" style={{ fontSize: 11, color: MUTED, marginTop: 4 }}>
          #{points[hover].id} · p50 {fmtNs(points[hover].p50)}
          {points[hover].p95 != null && ` · p95 ${fmtNs(points[hover].p95 as number)}`}
          {points[hover].p99 != null && ` · p99 ${fmtNs(points[hover].p99 as number)}`}
        </div>
      )}

      <div style={{ display: 'flex', gap: 14, marginTop: 6, fontSize: 11, color: MUTED }}>
        {series.map((s) => (
          <span key={s.key} style={{ display: 'inline-flex', alignItems: 'center', gap: 5 }}>
            <span
              style={{ width: 9, height: 2.5, background: SERIES[s.key], borderRadius: 1 }}
              aria-hidden
            />
            {s.label}
            {s.key === 'p50' && <span style={{ color: FAINT }}>(ranked)</span>}
          </span>
        ))}
      </div>
    </ChartFrame>
  );
}
