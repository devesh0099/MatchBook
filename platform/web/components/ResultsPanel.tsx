'use client';

// The feedback loop. Compiler errors verbatim, the FIRST divergence only, and a
// shrunk reproducer — showing all 400 failures encourages fixing by
// pattern-matching against output instead of thinking (plan section 6).

import {
  isTerminal,
  stateLabel,
  stateTone,
  type OutEventView,
  type RunResult,
  type Submission,
} from '@/lib/api';
import { DistributionChart, PerRunChart } from './Charts';

export function StatePill({ state }: { state: Submission['state'] }) {
  const tone = stateTone(state);
  return (
    <span className={`pill tone-${tone}`}>
      <i className="dot" />
      {stateLabel(state)}
    </span>
  );
}

function describeOut(e: OutEventView): string {
  const ref = (s: number, c: number, f: number) =>
    s === 0 && c === 0 && f === 0 ? '-' : `(s${s},c${c},f${f})`;
  let head = e.type;
  if (e.type === 'TRADE') head += ` ${e.qty} @ ${e.px}`;
  else if (e.type === 'CANCEL_ACK' || e.type === 'EXPIRED') head += ` ${e.qty}`;
  const tail =
    `  maker=${ref(e.maker_session, e.maker_coid, e.maker_firm)}` +
    `  taker=${ref(e.taker_session, e.taker_coid, e.taker_firm)}`;
  return head + tail + (e.reason && e.reason !== 'none' ? `  reason=${e.reason}` : '');
}

export function RunPanel({ result, running }: { result: RunResult | null; running: boolean }) {
  if (running) return <p className="mono tone-busy">running the visible tests…</p>;
  if (!result) {
    return (
      <p className="mono" style={{ color: 'var(--faint)' }}>
        Run executes the ~30 visible tests. They teach the rules; they do not grade. The gate is
        Submit.
      </p>
    );
  }
  if (result.error) return <pre className="out tone-bad">{result.error}</pre>;

  if (!result.compiled) {
    return (
      <>
        <p className="tone-bad mono">compile failed</p>
        <pre className="out">{result.stderr}</pre>
      </>
    );
  }

  const t = result.tests;
  if (!t) return <p className="mono tone-warn">compiled, but no test output came back</p>;

  const failed = t.tests.filter((x) => !x.passed);
  return (
    <>
      <p className="mono">
        <span className={t.passed === t.total ? 'tone-good' : 'tone-bad'}>
          {t.passed}/{t.total} passed
        </span>
      </p>
      {failed.length > 0 && (
        <pre className="out">
          {failed
            .slice(0, 6)
            .map((f) => `FAIL  ${f.name}  [${f.rule}]\n      ${f.detail}`)
            .join('\n\n')}
          {failed.length > 6 ? `\n\n… and ${failed.length - 6} more` : ''}
        </pre>
      )}
      {failed.length === 0 && (
        <p className="mono" style={{ color: 'var(--faint)' }}>
          All visible tests pass. This does not mean you pass the gate — that runs a hidden seed.
        </p>
      )}
    </>
  );
}

