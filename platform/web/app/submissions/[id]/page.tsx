'use client';

// One submission, in detail. Built around failure, because that is where people
// live: five of the seven states a submission can reach are bad news, and each
// one needs a different next action.
//
// The compiler's own errors, the FIRST divergence only, and a shrunk
// reproducer. Never the full list of failures — that encourages fixing by
// pattern-matching against output instead of thinking (plan section 6).

import { useParams, useRouter } from 'next/navigation';
import { useEffect, useState } from 'react';
import {
  api,
  isTerminal,
  stateLabel,
  type OutEventView,
  type Submission,
  type SubState,
} from '@/lib/api';
import { inkOf, stateBlurb, TONE_BG, TONE_INK } from '@/components/StateTokens';
import { HdrHistogram, HistorySeries, RunTimeline } from '@/components/Charts';

const POLL_MS = 2000;

export default function SubmissionPage() {
  const params = useParams<{ id: string }>();
  const id = Number(params.id);
  const router = useRouter();

  const [sub, setSub] = useState<Submission | null>(null);
  const [history, setHistory] = useState<Submission[]>([]);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!Number.isFinite(id)) return;
    let alive = true;
    const load = () =>
      api
        .submission(id)
        .then((r) => {
          if (!alive) return;
          setSub(r.submission);
        })
        .catch((e) => alive && setError(String((e as Error).message ?? e)));
    load();
    const t = setInterval(load, POLL_MS);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, [id]);

  // Stop polling once it stops moving.
  useEffect(() => {
    if (!sub || !isTerminal(sub.state)) return;
    // The interval above is cheap and self-limiting via `terminal`, but the
    // history only needs fetching once the result is final.
    api
      .mine(sub.participant_id)
      .then(setHistory)
      .catch(() => {});
  }, [sub?.state, sub?.participant_id]); // eslint-disable-line react-hooks/exhaustive-deps

  if (error) {
    return (
      <div className="page" style={{ paddingTop: 40 }}>
        <div style={{ fontWeight: 800, fontSize: 22, marginBottom: 8 }}>Could not load #{id}</div>
        <div className="mono" style={{ color: 'var(--s-fail)' }}>
          {error}
        </div>
      </div>
    );
  }
  if (!sub) {
    return (
      <div className="page" style={{ paddingTop: 40, color: 'var(--app-ink-3)' }}>
        Loading #{id}…
      </div>
    );
  }

  const d = sub.verify_detail;
  const ink = inkOf(sub.state);
  const done = sub.state === 'done' && sub.p50_ns != null;

  return (
    <div className="page scroll-y">
      {/* ── header ────────────────────────────────────────────── */}
      <div
        style={{
          display: 'flex',
          alignItems: 'flex-start',
          justifyContent: 'space-between',
          gap: 24,
          padding: '26px 0 20px',
          borderBottom: '2px solid var(--app-rule)',
          flexWrap: 'wrap',
        }}
      >
        <div style={{ minWidth: 0 }}>
          <div
            className="mono"
            style={{ fontSize: 11, color: 'var(--app-ink-3)', letterSpacing: '0.04em' }}
          >
            #{sub.id} · {sub.source_hash.slice(0, 12)} · submitted {hhmmss(sub.created_at)}
            {!isTerminal(sub.state) && ' · polling every 2s'}
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 14, marginTop: 10 }}>
            <span style={{ width: 16, height: 16, background: ink, flex: 'none' }} />
            <span
              style={{
                fontWeight: 800,
                fontSize: 34,
                lineHeight: 1,
                letterSpacing: '-0.02em',
              }}
            >
              {stateLabel(sub.state)}
            </span>
          </div>
          <div
            style={{
              fontSize: 14,
              color: 'var(--app-ink-2)',
              marginTop: 10,
              maxWidth: '62ch',
              lineHeight: 1.5,
            }}
          >
            {stateBlurb(sub.state)}
          </div>
        </div>

        <div style={{ flex: 'none', textAlign: 'right' }}>
          <div className="kicker">{done ? 'p50 / event' : 'Rank impact'}</div>
          <div
            className="mono"
            style={{ fontSize: 44, fontWeight: 700, lineHeight: 1.05, color: ink }}
          >
            {done ? Math.round(sub.p50_ns!) : '—'}
          </div>
          <div className="mono" style={{ fontSize: 11, color: 'var(--app-ink-3)' }}>
            {done ? intervalLabel(sub) : 'not ranked'}
          </div>
        </div>
      </div>

      <StageRail state={sub.state} />

      {/* ── the panel for the state it is actually in ──────────── */}
      {sub.state === 'compile_failed' && <CompileFailed stderr={d?.stderr} />}

      {d?.outcome === 'crashed' && (
        <Callout tone="fail" title="The engine died before finishing">
          <p style={{ margin: 0 }}>
            {d.hint ?? 'A signal, not a diff — so there is no expected-versus-actual to show.'}
          </p>
          {d.status && (
            <div className="mono" style={{ marginTop: 10, fontSize: 12 }}>
              isolate status: {d.status}
            </div>
          )}
          {d.stderr && <pre className="code" style={{ marginTop: 12 }}>{d.stderr}</pre>}
        </Callout>
      )}

      {d?.expected && d?.actual && (
        <Divergence
          inSeq={d.in_seq}
          outputIndex={d.output_index}
          expected={d.expected}
          actual={d.actual}
          message={d.message}
          reproducer={d.reproducer}
        />
      )}

      {d?.message && !d.expected && sub.state !== 'compile_failed' && (
        <Callout tone={sub.state === 'verify_timeout' ? 'slow' : 'fail'} title="What the harness saw">
          <p style={{ margin: 0 }}>{d.message}</p>
        </Callout>
      )}

      {sub.state === 'verify_timeout' && (
        <Callout tone="slow" title="Liveness, not correctness">
          <p style={{ margin: 0, maxWidth: '70ch' }}>
            Your engine stopped making progress and was stopped at the wall-clock limit. Output
            matched the reference up to that point, so this is not a wrong answer — it is a stall or
            an unbounded loop. Look for a walk that never advances or a level that is never removed,
            not a matching bug.
          </p>
          {d?.events_processed != null && (
            <Figures
              items={[
                ['Consumed', d.events_processed.toLocaleString()],
                ['Of', (d.total_events ?? 0).toLocaleString()],
              ]}
            />
          )}
        </Callout>
      )}

      {sub.state === 'pending_benchmark' && <Held sub={sub} />}

      {sub.state === 'verify_passed' && d?.bench_held && (
        <Callout tone="idle" title="Correct — but not queued for the benchmark">
          <p style={{ margin: 0, maxWidth: '66ch' }}>{d.bench_held}</p>
          <p style={{ margin: '10px 0 0', maxWidth: '66ch', color: 'var(--app-ink-2)' }}>
            Your code passed. This is a queue rule, not a result: the benchmark node runs one job
            at a time, and you can only have one of yours waiting for it.
          </p>
        </Callout>
      )}

      {sub.state === 'benchmarking' && <Benchmarking sub={sub} />}

      {sub.state === 'bench_verify_failed' && (
        <Callout tone="fail" title="Diverged while being timed">
          <p style={{ margin: 0, maxWidth: '70ch' }}>
            You passed the correctness lane and then produced different output on the benchmark
            stream. A ranked run re-checks your output with the same field-wise digest{' '}
            <em>inside</em> the timed loop, so this cannot be scored.
          </p>
          <p style={{ margin: '10px 0 0', maxWidth: '70ch', color: 'var(--app-ink-2)' }}>
            Same engine, a longer and different stream — look for something that only appears at
            depth: a level that is not cleaned up, an index that degrades, an assumption about book
            size.
          </p>
        </Callout>
      )}

      {done && <DoneResult sub={sub} history={history} />}

      {d?.progress && d.progress.length > 0 && <CategoryGrid progress={d.progress} />}

      <div style={{ marginTop: 28, display: 'flex', gap: 10 }}>
        <button className="btn btn-secondary" onClick={() => router.push('/editor')}>
          Back to the editor
        </button>
        <button className="btn btn-secondary" onClick={() => router.push('/submissions')}>
          All my submissions
        </button>
      </div>
    </div>
  );
}

