'use client';

// Everything this participant has submitted today, newest first. (The
// "across the day" chart returns ladder-aware in M6.)

import Link from 'next/link';
import { useRouter } from 'next/navigation';
import { useCallback, useEffect, useMemo, useState } from 'react';
import { api, isTerminal, type Submission } from '@/lib/api';
import { useIdentity } from '@/lib/identity';
import { inkOf, stateShort } from '@/components/StateTokens';

const POLL_MS = 2000;

export default function SubmissionsPage() {
  const { identity, ready } = useIdentity();
  const router = useRouter();
  const [subs, setSubs] = useState<Submission[]>([]);
  const [loaded, setLoaded] = useState(false);

  const load = useCallback(() => {
    if (!identity) return;
    api
      .mine()
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
        <div style={{ fontWeight: 800, fontSize: 22, marginBottom: 8 }}>Signed out</div>
        <p style={{ color: 'var(--app-ink-2)', fontSize: 14 }}>
          <Link href="/login">Sign in</Link> with the handle and password from your printed slip.
        </p>
      </div>
    );
  }

  const climbed = subs.filter((s) => s.max_level != null);
  const best = climbed.length ? Math.max(...climbed.map((s) => s.max_level as number)) : null;

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
            {subs.length} today · {climbed.length} climbed. Only your best climb counts on the
            leaderboard.
          </div>
        </div>
        {best != null && (
          <div style={{ textAlign: 'right' }}>
            <div className="kicker">Best level</div>
            <div className="mono" style={{ fontSize: 38, fontWeight: 700, lineHeight: 1 }}>
              {best}
            </div>
            <div className="mono" style={{ fontSize: 11, color: 'var(--app-ink-3)' }}>
              of the ladder
            </div>
          </div>
        )}
      </div>

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
          <span style={{ textAlign: 'right' }}>Level</span>
          <span style={{ textAlign: 'right' }}>p95 ns</span>
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
              {s.max_level != null ? `L${s.max_level}` : '—'}
            </span>
            <span style={{ textAlign: 'right', color: 'var(--app-ink-2)' }}>
              {s.top_p95_ns != null ? Math.round(s.top_p95_ns) : '—'}
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
