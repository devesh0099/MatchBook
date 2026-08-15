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
import {
  api,
  stateLabel,
  type LadderLevel,
  type Submission,
} from '@/lib/api';
import { bgOf, inkOf, stateBlurb } from '@/components/StateTokens';

const POLL_MS = 2000;

function Chain({ p95, p50, p99 }: { p95?: number | null; p50?: number | null; p99?: number | null }) {
  const cell = (label: string, v?: number | null, strong = false) => (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
      <span className="kicker" style={{ fontWeight: 400 }}>
        {label}
      </span>
      <span className="mono" style={{ fontSize: strong ? 22 : 16, fontWeight: strong ? 800 : 600 }}>
        {v != null && v > 0 ? Math.round(v).toLocaleString() : '—'}
        <span style={{ fontSize: 11, color: 'var(--app-ink-3)', marginLeft: 4 }}>ns</span>
      </span>
    </div>
  );
  return (
    <div style={{ display: 'flex', gap: 28 }}>
      {cell('p95 · ranked first', p95, true)}
      {cell('p50', p50)}
      {cell('p99', p99)}
    </div>
  );
}

function LevelRow({ l }: { l: LadderLevel }) {
  const ok = l.outcome === 'ok';
  const pct =
    l.events > 0 && l.events_processed != null
      ? Math.round((100 * l.events_processed) / l.events)
      : null;
  return (
    <div
      className="mono"
      style={{
        display: 'grid',
        gridTemplateColumns: '64px 110px 90px 1fr',
        gap: 12,
        alignItems: 'baseline',
        padding: '8px 14px',
        borderBottom: '1px solid var(--app-line)',
        fontSize: 13,
      }}
    >
      <span style={{ fontWeight: 800 }}>L{l.level}</span>
      <span style={{ fontWeight: 700, color: ok ? 'var(--s-pass)' : 'var(--s-fail)' }}>
        {ok ? 'cleared' : l.outcome.replace(/_/g, ' ')}
      </span>
      <span style={{ color: 'var(--app-ink-2)' }}>
        {(l.events / 1_000_000).toFixed(1)}M · {(l.deadline_ms / 1000).toFixed(1)}s
      </span>
      <span style={{ color: 'var(--app-ink-2)' }}>
        {ok
          ? `p95 ${Math.round(l.p95_ns)} · p50 ${Math.round(l.p50_ns)} · p99 ${Math.round(l.p99_ns)} ns` +
            (l.wall_s != null ? ` · ${Number(l.wall_s).toFixed(2)}s` : '')
          : pct != null
            ? `${pct}% processed (${l.events_processed?.toLocaleString()} of ${l.events.toLocaleString()})` +
              (l.checked_events != null ? ` · verified through ${Number(l.checked_events).toLocaleString()}` : '')
            : (l.notes ?? '')}
      </span>
    </div>
  );
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div style={{ border: '1px solid var(--app-line)', marginTop: 18 }}>
      <div
        className="kicker"
        style={{
          padding: '9px 14px',
          background: 'var(--app-panel)',
          borderBottom: '1px solid var(--app-line)',
        }}
      >
        {title}
      </div>
      {children}
    </div>
  );
}

const pre: React.CSSProperties = {
  margin: 0,
  padding: '10px 14px',
  fontSize: 12,
  lineHeight: 1.55,
  overflowX: 'auto',
  whiteSpace: 'pre-wrap',
};

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

  const tests = sub.phase0?.tests;
  const verify = sub.phase0?.verify;
  const failing = tests?.tests?.filter((t) => !t.passed) ?? [];

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

      {sub.state === 'done' && sub.max_level != null && sub.max_level > 0 && (
        <Section title={`The chain at level ${sub.max_level} — what ranks you`}>
          <div style={{ padding: '14px 14px' }}>
            <Chain p95={sub.top_p95_ns} p50={sub.top_p50_ns} p99={sub.top_p99_ns} />
          </div>
        </Section>
      )}

      {sub.phase0?.compile?.stderr && (
        <Section title="Compiler output">
          <pre className="mono" style={{ ...pre, color: 'var(--s-fail)' }}>
            {sub.phase0.compile.stderr}
          </pre>
        </Section>
      )}

      {tests && (
        <Section title={`Phase 0 — spec tests · ${tests.passed}/${tests.total}`}>
          {failing.length === 0 ? (
            <div style={{ padding: '10px 14px', fontSize: 13, color: 'var(--s-pass)' }}>
              All visible tests passed.
            </div>
          ) : (
            failing.slice(0, 20).map((t) => (
              <div
                key={t.name}
                className="mono"
                style={{ padding: '7px 14px', borderBottom: '1px solid var(--app-line)', fontSize: 12 }}
              >
                <span style={{ color: 'var(--s-fail)', fontWeight: 700 }}>{t.name}</span>
                <span style={{ color: 'var(--app-ink-3)' }}> [{t.rule}] </span>
                <span style={{ color: 'var(--app-ink-2)' }}>{t.detail}</span>
              </div>
            ))
          )}
        </Section>
      )}

      {verify && sub.state !== 'done' && verify.outcome && verify.outcome !== 'passed' && (
        <Section title="Phase 0 — hidden verify">
          <div style={{ padding: '10px 14px', fontSize: 13 }}>
            <div className="mono" style={{ color: 'var(--s-fail)', fontWeight: 700 }}>
              {verify.outcome.replace(/_/g, ' ')}
            </div>
            {verify.message && (
              <p style={{ margin: '6px 0 0', fontSize: 12, color: 'var(--app-ink-2)' }}>{verify.message}</p>
            )}
            {verify.reproducer && verify.reproducer.length > 0 && (
              <pre className="mono" style={{ ...pre, padding: '8px 0 0' }}>
                {verify.reproducer
                  .map(
                    (e) =>
                      `#${e.seq} ${e.type} ${e.side ?? ''} ${e.tif ?? ''} px=${e.px} qty=${e.qty} session=${e.session} coid=${e.coid}`,
                  )
                  .join('\n')}
              </pre>
            )}
            {verify.stderr && (
              <pre className="mono" style={{ ...pre, color: 'var(--app-ink-3)' }}>{verify.stderr}</pre>
            )}
          </div>
        </Section>
      )}

      {sub.phase1 && (
        <Section
          title={`Phase I — bulk run · ${(sub.phase1.events / 1000).toFixed(0)}k events · gate ${(sub.phase1.deadline_ms / 1000).toFixed(1)}s`}
        >
          <div style={{ padding: '14px 14px' }}>
            {sub.phase1.outcome === 'ok' ? (
              <Chain p95={sub.phase1.p95_ns} p50={sub.phase1.p50_ns} p99={sub.phase1.p99_ns} />
            ) : (
              <div className="mono" style={{ fontSize: 13, color: 'var(--s-fail)' }}>
                {sub.phase1.outcome.replace(/_/g, ' ')}
                {sub.phase1.notes ? ` — ${sub.phase1.notes}` : ''}
              </div>
            )}
          </div>
        </Section>
      )}

      {sub.phase2 && sub.phase2.levels.length > 0 && (
        <Section title="Phase II — the ladder">
          {sub.phase2.levels.map((l) => (
            <LevelRow key={l.level} l={l} />
          ))}
        </Section>
      )}

      <p style={{ color: 'var(--app-ink-3)', fontSize: 12, maxWidth: '78ch', marginTop: 16, lineHeight: 1.6 }}>
        Numbers on this page are measured on your own box and rank the live
        board. Final standings are re-measured after the contest on the golden
        box, on a sealed stream, from your last submitted code.
      </p>
    </div>
  );
}