/// The four stages, always all four, so the one that has not run yet is
/// visible as *not run* rather than absent.
function StageRail({ state }: { state: SubState }) {
  const before = (s: SubState[]) => s.includes(state);

  const compile: Stage = before(['compile_failed'])
    ? { bar: TONE_INK.fail, note: 'failed' }
    : before(['received', 'compiling'])
      ? { bar: 'var(--app-line)', note: 'running' }
      : { bar: TONE_INK.pass, note: 'built' };

  const verify: Stage = before(['compile_failed'])
    ? { bar: 'var(--app-line)', note: 'not run' }
    : before(['verify_failed'])
      ? { bar: TONE_INK.fail, note: 'diverged' }
      : before(['verify_timeout'])
        ? { bar: TONE_INK.slow, note: 'timed out' }
        : before(['received', 'compiling', 'verifying'])
          ? { bar: 'var(--app-line)', note: 'running' }
          : { bar: TONE_INK.pass, note: 'passed' };

  const queue: Stage = before(['done', 'benchmarking', 'bench_verify_failed'])
    ? { bar: TONE_INK.pass, note: 'admitted' }
    : before(['pending_benchmark'])
      ? { bar: TONE_INK.idle, note: 'held' }
      : before(['bench_queued'])
        ? { bar: TONE_INK.bench, note: 'queued' }
        : before(['verify_passed'])
          ? { bar: TONE_INK.pass, note: 'eligible' }
          : { bar: 'var(--app-line)', note: '—' };

  const bench: Stage = before(['done'])
    ? { bar: TONE_INK.pass, note: '9 runs' }
    : before(['benchmarking'])
      ? { bar: TONE_INK.bench, note: 'running' }
      : before(['bench_verify_failed'])
        ? { bar: TONE_INK.fail, note: 'diverged' }
        : { bar: 'var(--app-line)', note: '—' };

  const stages: [string, Stage][] = [
    ['Compile', compile],
    ['Verify', verify],
    ['Queue', queue],
    ['Benchmark', bench],
  ];

  return (
    <div style={{ display: 'flex', gap: 2, marginTop: 16 }}>
      {stages.map(([name, s]) => (
        <div key={name} style={{ flex: 1, borderTop: `4px solid ${s.bar}`, paddingTop: 8 }}>
          <div
            style={{
              fontSize: 10,
              fontWeight: 800,
              letterSpacing: '0.12em',
              textTransform: 'uppercase',
              color: s.bar === 'var(--app-line)' ? 'var(--app-ink-3)' : s.bar,
            }}
          >
            {name}
          </div>
          <div
            className="mono"
            style={{ fontSize: 11, color: 'var(--app-ink-3)', marginTop: 3 }}
          >
            {s.note}
          </div>
        </div>
      ))}
    </div>
  );
}

