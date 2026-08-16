// The phase-by-phase benchmark analytics for one submission, factored out so
// the participant's own detail page and the operator's student-monitoring
// drill-down render the exact same instrument. One scoring chain everywhere:
// p95, then p50, then p99. Deadlines are gates, never scores.

import type { LadderLevel, Submission } from '@/lib/api';

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
    <div style={{ display: 'flex', gap: 28, flexWrap: 'wrap' }}>
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

/// Renders the phase analytics for a submission. Pass `source` to append the
/// code the worker only ever saw as a hash (the operator view does; a
/// participant looking at their own submission does not need it).
export default function SubmissionAnalytics({
  sub,
  source,
}: {
  sub: Submission;
  source?: string | null;
}) {
  const tests = sub.phase0?.tests;
  const verify = sub.phase0?.verify;
  const failing = tests?.tests?.filter((t) => !t.passed) ?? [];

  return (
    <>
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
                      `#${e.seq} ${e.type} ${e.side ?? ''} ${e.tif ?? ''} price=${e.price} qty=${e.qty} session=${e.session} coid=${e.coid}`,
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

      {source != null && (
        <Section title={`Source · ${sub.source_hash.slice(0, 12)}`}>
          {source.trim().length === 0 ? (
            <div style={{ padding: '10px 14px', fontSize: 12, color: 'var(--app-ink-3)' }}>
              Source blob not found in storage.
            </div>
          ) : (
            <pre className="mono" style={{ ...pre, whiteSpace: 'pre', maxHeight: 460 }}>
              {source}
            </pre>
          )}
        </Section>
      )}
    </>
  );
}
