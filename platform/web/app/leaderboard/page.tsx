'use client';

// Ranked on p50, ascending. Correctness is a gate, not a tiebreak: nothing
// reaches this table until it has passed the hidden check.
//
// The row order is the SERVER's, not ours. Equal p50s break on the earlier
// submission, and that timestamp is deliberately not in the payload — so a
// client-side sort could only ever get ties wrong. Render in the order given.
//
// The interval column is not decoration. It is the spread across the nine runs
// behind each score, and two people whose intervals overlap are not reliably
// separated by the number between them — which is worth seeing next to the rank
// rather than in a footnote. It is information, not a rule: it does not tie ranks.

import { useEffect, useState } from 'react';
import { api, type LeaderboardEntry } from '@/lib/api';
import { useIdentity } from '@/lib/identity';

const COLS = '56px minmax(0,1fr) 90px 110px 110px 110px';

export default function LeaderboardPage() {
  const { identity } = useIdentity();
  const [entries, setEntries] = useState<LeaderboardEntry[]>([]);
  const [frozen, setFrozen] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [loaded, setLoaded] = useState(false);

  useEffect(() => {
    const load = () =>
      api
        .leaderboard()
        .then((r) => {
          setEntries(r.entries);
          setFrozen(r.frozen);
          setError(null);
          setLoaded(true);
        })
        .catch((e) => {
          setError(String(e.message ?? e));
          setLoaded(true);
        });
    load();
    const t = setInterval(load, 5000);
    return () => clearInterval(t);
  }, []);

  const ranked = entries;

  return (
    <div className="page scroll-y">
      <div
        style={{
          display: 'flex',
          alignItems: 'flex-end',
          justifyContent: 'space-between',
          gap: 24,
          padding: '28px 0 16px',
          borderBottom: '2px solid var(--app-rule)',
          flexWrap: 'wrap',
        }}
      >
        <div>
          <div style={{ fontWeight: 800, fontSize: 38, letterSpacing: '-0.02em', lineHeight: 1 }}>
            Leaderboard
          </div>
          <div style={{ fontSize: 13, color: 'var(--app-ink-2)', marginTop: 8, maxWidth: '74ch' }}>
            Ranked on p50 nanoseconds per event, ascending. Equal scores rank on the earlier
            submission. Correctness is a gate, not a tiebreak.
          </div>
        </div>
        <div className="mono" style={{ fontSize: 11, color: 'var(--app-ink-3)', textAlign: 'right' }}>
          {frozen ? 'snapshot · not live' : 'live · refreshed every 5s'}
        </div>
      </div>

      {/* The freeze is the one thing on this page that must not be missed:
          the standings people see at hour five are not the final ones. */}
      {frozen && (
        <div
          style={{
            display: 'flex',
            alignItems: 'center',
            gap: 16,
            marginTop: 18,
            padding: '16px 18px',
            background: 'var(--color-accent)',
            color: '#fff',
            flexWrap: 'wrap',
          }}
        >
          <span style={{ fontWeight: 800, fontSize: 22, letterSpacing: '-0.01em', whiteSpace: 'nowrap' }}>
            Standings frozen
          </span>
          <span style={{ width: 2, alignSelf: 'stretch', background: 'rgba(255,255,255,0.5)' }} />
          <span style={{ fontSize: 14, lineHeight: 1.45, minWidth: '30ch', flex: 1 }}>
            This is a snapshot. Submissions still run and still count — you are just not being shown
            where they land. What you see below is not the final result.
          </span>
        </div>
      )}

      {error && (
        <div
          className="mono"
          style={{
            marginTop: 18,
            padding: '10px 14px',
            background: 'var(--s-fail-bg)',
            color: 'var(--s-fail)',
            fontSize: 12,
          }}
        >
          {error}
        </div>
      )}

      <div
        style={{
          marginTop: frozen ? 18 : 22,
          border: '1px solid var(--app-line)',
          opacity: frozen ? 0.82 : 1,
        }}
      >
        <div
          className="kicker"
          style={{
            display: 'grid',
            gridTemplateColumns: COLS,
            padding: '10px 16px',
            background: 'var(--app-panel)',
            borderBottom: '2px solid var(--app-rule)',
            letterSpacing: '0.12em',
            color: 'var(--app-ink-2)',
          }}
        >
          <span>Rank</span>
          <span>Participant</span>
          <span style={{ textAlign: 'right' }}>Level</span>
          <span style={{ textAlign: 'right' }}>p95 ns</span>
          <span style={{ textAlign: 'right' }}>p50 ns</span>
          <span style={{ textAlign: 'right' }}>p99 ns</span>
        </div>

        {!loaded && <div style={{ padding: 16, color: 'var(--app-ink-3)' }}>Loading…</div>}
        {loaded && ranked.length === 0 && (
          <div style={{ padding: 16, color: 'var(--app-ink-3)' }}>
            Nothing has passed the correctness gate yet.
          </div>
        )}

        {ranked.map((e, i) => {
          const me = identity?.handle === e.handle;
          return (
            <div
              key={e.handle}
              className="mono"
              style={{
                display: 'grid',
                gridTemplateColumns: COLS,
                alignItems: 'center',
                padding: '9px 16px',
                borderBottom: '1px solid var(--app-line)',
                background: me ? 'var(--s-idle-bg)' : 'transparent',
                fontSize: 13,
              }}
            >
              <span
                style={{
                  fontWeight: 700,
                  color: i < 3 ? 'var(--color-accent)' : 'var(--app-ink-3)',
                }}
              >
                {String(i + 1).padStart(2, '0')}
              </span>
              <span style={{ display: 'flex', alignItems: 'center', gap: 9, minWidth: 0 }}>
                <span
                  style={{
                    fontFamily: 'var(--font-body)',
                    fontWeight: me ? 800 : i < 3 ? 700 : 500,
                    fontSize: 14,
                    overflow: 'hidden',
                    textOverflow: 'ellipsis',
                    whiteSpace: 'nowrap',
                  }}
                >
                  {e.handle}
                </span>
                {me && <span style={{ fontSize: 10, color: 'var(--app-ink-3)' }}>you</span>}
              </span>
              <span style={{ textAlign: 'right', fontWeight: 700, fontSize: 15 }}>
                {e.max_level}
              </span>
              <span style={{ textAlign: 'right', fontWeight: 700 }}>
                {e.chain_p95_ns != null ? Math.round(e.chain_p95_ns) : '—'}
              </span>
              <span style={{ textAlign: 'right', color: 'var(--app-ink-2)' }}>
                {e.chain_p50_ns != null ? Math.round(e.chain_p50_ns) : '—'}
              </span>
              <span style={{ textAlign: 'right', color: 'var(--app-ink-3)' }}>
                {e.chain_p99_ns != null ? Math.round(e.chain_p99_ns) : '—'}
              </span>
            </div>
          );
        })}
      </div>

      <p
        style={{
          color: 'var(--app-ink-3)',
          fontSize: 12,
          maxWidth: '78ch',
          marginTop: 14,
          lineHeight: 1.6,
        }}
      >
        p99 is reported but never ranked: sporadic hypervisor or thermal interference contaminates a
        tail while barely moving a median over millions of events, so ranking on it would resolve
        the closest pairs with the least reliable statistic. The probe cost is the fixed overhead of
        the two <span className="mono">rdtscp</span> reads around each event — published so the
        numbers can be read honestly rather than taken as pure engine time.
      </p>
    </div>
  );
}
