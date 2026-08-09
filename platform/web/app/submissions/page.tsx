'use client';

// Everything this participant has submitted today, newest first, plus the one
// chart that answers "am I getting better".

import { useRouter } from 'next/navigation';
import { useCallback, useEffect, useMemo, useState } from 'react';
import { api, isTerminal, type Submission } from '@/lib/api';
import { useIdentity } from '@/lib/identity';
import { inkOf, stateShort } from '@/components/StateTokens';
import { HistorySeries } from '@/components/Charts';

const POLL_MS = 2000;

export default function SubmissionsPage() {
  const { identity, ready } = useIdentity();
  const router = useRouter();
  const [subs, setSubs] = useState<Submission[]>([]);
  const [loaded, setLoaded] = useState(false);

  const load = useCallback(() => {
    if (!identity) return;
    api
      .mine(identity.id)
      .then((r) => {
        setSubs(r);
        setLoaded(true);
      })
      .catch(() => setLoaded(true));
  }, [identity]);

  useEffect(load, [load]);

  const moving = useMemo(() => subs.some((s) => !isTerminal(s.state)), [subs]);
  useEffect(() => {
    if (!identity) return;
    const t = setInterval(load, moving ? POLL_MS : 15000);
    return () => clearInterval(t);
  }, [identity, moving, load]);

  if (ready && !identity) {
    return (
      <div className="page" style={{ paddingTop: 40 }}>
        <div style={{ fontWeight: 800, fontSize: 22, marginBottom: 8 }}>Pick your handle</div>
        <p style={{ color: 'var(--app-ink-2)', fontSize: 14 }}>
          Choose it from the <strong>Handle</strong> menu in the top bar.
        </p>
      </div>
    );
  }

  const ranked = subs.filter((s) => s.p50_ns != null);
  const best = ranked.length ? Math.min(...ranked.map((s) => s.p50_ns as number)) : null;

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
            Your submissions
          </div>
          <div style={{ fontSize: 13, color: 'var(--app-ink-2)', marginTop: 8 }}>
            {subs.length} today · {ranked.length} benchmarked. Only your best p50 counts on the
            leaderboard.
          </div>
        </div>
        {best != null && (
          <div style={{ textAlign: 'right' }}>
            <div className="kicker">Best p50</div>
            <div className="mono" style={{ fontSize: 38, fontWeight: 700, lineHeight: 1 }}>
              {Math.round(best)}
            </div>
            <div className="mono" style={{ fontSize: 11, color: 'var(--app-ink-3)' }}>
              ns / event
            </div>
          </div>
        )}
      </div>

      {ranked.length > 1 && (
        <div style={{ marginTop: 24 }}>
          <div style={{ fontWeight: 800, fontSize: 15, marginBottom: 4 }}>Across the day</div>
          <div style={{ fontSize: 12, color: 'var(--app-ink-2)', marginBottom: 12 }}>
            Each point is a completed benchmark. p50 is the ranked number; p95 and p99 say whether
            the tail came with you.
          </div>
          <div style={{ border: '1px solid var(--app-line)', background: 'var(--app-panel)', padding: 10 }}>
            <HistorySeries submissions={subs} />
          </div>
        </div>
      )}

      <div style={{ marginTop: 28, border: '1px solid var(--app-line)' }}>
        <div
          className="kicker"
          style={{
            display: 'grid',
            gridTemplateColumns: '70px minmax(0,1fr) 120px 110px 110px',
            padding: '10px 16px',
            background: 'var(--app-panel)',
            borderBottom: '2px solid var(--app-rule)',
            letterSpacing: '0.12em',
          }}
        >
          <span>#</span>
          <span>State</span>
          <span style={{ textAlign: 'right' }}>p50 ns</span>
          <span style={{ textAlign: 'right' }}>p99 ns</span>
          <span style={{ textAlign: 'right' }}>At</span>
        </div>

        {!loaded && (
          <div style={{ padding: 16, color: 'var(--app-ink-3)' }}>Loading…</div>
        )}
        {loaded && subs.length === 0 && (
          <div style={{ padding: 16, color: 'var(--app-ink-3)' }}>
            Nothing yet. Run is unlimited; submit when the visible tests pass.
          </div>
        )}

        {subs.map((s) => (
          <button
            key={s.id}
            onClick={() => router.push(`/submissions/${s.id}`)}
            className="mono"
            style={{
              appearance: 'none',
              width: '100%',
              border: 0,
              borderBottom: '1px solid var(--app-line)',
              borderLeft: `3px solid ${inkOf(s.state)}`,
              background: 'transparent',
              color: 'var(--app-ink)',
              display: 'grid',
              gridTemplateColumns: '70px minmax(0,1fr) 120px 110px 110px',
              alignItems: 'center',
              padding: '9px 16px',
              fontSize: 13,
              cursor: 'pointer',
              textAlign: 'left',
            }}
          >
            <span style={{ color: 'var(--app-ink-3)' }}>{s.id}</span>
            <span
              style={{
                fontFamily: 'var(--font-body)',
                fontWeight: 600,
                fontSize: 13,
                color: inkOf(s.state),
                overflow: 'hidden',
                textOverflow: 'ellipsis',
                whiteSpace: 'nowrap',
              }}
            >
              {stateShort(s.state)}
              {!isTerminal(s.state) && ' …'}
            </span>
            <span style={{ textAlign: 'right', fontWeight: 700, fontSize: 15 }}>
              {s.p50_ns != null ? Math.round(s.p50_ns) : '—'}
            </span>
            <span style={{ textAlign: 'right', color: 'var(--app-ink-2)' }}>
              {s.p99_ns != null ? Math.round(s.p99_ns) : '—'}
            </span>
            <span style={{ textAlign: 'right', color: 'var(--app-ink-3)' }}>
              {s.created_at
                ? new Date(s.created_at).toLocaleTimeString('en-GB', {
                    hour: '2-digit',
                    minute: '2-digit',
                    hour12: false,
                  })
                : '—'}
            </span>
          </button>
        ))}
      </div>
    </div>
  );
}