interface Stage {
  bar: string;
  note: string;
}

function CompileFailed({ stderr }: { stderr?: string }) {
  return (
    <div style={{ marginTop: 28, border: '2px solid var(--s-fail)' }}>
      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          padding: '9px 14px',
          background: 'var(--s-fail-bg)',
          borderBottom: '1px solid var(--s-fail)',
        }}
      >
        <span
          style={{
            fontWeight: 800,
            fontSize: 11,
            letterSpacing: '0.12em',
            textTransform: 'uppercase',
            color: 'var(--s-fail)',
          }}
        >
          stderr · first 100 lines
        </span>
        <span className="mono" style={{ fontSize: 11, color: 'var(--app-ink-2)' }}>
          g++ · exit 1
        </span>
      </div>
      <pre className="code">{stderr ?? 'no compiler output was recorded'}</pre>
    </div>
  );
}

/// The most important object in the app: what went in, what should have come
/// out, what did, and the smallest input that still reproduces it.
function Divergence({
  inSeq,
  outputIndex,
  expected,
  actual,
  message,
  reproducer,
}: {
  inSeq?: number;
  outputIndex?: number;
  expected: OutEventView;
  actual: OutEventView;
  message?: string;
  reproducer?: Submission['verify_detail'] extends null ? never : NonNullable<Submission['verify_detail']>['reproducer'];
}) {
  return (
    <div style={{ marginTop: 28 }}>
      <div
        style={{
          display: 'flex',
          alignItems: 'baseline',
          justifyContent: 'space-between',
          marginBottom: 12,
          gap: 12,
          flexWrap: 'wrap',
        }}
      >
        <span style={{ fontWeight: 800, fontSize: 13, letterSpacing: '0.1em', textTransform: 'uppercase' }}>
          First divergence
        </span>
        {reproducer && reproducer.length > 0 && (
          <span className="mono" style={{ fontSize: 11, color: 'var(--app-ink-3)' }}>
            shrunk from 300,000 events → {reproducer.length} event
            {reproducer.length === 1 ? '' : 's'}
          </span>
        )}
      </div>

      <div
        style={{
          display: 'grid',
          gridTemplateColumns: '1fr 1fr',
          border: '2px solid var(--app-rule)',
        }}
      >
        <div
          style={{
            gridColumn: '1 / -1',
            padding: '12px 14px',
            borderBottom: '2px solid var(--app-rule)',
            background: 'var(--app-panel)',
          }}
        >
          <div className="kicker" style={{ marginBottom: 6 }}>
            Input event #{inSeq ?? '?'}
            {outputIndex != null && ` · output ${outputIndex}`}
          </div>
          <div className="mono" style={{ fontSize: 14, fontWeight: 600 }}>
            {describeOut(expected).split('  ')[0]} was expected here
          </div>
        </div>

        <OutColumn title="Reference emitted" event={expected} tone="pass" borderRight />
        <OutColumn title="Yours emitted" event={actual} tone="fail" />

        {message && (
          <div
            style={{
              gridColumn: '1 / -1',
              padding: '12px 14px',
              borderTop: '1px solid var(--app-line)',
              background: 'var(--s-fail-bg)',
              color: 'var(--app-ink)',
              fontSize: 13,
              lineHeight: 1.5,
            }}
          >
            {message}
          </div>
        )}
      </div>

      {reproducer && reproducer.length > 0 && (
        <div style={{ marginTop: 20, border: '1px solid var(--app-line)' }}>
          <div
            style={{
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'space-between',
              padding: '9px 14px',
              borderBottom: '1px solid var(--app-line)',
              background: 'var(--app-panel)',
            }}
          >
            <span
              style={{
                fontWeight: 800,
                fontSize: 11,
                letterSpacing: '0.12em',
                textTransform: 'uppercase',
              }}
            >
              Minimised reproducer
            </span>
            <button
              className="btn btn-secondary"
              style={{ fontSize: 11, padding: '4px 9px' }}
              onClick={() => {
                void navigator.clipboard?.writeText(reproducer.map(reproLine).join('\n'));
              }}
            >
              Copy
            </button>
          </div>
          <div style={{ display: 'flex', flexDirection: 'column' }}>
            {reproducer.map((e, i) => {
              const last = i === reproducer.length - 1;
              return (
                <div
                  key={e.seq}
                  className="mono"
                  style={{
                    display: 'grid',
                    gridTemplateColumns: '44px 1fr auto',
                    gap: 12,
                    alignItems: 'center',
                    padding: '6px 14px',
                    background: last ? 'var(--s-fail-bg)' : 'transparent',
                    borderBottom: '1px solid var(--app-line)',
                    fontSize: 12.5,
                  }}
                >
                  <span style={{ color: 'var(--app-ink-3)' }}>{i + 1}</span>
                  <span style={{ fontWeight: last ? 700 : 400 }}>{reproLine(e)}</span>
                  <span style={{ color: 'var(--s-fail)', fontWeight: 700, fontSize: 11 }}>
                    {last ? 'diverges' : ''}
                  </span>
                </div>
              );
            })}
          </div>
          <div style={{ padding: '9px 14px', fontSize: 11.5, color: 'var(--app-ink-3)' }}>
            Replay these from a fresh book. Sequence numbers are the record index, so they renumber
            when you drop leading events — which is why the shrink is a search and not an
            assumption.
          </div>
        </div>
      )}
    </div>
  );
}