export function SubmissionPanel({ sub }: { sub: Submission }) {
  const d = sub.verify_detail;

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
        <StatePill state={sub.state} />
        <span className="mono" style={{ color: 'var(--faint)' }}>
          #{sub.id} · {sub.source_hash.slice(0, 12)}
        </span>
        {!isTerminal(sub.state) && (
          <span className="mono tone-busy" style={{ marginLeft: 'auto' }}>
            polling…
          </span>
        )}
      </div>

      {sub.state === 'compile_failed' && d?.stderr && <pre className="out">{d.stderr}</pre>}

      {sub.state === 'verify_passed' && d?.bench_held && (
        <pre className="out tone-warn">
          {`Correct — but not queued for the benchmark.

${d.bench_held}

Your code passed. This is a queue rule, not a result: the benchmark node runs
one job at a time and one per participant per 15 minutes.`}
        </pre>
      )}

      {sub.state === 'verify_timeout' && (
        <pre className="out tone-warn">
          {`The engine stopped making progress and was stopped at the wall-clock limit.

This is a liveness failure, not a wrong answer. Look for an unbounded loop —
a walk that never advances, a level that is never removed — not a matching bug.`}
        </pre>
      )}

      {d?.outcome === 'crashed' && (
        <pre className="out tone-bad">
          {`The engine died before finishing (${d.status ?? 'signal'}).

${d.hint ?? ''}
${d.stderr ?? ''}`}
        </pre>
      )}

      {d?.expected && d?.actual && (
        <div>
          <p className="mono" style={{ margin: '0 0 6px' }}>
            first divergence at output #{d.output_index} (input seq {d.in_seq})
          </p>
          <pre className="out">
            {`  expected: ${describeOut(d.expected)}\n  actual:   ${describeOut(d.actual)}`}
          </pre>
        </div>
      )}

      {d?.message && !d.expected && <pre className="out">{d.message}</pre>}

      {d?.reproducer && d.reproducer.length > 0 && (
        <div>
          <p className="mono" style={{ margin: '0 0 6px', color: 'var(--muted)' }}>
            reproducer — {d.reproducer.length} event{d.reproducer.length === 1 ? '' : 's'}, replay
            these from a fresh book
          </p>
          <pre className="out">
            {d.reproducer
              .map((e) =>
                e.type === 'CANCEL'
                  ? `seq=${e.seq}  CANCEL session=${e.session} coid=${e.coid}`
                  : `seq=${e.seq}  NEW ${e.side} ${e.qty} @ ${
                      e.tif === 'MARKET' ? 'market' : e.px
                    }  session=${e.session} coid=${e.coid} firm=${e.firm} tif=${e.tif}`,
              )
              .join('\n')}
          </pre>
        </div>
      )}

      {d?.progress && d.progress.length > 0 && (
        <div>
          <p className="mono" style={{ margin: '0 0 6px', color: 'var(--muted)' }}>
            progress
          </p>
          <pre className="out">
            {d.progress
              .map((p) => {
                const mark = p.failed ? '✗' : p.exercised > 0 ? '✓' : '·';
                const tone = p.failed ? '' : '';
                return `  ${mark}  ${p.area.padEnd(24)} ${p.exercised} events${tone}`;
              })
              .join('\n')}
          </pre>
        </div>
      )}

      {sub.state === 'done' && sub.p50_ns != null && (
        <div className="card">
          <div style={{ display: 'flex', gap: 28, flexWrap: 'wrap' }}>
            <Metric label="p50 (ranked)" value={`${sub.p50_ns.toFixed(1)} ns`} strong />
            <Metric label="p99" value={sub.p99_ns != null ? `${sub.p99_ns.toFixed(1)} ns` : '—'} />
            <Metric
              label="probe cost"
              value={sub.probe_cost_ns != null ? `${sub.probe_cost_ns.toFixed(1)} ns` : '—'}
            />
            <Metric label="runs" value={String(sub.run_p50s_ns?.length ?? 0)} />
            {(sub.discard_count ?? 0) > 0 && (
              <Metric label="discarded" value={String(sub.discard_count)} />
            )}
          </div>
          {(sub.run_p50s_ns?.length ?? 0) > 1 && (
            <div style={{ marginTop: 18 }}>
              <PerRunChart
                runs={sub.run_p50s_ns as number[]}
                medianNs={sub.p50_ns}
                ciLowNs={Math.min(...(sub.run_p50s_ns as number[]))}
                ciHighNs={Math.max(...(sub.run_p50s_ns as number[]))}
              />
            </div>
          )}

          {sub.percentiles && sub.percentiles.length > 1 && (
            <div style={{ marginTop: 22 }}>
              <DistributionChart percentiles={sub.percentiles} probeCostNs={sub.probe_cost_ns} />
            </div>
          )}

          {sub.probe_cost_ns != null && (
            <p style={{ color: 'var(--faint)', margin: '12px 0 0', fontSize: 12.5 }}>
              The probe cost is the fixed overhead of the two <span className="mono">rdtscp</span>{' '}
              reads around every event. It is added to every sample, so ordering is preserved and
              gaps are compressed. Engine work net of it is about{' '}
              <span className="mono">{Math.max(0, sub.p50_ns - sub.probe_cost_ns).toFixed(1)} ns</span>.
            </p>
          )}
        </div>
      )}

      {sub.state === 'bench_verify_failed' && (
        <pre className="out tone-bad">
          {`Passed the correctness lane, then diverged on the benchmark stream.

The ranked run re-checks your output with the same field-wise digest, inside
the timed loop. Same engine, longer and different stream — look for something
that only shows up at depth: a level that is not cleaned up, an index that
degrades, an assumption about book size.`}
        </pre>
      )}
    </div>
  );
}

function Metric({ label, value, strong }: { label: string; value: string; strong?: boolean }) {
  return (
    <div>
      <div style={{ fontSize: 11, textTransform: 'uppercase', letterSpacing: '0.06em', color: 'var(--faint)' }}>
        {label}
      </div>
      <div
        className="mono"
        style={{ fontSize: strong ? 22 : 16, marginTop: 2, color: strong ? 'var(--text)' : 'var(--muted)' }}
      >
        {value}
      </div>
    </div>
  );
}
