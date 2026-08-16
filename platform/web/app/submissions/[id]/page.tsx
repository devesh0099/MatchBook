'use client';

// The submission detail page, phase by phase (interim M3 version — M6 gives
// the ladder its full visual treatment). Owner-only: the API returns 404 for
// anyone else's submission, so this page can only ever show your own.
//
// One scoring chain everywhere: p95, then p50, then p99. Deadlines are
// gates, never scores, and a failed phase renders the diagnostics for
// exactly that phase — failing at level k with 61% processed tells you more
// than any single number.

import Link from 'next/link';
import { useParams } from 'next/navigation';
import { useEffect, useState } from 'react';
import { api, stateLabel, type Submission } from '@/lib/api';
import { bgOf, inkOf, stateBlurb } from '@/components/StateTokens';
import SubmissionAnalytics from '@/components/SubmissionAnalytics';

const POLL_MS = 2000;

export default function SubmissionPage() {
  const params = useParams<{ id: string }>();
  const id = Number(params.id);
  const [sub, setSub] = useState<Submission | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!Number.isFinite(id)) return;
    let stop = false;
    let timer: ReturnType<typeof setTimeout> | null = null;
    const tick = () => {
      api
        .submission(id)
        .then((r) => {
          if (stop) return;
          setSub(r.submission);
          setError(null);
          if (!r.terminal) timer = setTimeout(tick, POLL_MS);
        })
        .catch((e) => {
          if (stop) return;
          setError(String((e as Error).message ?? e));
          timer = setTimeout(tick, POLL_MS * 2);
        });
    };
    tick();
    return () => {
      stop = true;
      if (timer) clearTimeout(timer);
    };
  }, [id]);

  if (error && !sub) {
    return (
      <div style={{ padding: 28 }}>
        <div className="mono" style={{ color: 'var(--s-fail)', fontSize: 13 }}>
          {error}
        </div>
        <p style={{ fontSize: 12, color: 'var(--app-ink-3)', marginTop: 10 }}>
          Submissions are visible only to their owner — check that you are signed
          in as the right handle.
        </p>
      </div>
    );
  }
  if (!sub) return <div style={{ padding: 28, color: 'var(--app-ink-3)' }}>Loading…</div>;

  return (
    <div style={{ padding: '22px 28px', maxWidth: 980 }}>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 14 }}>
        <Link href="/editor" className="mono" style={{ fontSize: 12, color: 'var(--app-ink-3)' }}>
          ← editor
        </Link>
        <span className="mono" style={{ fontSize: 12, color: 'var(--app-ink-3)' }}>
          submission #{sub.id}
        </span>
      </div>

      <div
        style={{
          marginTop: 14,
          padding: '16px 18px',
          background: bgOf(sub.state),
          borderLeft: `3px solid ${inkOf(sub.state)}`,
        }}
      >
        <div style={{ display: 'flex', alignItems: 'baseline', gap: 16, flexWrap: 'wrap' }}>
          <span style={{ fontWeight: 800, fontSize: 18, color: inkOf(sub.state) }}>
            {stateLabel(sub.state)}
          </span>
          {sub.state === 'done' && sub.max_level != null && (
            <span className="mono" style={{ fontWeight: 800, fontSize: 18 }}>
              level {sub.max_level}
            </span>
          )}
        </div>
        <p style={{ margin: '6px 0 0', fontSize: 13, color: 'var(--app-ink-2)', maxWidth: '72ch' }}>
          {stateBlurb(sub.state)}
        </p>
      </div>

      <SubmissionAnalytics sub={sub} />

      <p style={{ color: 'var(--app-ink-3)', fontSize: 12, maxWidth: '78ch', marginTop: 16, lineHeight: 1.6 }}>
        Numbers on this page are measured on your own box and rank the live
        board. Final standings are re-measured after the contest on the golden
        box, on a sealed stream, from your last submitted code.
      </p>
    </div>
  );
}