function OutColumn({
  title,
  event,
  tone,
  borderRight,
}: {
  title: string;
  event: OutEventView;
  tone: 'pass' | 'fail';
  borderRight?: boolean;
}) {
  return (
    <div
      style={{
        padding: '12px 14px',
        borderRight: borderRight ? '1px solid var(--app-line)' : undefined,
        minWidth: 0,
      }}
    >
      <div className="kicker" style={{ marginBottom: 8, color: tone === 'fail' ? TONE_INK.fail : undefined }}>
        {title}
      </div>
      <div
        className="mono"
        style={{
          fontSize: 13,
          padding: '5px 8px',
          background: TONE_BG[tone],
          borderLeft: `3px solid ${TONE_INK[tone]}`,
          overflowWrap: 'anywhere',
        }}
      >
        {describeOut(event)}
      </div>
    </div>
  );
}

function Held({ sub }: { sub: Submission }) {
  return (
    <div
      style={{
        marginTop: 28,
        border: '2px dashed var(--app-rule)',
        padding: 22,
        display: 'flex',
        gap: 22,
        alignItems: 'flex-start',
      }}
    >
      <div style={{ width: 10, height: 10, background: 'var(--app-ink-3)', marginTop: 7, flex: 'none' }} />
      <div>
        <div style={{ fontWeight: 800, fontSize: 18, marginBottom: 8 }}>
          Your submission is held, not lost.
        </div>
        <div
          style={{
            fontSize: 14,
            lineHeight: 1.6,
            maxWidth: '66ch',
            color: 'var(--app-ink-2)',
          }}
        >
          Correctness passed at {hhmmss(sub.updated_at)} and is recorded. The benchmark node is
          unavailable, so the timing run is queued and will execute automatically when it returns —
          you do not need to resubmit, and resubmitting will not move you up the queue. Keep
          iterating; correctness still runs on anything you submit meanwhile.
        </div>
      </div>
    </div>
  );
}

