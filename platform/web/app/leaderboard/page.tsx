'use client';

// Bands against fixed thresholds, not exact positions. The correctness gate
// compresses the surviving cohort — everyone left wrote a working order book,
// so spreads are often under 2x and defending rank 7 against rank 9 is not
// honestly possible. Overlapping confidence intervals mean tied rank.

import { useEffect, useState } from 'react';
import { api, type LeaderboardEntry } from '@/lib/api';

const BAND_ORDER = ['gold', 'silver', 'bronze', 'finisher'];

export default function LeaderboardPage() {
  const [entries, setEntries] = useState<LeaderboardEntry[]>([]);
  const [frozen, setFrozen] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    const load = () =>
      api
        .leaderboard()
        .then((r) => {
          setEntries(r.entries);
          setFrozen(r.frozen);
          setError(null);
        })
        .catch((e) => setError(String(e.message ?? e)));
    load();
    const t = setInterval(load, 5000);
    return () => clearInterval(t);
  }, []);

  const byBand = BAND_ORDER.map((b) => ({ band: b, rows: entries.filter((e) => e.band === b) })).filter(
    (g) => g.rows.length > 0,
  );

  return (
    <main className="main">
      <div className="page">
        <h1>Leaderboard</h1>
        <p className="sub">
          Ranked on p50 per-order latency, taken as the median of 7–10 per-run medians. Bands, not
          positions. Overlapping intervals mean a tie, and that is the honest answer rather than a
          number that cannot be defended.
        </p>

        {frozen && (
          <div className="card tone-warn mono" style={{ marginBottom: 20 }}>
            Frozen. This is the standing as of the freeze; runs are still being measured underneath
            and the final order is revealed at the end.
          </div>
        )}

        {error && <p className="tone-bad mono">{error}</p>}

        {entries.length === 0 ? (
          <div className="card">
            <div className="empty">Nothing has passed the correctness gate yet.</div>
          </div>
        ) : (
          byBand.map((g) => (
            <section key={g.band} style={{ marginBottom: 26 }}>
              <h2 style={{ textTransform: 'capitalize' }}>{g.band}</h2>
              <div className="card" style={{ padding: 0 }}>
                <table>
                  <thead>
                    <tr>
                      <th>Handle</th>
                      <th style={{ textAlign: 'right' }}>p50</th>
                      <th style={{ textAlign: 'right' }}>95% interval</th>
                      <th style={{ textAlign: 'right' }}>p99</th>
                      <th style={{ textAlign: 'right' }}>probe</th>
                    </tr>
                  </thead>
                  <tbody>
                    {g.rows.map((e) => (
                      <tr key={e.handle}>
                        <td className="mono">{e.handle}</td>
                        <td className="num">{e.p50_ns.toFixed(1)} ns</td>
                        <td className="num" style={{ color: 'var(--faint)' }}>
                          {e.ci_low_ns.toFixed(1)} – {e.ci_high_ns.toFixed(1)}
                        </td>
                        <td className="num" style={{ color: 'var(--muted)' }}>
                          {e.p99_ns != null ? `${e.p99_ns.toFixed(1)} ns` : '—'}
                        </td>
                        <td className="num" style={{ color: 'var(--faint)' }}>
                          {e.probe_cost_ns != null ? `${e.probe_cost_ns.toFixed(1)} ns` : '—'}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </section>
          ))
        )}

        <p style={{ color: 'var(--faint)', fontSize: 12.5, maxWidth: '70ch' }}>
          p99 is reported but never ranked: sporadic hypervisor or thermal interference contaminates
          tails while barely moving a median over 10M events. The probe cost is the fixed overhead of
          the two <span className="mono">rdtscp</span> reads around each event, published so the
          numbers can be read honestly.
        </p>
      </div>
    </main>
  );
}