function Benchmarking({ sub }: { sub: Submission }) {
  const done = sub.run_p50s_ns?.length ?? 0;
  const total = 9;
  return (
    <div style={{ marginTop: 28, border: '2px solid var(--s-bench)', padding: 22 }}>
      <div
        style={{
          display: 'flex',
          alignItems: 'baseline',
          justifyContent: 'space-between',
          gap: 12,
          flexWrap: 'wrap',
        }}
      >
        <div style={{ fontWeight: 800, fontSize: 18, color: 'var(--s-bench)' }}>
          Benchmarking{done ? ` — run ${Math.min(done + 1, total)} of ${total}` : ''}
        </div>
        <div className="mono" style={{ fontSize: 12, color: 'var(--app-ink-2)' }}>
          polling /submissions/{sub.id} every 2s
        </div>
      </div>
      <div style={{ display: 'flex', gap: 3, marginTop: 16, height: 26 }}>
        {Array.from({ length: total }, (_, i) => {
          const complete = i < done;
          return (
            <div
              key={i}
              className="mono"
              style={{
                flex: 1,
                background: complete ? 'var(--s-bench)' : 'var(--s-bench-bg)',
                color: complete ? '#fff' : 'var(--app-ink-3)',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
                fontSize: 11,
                fontWeight: 700,
              }}
            >
              {i + 1}
            </div>
          );
        })}
      </div>
      <div style={{ fontSize: 13, color: 'var(--app-ink-2)', marginTop: 14, maxWidth: '72ch' }}>
        Each run replays 10 million events: about 3.9 million untimed while the book fills to its
        300,000 resting orders, then 6.1 million measured. The score is the median of the nine
        per-run p50s, so nothing is shown until all nine finish — a partial median would be wrong.
      </div>
    </div>
  );
}

function DoneResult({ sub, history }: { sub: Submission; history: Submission[] }) {
  const kept = (sub.run_p50s_ns?.length ?? 0) - (sub.discard_count ?? 0);
  const metrics: [string, string, string][] = [
    ['Runs', String(sub.run_p50s_ns?.length ?? 0), `${sub.discard_count ?? 0} discarded on steal`],
    ['Kept', String(Math.max(0, kept)), 'median of these'],
    ['Interval', intervalLabel(sub), 'min/max of kept, ns'],
    ['p99', sub.p99_ns != null ? Math.round(sub.p99_ns).toLocaleString() : '—', 'ns · tail'],
    [
      'Probe cost',
      sub.probe_cost_ns != null ? `${sub.probe_cost_ns.toFixed(1)} ns` : '—',
      sub.probe_cost_ns != null && sub.p50_ns
        ? `${((100 * sub.probe_cost_ns) / sub.p50_ns).toFixed(1)}% of p50`
        : 'rdtscp overhead',
    ],
    ['Events', '6,108,800', 'timed, per run'],
  ];

  return (
    <>
      <div
        style={{
          marginTop: 26,
          display: 'grid',
          gridTemplateColumns: 'repeat(auto-fit,minmax(150px,1fr))',
          borderTop: '1px solid var(--app-line)',
          borderBottom: '1px solid var(--app-line)',
        }}
      >
        {metrics.map(([k, v, s]) => (
          <div key={k} style={{ padding: '14px 14px 14px 0' }}>
            <div className="kicker" style={{ letterSpacing: '0.12em' }}>
              {k}
            </div>
            <div className="mono" style={{ fontSize: 19, fontWeight: 700, marginTop: 5 }}>
              {v}
            </div>
            <div style={{ fontSize: 11, color: 'var(--app-ink-3)', marginTop: 2 }}>{s}</div>
          </div>
        ))}
      </div>

      {sub.percentiles && sub.percentiles.length > 3 && (
        <section style={{ marginTop: 32 }}>
          <ChartHead
            title="Latency distribution"
            note="HdrHistogram · x = 1/(1−p), log scale"
            blurb="Every point is a percentile. The right-hand half of the plot is the tail — one event in a hundred thousand."
          />
          <div style={{ border: '1px solid var(--app-line)', background: 'var(--app-panel)', padding: 10 }}>
            <HdrHistogram percentiles={sub.percentiles} probeCostNs={sub.probe_cost_ns} />
          </div>
        </section>
      )}

      <div
        style={{
          marginTop: 32,
          display: 'grid',
          gridTemplateColumns: 'repeat(auto-fit,minmax(340px,1fr))',
          gap: 24,
        }}
      >
        {sub.timeline && sub.timeline.length > 2 && (
          <section>
            <ChartHead
              title="Through the median run"
              blurb="60 windows. Flat is what you want; a rising p99 is a cache or allocation problem."
            />
            <div style={{ border: '1px solid var(--app-line)', background: 'var(--app-panel)', padding: 10 }}>
              <RunTimeline points={sub.timeline} />
            </div>
          </section>
        )}

        {history.filter((h) => h.p50_ns != null).length > 1 && (
          <section>
            <ChartHead
              title="Across your submissions"
              blurb="Am I getting better. Each point is a completed benchmark today."
            />
            <div style={{ border: '1px solid var(--app-line)', background: 'var(--app-panel)', padding: 10 }}>
              <HistorySeries submissions={history} highlightId={sub.id} />
            </div>
          </section>
        )}
      </div>

      {sub.probe_cost_ns != null && sub.p50_ns != null && (
        <p style={{ color: 'var(--app-ink-3)', margin: '16px 0 0', fontSize: 12.5, maxWidth: '72ch' }}>
          The probe cost is the fixed overhead of the two <span className="mono">rdtscp</span> reads
          around every event. It is added to every sample, so ordering is preserved and gaps are
          compressed. Engine work net of it is about{' '}
          <span className="mono">{Math.max(0, sub.p50_ns - sub.probe_cost_ns).toFixed(1)} ns</span>.
        </p>
      )}
    </>
  );
}

function CategoryGrid({ progress }: { progress: { area: string; exercised: number; failed: number }[] }) {
  return (
    <div style={{ marginTop: 28 }}>
      <div
        style={{
          fontWeight: 800,
          fontSize: 11,
          letterSpacing: '0.12em',
          textTransform: 'uppercase',
          marginBottom: 10,
        }}
      >
        Category progress · hidden stream
      </div>
      <div
        style={{
          display: 'grid',
          // Exactly three, so six categories fill two full rows. auto-fit
          // left empty cells showing as grey tiles.
          gridTemplateColumns: 'repeat(3,minmax(0,1fr))',
          gap: 1,
          background: 'var(--app-line)',
          border: '1px solid var(--app-line)',
        }}
      >
        {progress.map((p, i) => {
          const tone = p.failed ? 'fail' : p.exercised > 0 ? 'pass' : 'idle';
          const glyph = p.failed ? '✕' : p.exercised > 0 ? '✓' : '—';
          // The harness reports however many areas it reports — seven today. A
          // fixed three-column grid leaves the last row short, and the gap
          // colour then shows through as an empty grey tile. Stretch the final
          // item across whatever is left instead.
          const remainder = progress.length % 3;
          const spanLast = i === progress.length - 1 && remainder !== 0 ? 4 - remainder : 1;
          return (
            <div
              key={p.area}
              style={{
                background: TONE_BG[tone],
                padding: '12px 14px',
                gridColumn: spanLast > 1 ? `span ${spanLast}` : undefined,
              }}
            >
              <div style={{ display: 'flex', alignItems: 'center', gap: 9 }}>
                <span className="mono" style={{ fontWeight: 700, fontSize: 15, color: TONE_INK[tone] }}>
                  {glyph}
                </span>
                <span style={{ fontWeight: 700, fontSize: 13 }}>{p.area}</span>
              </div>
              <div className="mono" style={{ fontSize: 11, color: 'var(--app-ink-2)', marginTop: 5 }}>
                {p.exercised.toLocaleString()} events
                {p.failed ? ` · ${p.failed} failed` : ''}
              </div>
            </div>
          );
        })}
      </div>
      <div style={{ fontSize: 11.5, color: 'var(--app-ink-3)', marginTop: 8, maxWidth: '72ch' }}>
        Judged from the <em>oracle&apos;s</em> output, never yours — otherwise an engine that emits
        nothing would be credited with exercising nothing, and the breakdown would flatter exactly
        the submissions that deserve it least.
      </div>
    </div>
  );
}

// ── small pieces ───────────────────────────────────────────────

function ChartHead({ title, note, blurb }: { title: string; note?: string; blurb: string }) {
  return (
    <>
      <div
        style={{
          display: 'flex',
          alignItems: 'baseline',
          justifyContent: 'space-between',
          marginBottom: 4,
          gap: 12,
          flexWrap: 'wrap',
        }}
      >
        <span style={{ fontWeight: 800, fontSize: 15 }}>{title}</span>
        {note && (
          <span className="mono" style={{ fontSize: 11, color: 'var(--app-ink-3)' }}>
            {note}
          </span>
        )}
      </div>
      <div style={{ fontSize: 12, color: 'var(--app-ink-2)', marginBottom: 12 }}>{blurb}</div>
    </>
  );
}

function Callout({
  tone,
  title,
  children,
}: {
  tone: 'fail' | 'slow' | 'idle' | 'bench';
  title: string;
  children: React.ReactNode;
}) {
  return (
    <div style={{ marginTop: 28, border: `2px solid ${TONE_INK[tone]}`, padding: 18 }}>
      <div
        style={{
          fontWeight: 800,
          fontSize: 12,
          letterSpacing: '0.12em',
          textTransform: 'uppercase',
          color: TONE_INK[tone],
          marginBottom: 10,
        }}
      >
        {title}
      </div>
      <div style={{ fontSize: 14, lineHeight: 1.6, color: 'var(--app-ink)' }}>{children}</div>
    </div>
  );
}

function Figures({ items }: { items: [string, string][] }) {
  return (
    <div className="mono" style={{ display: 'flex', gap: 32, marginTop: 16, flexWrap: 'wrap' }}>
      {items.map(([k, v]) => (
        <div key={k}>
          <div className="kicker" style={{ letterSpacing: '0.12em' }}>
            {k}
          </div>
          <div style={{ fontSize: 22, fontWeight: 700 }}>{v}</div>
        </div>
      ))}
    </div>
  );
}

function describeOut(e: OutEventView): string {
  const ref = (s: number, c: number, f: number) =>
    s === 0 && c === 0 && f === 0 ? '—' : `(s${s} c${c} f${f})`;
  let head = e.type;
  if (e.type === 'TRADE') head += ` ${e.qty} @ ${e.px}`;
  else if (e.type === 'CANCEL_ACK' || e.type === 'EXPIRED') head += ` ${e.qty}`;
  const tail = `  maker=${ref(e.maker_session, e.maker_coid, e.maker_firm)}  taker=${ref(
    e.taker_session,
    e.taker_coid,
    e.taker_firm,
  )}`;
  return head + tail + (e.reason && e.reason !== 'none' ? `  reason=${e.reason}` : '');
}

function reproLine(e: {
  seq: number;
  type: string;
  side: string;
  tif: string;
  px: number;
  qty: number;
  session: number;
  coid: number;
  firm: number;
}) {
  return e.type === 'CANCEL'
    ? `CANCEL  session=${e.session} coid=${e.coid}`
    : `NEW  ${e.side} ${e.qty} @ ${e.tif === 'MARKET' ? 'market' : e.px}  session=${e.session} coid=${e.coid} firm=${e.firm} tif=${e.tif}`;
}

function intervalLabel(sub: Submission) {
  const ps = sub.run_p50s_ns ?? [];
  if (ps.length === 0) return '—';
  const lo = Math.min(...ps);
  const hi = Math.max(...ps);
  return `[${Math.round(lo)}, ${Math.round(hi)}]`;
}

function hhmmss(iso: string | null) {
  if (!iso) return '—';
  return new Date(iso).toLocaleTimeString('en-GB', { hour12: false });
}
