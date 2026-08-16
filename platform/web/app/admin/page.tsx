'use client';

// The operator dashboard (M7's read surface, rendered). Sign in with the
// master credential (ADMIN_EMAIL / ADMIN_PASSWORD, deployment config); every
// panel below is one of the /op endpoints polling every 5s. The actions
// column is the whole event-day control surface: freeze, close, rejudge,
// publish. Students never see this page — the operator session is a separate
// lane from participant sessions by construction.

import { useCallback, useEffect, useState } from 'react';
import { op, stateLabel, type OpHealth, type OpParticipantRow, type RejudgeStatus, type Submission } from '@/lib/api';
import SubmissionAnalytics from '@/components/SubmissionAnalytics';

const field: React.CSSProperties = {
  width: '100%',
  border: '1px solid var(--app-rule)',
  background: 'var(--app-bg)',
  color: 'var(--app-ink)',
  font: 'inherit',
  fontSize: 14,
  padding: '9px 12px',
  borderRadius: 0,
};

function Panel({ title, right, children }: { title: string; right?: React.ReactNode; children: React.ReactNode }) {
  return (
    <div style={{ border: '1px solid var(--app-line)', marginTop: 18 }}>
      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          padding: '9px 14px',
          background: 'var(--app-panel)',
          borderBottom: '1px solid var(--app-line)',
        }}
      >
        <span className="kicker">{title}</span>
        {right}
      </div>
      {children}
    </div>
  );
}

// Small per-row box button (Deploy / Redeploy / Retry).
function boxBtn(confirming: boolean): React.CSSProperties {
  return {
    appearance: 'none',
    border: `1px solid ${confirming ? 'var(--s-slow)' : 'var(--app-rule)'}`,
    background: 'transparent',
    color: confirming ? 'var(--s-slow)' : 'var(--app-ink)',
    font: 'inherit',
    fontWeight: 700,
    fontSize: 10,
    letterSpacing: '0.08em',
    textTransform: 'uppercase',
    padding: '4px 10px',
    cursor: 'pointer',
  };
}

function ActionButton({
  label,
  danger,
  onClick,
  disabled,
  reason,
}: {
  label: string;
  danger?: boolean;
  onClick: () => void;
  /// A disabled control is the failover: the operator can SEE the action
  /// exists but cannot fire it until its precondition holds.
  disabled?: boolean;
  /// Why it's disabled — surfaced as the hover title so the block is never
  /// mysterious.
  reason?: string;
}) {
  return (
    <button
      onClick={disabled ? undefined : onClick}
      disabled={disabled}
      title={disabled ? reason : undefined}
      style={{
        appearance: 'none',
        border: `1px solid ${disabled ? 'var(--app-line)' : danger ? 'var(--s-fail)' : 'var(--app-rule)'}`,
        background: 'transparent',
        color: disabled ? 'var(--app-ink-3)' : danger ? 'var(--s-fail)' : 'var(--app-ink)',
        font: 'inherit',
        fontWeight: 700,
        fontSize: 11,
        letterSpacing: '0.1em',
        textTransform: 'uppercase',
        padding: '6px 12px',
        cursor: disabled ? 'not-allowed' : 'pointer',
        opacity: disabled ? 0.5 : 1,
      }}
    >
      {label}
    </button>
  );
}

// Student monitoring: a modal that drills from a student to all their
// submissions, and from one submission to its full benchmark analytics +
// source. Two views in one overlay — the list, then a chosen submission.
function StudentInspector({
  pid,
  handle,
  onClose,
}: {
  pid: number;
  handle: string;
  onClose: () => void;
}) {
  const [subs, setSubs] = useState<Submission[] | null>(null);
  const [sel, setSel] = useState<number | null>(null);
  const [detail, setDetail] = useState<{ submission: Submission; source: string } | null>(null);
  const [err, setErr] = useState<string | null>(null);

  useEffect(() => {
    let stop = false;
    op.studentSubmissions(pid)
      .then((r) => { if (!stop) setSubs(r.submissions); })
      .catch((e) => { if (!stop) setErr(String((e as Error).message ?? e)); });
    return () => { stop = true; };
  }, [pid]);

  useEffect(() => {
    if (sel == null) { setDetail(null); return; }
    let stop = false;
    setDetail(null);
    setErr(null);
    op.submissionDetail(sel)
      .then((r) => { if (!stop) setDetail(r); })
      .catch((e) => { if (!stop) setErr(String((e as Error).message ?? e)); });
    return () => { stop = true; };
  }, [sel]);

  const chip = (s: Submission) => {
    const good = s.state === 'done';
    const bad = s.state.endsWith('_failed') || s.state === 'error' || s.state === 'verify_timeout';
    return good ? 'var(--s-pass)' : bad ? 'var(--s-fail)' : 'var(--s-bench)';
  };

  return (
    <div
      onClick={onClose}
      style={{
        position: 'fixed', inset: 0, zIndex: 50,
        background: 'rgba(0,0,0,0.55)',
        display: 'flex', justifyContent: 'center', alignItems: 'flex-start',
        padding: '5vh 16px', overflow: 'auto',
      }}
    >
      <div
        onClick={(e) => e.stopPropagation()}
        style={{
          background: 'var(--app-bg)', border: '1px solid var(--app-rule)',
          width: 'min(940px, 100%)', maxHeight: '90vh', overflow: 'auto',
          boxShadow: '0 24px 70px rgba(0,0,0,0.45)',
        }}
      >
        <div
          style={{
            position: 'sticky', top: 0, zIndex: 1,
            display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 12,
            padding: '12px 16px', background: 'var(--app-panel)',
            borderBottom: '2px solid var(--app-rule)',
          }}
        >
          <div style={{ display: 'flex', alignItems: 'baseline', gap: 10, minWidth: 0 }}>
            {sel != null && (
              <button
                onClick={() => setSel(null)}
                className="mono"
                style={{ appearance: 'none', border: '1px solid var(--app-rule)', background: 'transparent', color: 'var(--app-ink)', font: 'inherit', fontSize: 11, fontWeight: 700, padding: '4px 9px', cursor: 'pointer' }}
              >
                ← submissions
              </button>
            )}
            <span style={{ fontWeight: 800, fontSize: 16 }}>{handle}</span>
            <span className="mono" style={{ fontSize: 12, color: 'var(--app-ink-3)' }}>
              {sel != null ? `submission #${sel}` : `participant ${pid}`}
            </span>
          </div>
          <button
            onClick={onClose}
            aria-label="Close"
            style={{ appearance: 'none', border: 0, background: 'transparent', color: 'var(--app-ink-3)', font: 'inherit', fontSize: 20, lineHeight: 1, cursor: 'pointer', padding: '0 4px' }}
          >
            ×
          </button>
        </div>

        <div style={{ padding: '4px 16px 20px' }}>
          {err && (
            <div className="mono" style={{ color: 'var(--s-fail)', fontSize: 12, padding: '12px 0' }}>{err}</div>
          )}

          {/* View 1 — the submission list */}
          {sel == null && (
            <>
              {subs == null && !err && (
                <div style={{ padding: '16px 0', color: 'var(--app-ink-3)' }}>Loading…</div>
              )}
              {subs != null && subs.length === 0 && (
                <div style={{ padding: '16px 0', color: 'var(--app-ink-3)', fontSize: 13 }}>
                  No submissions yet.
                </div>
              )}
              {subs != null && subs.length > 0 && (
                <div style={{ border: '1px solid var(--app-line)', marginTop: 14 }}>
                  <div
                    className="kicker"
                    style={{ display: 'grid', gridTemplateColumns: '58px 1fr 120px 52px 110px', padding: '8px 12px', borderBottom: '1px solid var(--app-line)' }}
                  >
                    <span>#</span><span>State</span><span>When</span>
                    <span style={{ textAlign: 'right' }}>Lvl</span>
                    <span style={{ textAlign: 'right' }}>p95 ns</span>
                  </div>
                  {subs.map((s) => (
                    <button
                      key={s.id}
                      onClick={() => setSel(s.id)}
                      className="mono"
                      style={{
                        display: 'grid', gridTemplateColumns: '58px 1fr 120px 52px 110px',
                        alignItems: 'center', width: '100%', textAlign: 'left',
                        appearance: 'none', border: 0, borderBottom: '1px solid var(--app-line)',
                        background: 'transparent', color: 'var(--app-ink)', font: 'inherit',
                        fontSize: 12, padding: '9px 12px', cursor: 'pointer',
                      }}
                    >
                      <span style={{ color: 'var(--app-ink-3)' }}>{s.id}</span>
                      <span style={{ color: chip(s), fontWeight: 700 }}>{stateLabel(s.state)}</span>
                      <span style={{ color: 'var(--app-ink-3)' }}>
                        {s.created_at ? new Date(s.created_at).toLocaleString() : '—'}
                      </span>
                      <span style={{ textAlign: 'right', fontWeight: 700 }}>
                        {s.max_level != null ? s.max_level : '—'}
                      </span>
                      <span style={{ textAlign: 'right' }}>
                        {s.top_p95_ns != null ? Math.round(s.top_p95_ns).toLocaleString()
                          : s.phase1?.p95_ns != null ? Math.round(s.phase1.p95_ns).toLocaleString() : '—'}
                      </span>
                    </button>
                  ))}
                </div>
              )}
            </>
          )}

          {/* View 2 — one submission's analytics + source */}
          {sel != null && (
            detail == null && !err ? (
              <div style={{ padding: '16px 0', color: 'var(--app-ink-3)' }}>Loading…</div>
            ) : detail != null ? (
              <SubmissionAnalytics sub={detail.submission} source={detail.source} />
            ) : null
          )}
        </div>
      </div>
    </div>
  );
}

export default function AdminPage() {
  const [operator, setOperator] = useState<boolean | null>(null);
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const [health, setHealth] = useState<OpHealth | null>(null);
  const [participants, setParticipants] = useState<OpParticipantRow[]>([]);
  const [events, setEvents] = useState<{ id: number; at: string; kind: string; submission_id: number | null }[]>([]);
  const [notice, setNotice] = useState<string | null>(null);
  const [newHandle, setNewHandle] = useState('');
  const [confirmAction, setConfirmAction] = useState<string | null>(null);

  const [rejudge, setRejudge] = useState<RejudgeStatus | null>(null);
  const [inspect, setInspect] = useState<{ pid: number; handle: string } | null>(null);
  const refresh = useCallback(() => {
    op.health().then(setHealth).catch(() => {});
    op.participants().then((r) => setParticipants(r.participants)).catch(() => {});
    op.events().then((r) => setEvents(r.events)).catch(() => {});
    op.rejudgeStatus().then(setRejudge).catch(() => {});
  }, []);

  useEffect(() => {
    op.session()
      .then((s) => setOperator(s.operator))
      .catch(() => setOperator(false));
  }, []);

  useEffect(() => {
    if (!operator) return;
    refresh();
    const t = setInterval(refresh, 5000);
    return () => clearInterval(t);
  }, [operator, refresh]);

  async function onLogin(e: React.FormEvent) {
    e.preventDefault();
    setError(null);
    setBusy(true);
    try {
      await op.login(email.trim(), password);
      setOperator(true);
    } catch (err) {
      setError(String((err as Error).message ?? err));
    } finally {
      setBusy(false);
    }
  }

  async function act(path: string, label: string) {
    setNotice(null);
    try {
      await op.act(path);
      setNotice(`${label} — done`);
      refresh();
    } catch (err) {
      setNotice(`${label} failed: ${String((err as Error).message ?? err)}`);
    }
    setConfirmAction(null);
  }

  // The dangerous levers ask twice; the second click within the same panel is
  // the confirmation.
  function guarded(path: string, label: string) {
    if (confirmAction === path) void act(path, label);
    else setConfirmAction(path);
  }

  if (operator === null) return <div style={{ padding: 28, color: 'var(--app-ink-3)' }}>Loading…</div>;

  if (!operator) {
    return (
      <div style={{ display: 'flex', justifyContent: 'center', paddingTop: '14vh' }}>
        <form onSubmit={onLogin} style={{ width: 340, display: 'flex', flexDirection: 'column', gap: 14 }}>
          <div>
            <div style={{ fontWeight: 800, letterSpacing: '0.14em', fontSize: 14, textTransform: 'uppercase' }}>
              Operator
            </div>
            <div style={{ fontSize: 12, color: 'var(--app-ink-3)', marginTop: 4 }}>
              The contest control surface. Master credential only.
            </div>
          </div>
          <label style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
            <span className="kicker">Email</span>
            <input autoFocus value={email} onChange={(e) => setEmail(e.target.value)} autoComplete="username" style={field} />
          </label>
          <label style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
            <span className="kicker">Password</span>
            <input type="password" value={password} onChange={(e) => setPassword(e.target.value)} autoComplete="current-password" style={field} />
          </label>
          {error && <div style={{ fontSize: 12, color: 'var(--s-fail)' }}>{error}</div>}
          <button
            type="submit"
            disabled={busy || !email.trim() || !password}
            style={{
              border: '1px solid var(--app-ink)',
              background: 'var(--app-ink)',
              color: 'var(--app-bg)',
              font: 'inherit',
              fontWeight: 700,
              fontSize: 12,
              letterSpacing: '0.12em',
              textTransform: 'uppercase',
              padding: '10px 0',
              cursor: busy ? 'wait' : 'pointer',
              opacity: busy || !email.trim() || !password ? 0.6 : 1,
            }}
          >
            {busy ? 'Signing in…' : 'Sign in'}
          </button>
        </form>
      </div>
    );
  }

  const flagOn = (key: string) =>
    health?.flags.some((f) => (f as Record<string, unknown>)[key] === true) ?? false;
  const frozen = flagOn('leaderboard_frozen');
  const closed = flagOn('submissions_closed');
  const published = flagOn('final_published');

  // Safe failover: each late-stage action stays disabled until its
  // precondition holds, so the day's sequence can't be run out of order.
  //  · Queue rejudge waits for submissions to close — otherwise the golden box
  //    would rejudge a moving target.
  //  · Publish final waits for the rejudge to actually FINISH — publishing a
  //    half-formed board is the mistake this guard exists to prevent.
  const rp = rejudge?.progress;
  const rejudgeQueued = (rp?.total ?? 0) > 0;
  const rejudgeFinished = rejudgeQueued && (rp?.running ?? 0) === 0 && (rp?.pending ?? 0) === 0;

  return (
    <div className="page scroll-y" style={{ paddingBottom: 40 }}>
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
            Operator
          </div>
          <div style={{ fontSize: 13, color: 'var(--app-ink-2)', marginTop: 8 }}>
            {health
              ? health.ok
                ? 'Every box healthy. Nothing needs you.'
                : `${health.boxes.unhealthy} of ${health.boxes.total} boxes unhealthy — look below.`
              : 'Loading…'}
          </div>
        </div>
        <div style={{ display: 'flex', gap: 8, alignItems: 'center', flexWrap: 'wrap' }}>
          <span
            className="mono"
            style={{ fontSize: 11, color: health?.ok ? 'var(--s-pass)' : 'var(--s-fail)', fontWeight: 700 }}
          >
            {health ? (health.ok ? '● healthy' : '● attention') : '…'}
          </span>
          <ActionButton label="Sign out" onClick={() => { void op.logout().then(() => setOperator(false)); }} />
        </div>
      </div>

      {notice && (
        <div className="mono" style={{ marginTop: 12, padding: '8px 12px', background: 'var(--s-idle-bg)', fontSize: 12 }}>
          {notice}
        </div>
      )}

      <Panel
        title="Event controls"
        right={
          <span className="mono" style={{ fontSize: 11, color: 'var(--app-ink-3)' }}>
            {closed ? 'submissions CLOSED' : 'submissions open'} · {frozen ? 'board FROZEN' : 'board live'} ·{' '}
            {published ? 'final PUBLISHED' : 'final hidden'}
          </span>
        }
      >
        <div style={{ display: 'flex', gap: 10, padding: '12px 14px', flexWrap: 'wrap' }}>
          <ActionButton
            label={frozen ? 'Unfreeze board' : 'Freeze board'}
            onClick={() => void act(frozen ? 'unfreeze' : 'freeze', frozen ? 'unfreeze' : 'freeze')}
          />
          <ActionButton
            label={closed ? (confirmAction === 'open' ? 'Really reopen?' : 'Reopen submissions') : confirmAction === 'close' ? 'Really close?' : 'Close submissions'}
            danger
            onClick={() => guarded(closed ? 'open' : 'close', closed ? 'reopen' : 'close')}
          />
          <ActionButton
            label={confirmAction === 'rejudge' ? 'Really rejudge?' : 'Queue rejudge'}
            danger
            disabled={!closed}
            reason="Close submissions first — the rejudge measures the final field."
            onClick={() => guarded('rejudge', 'rejudge')}
          />
          <ActionButton
            label={confirmAction === 'publish-final' ? 'Really publish?' : 'Publish final'}
            danger
            disabled={!rejudgeFinished}
            reason={
              !rejudgeQueued
                ? 'Queue the rejudge first — the final standings come from the golden box.'
                : `Rejudge still running — ${rp?.done ?? 0}/${rp?.total ?? 0} done. Wait for it to finish.`
            }
            onClick={() => guarded('publish-final', 'publish final')}
          />
        </div>
      </Panel>

      <Panel title={`Boxes · ${health?.boxes.total ?? 0}`}>
        <div
          className="kicker"
          style={{
            display: 'grid',
            gridTemplateColumns: 'minmax(200px,1.4fr) 104px 150px 66px 88px 64px 96px',
            gap: 10,
            padding: '8px 14px',
            borderBottom: '1px solid var(--app-line)',
            letterSpacing: '0.12em',
          }}
        >
          <span>Box</span>
          <span>Student</span>
          <span>Doing</span>
          <span style={{ textAlign: 'right' }}>Load</span>
          <span style={{ textAlign: 'right' }}>Mem free</span>
          <span style={{ textAlign: 'right' }}>Steal</span>
          <span style={{ textAlign: 'right' }}>Heartbeat</span>
        </div>
        {(health?.boxes.rows ?? []).map((b) => (
          <div
            key={b.id}
            className="mono"
            style={{
              display: 'grid',
              gridTemplateColumns: 'minmax(200px,1.4fr) 104px 150px 66px 88px 64px 96px',
              gap: 10,
              padding: '8px 14px',
              borderBottom: '1px solid var(--app-line)',
              borderLeft: `3px solid ${b.healthy ? 'var(--s-pass)' : 'var(--s-fail)'}`,
              fontSize: 12,
            }}
          >
            <span style={{ overflow: 'hidden', textOverflow: 'ellipsis' }} title={b.id}>{b.id.replace(/^(agent|golden)-ip-/, '').replace(/-0$/, '')}</span>
            <span style={{ color: 'var(--app-ink-2)' }}>
              {b.participant_id != null ? `student ${b.participant_id}` : b.role}
            </span>
            <span style={{ color: b.job && b.job !== 'idle' ? 'var(--s-bench)' : 'var(--app-ink-3)', fontWeight: b.job && b.job !== 'idle' ? 700 : 400 }}>
              {b.job ?? '—'}
            </span>
            <span style={{ textAlign: 'right', color: 'var(--app-ink-2)' }}>
              {b.load1 != null ? b.load1.toFixed(2) : '—'}
            </span>
            <span style={{ textAlign: 'right', color: 'var(--app-ink-2)' }}>
              {b.mem_avail_mb != null ? `${(b.mem_avail_mb / 1024).toFixed(1)}G` : '—'}
            </span>
            <span style={{ textAlign: 'right', color: 'var(--app-ink-3)' }}>
              {b.steal_total != null ? b.steal_total : '—'}
            </span>
            <span style={{ textAlign: 'right', color: b.last_seen_secs_ago > 60 ? 'var(--s-fail)' : 'var(--app-ink-3)' }}>
              {Math.round(b.last_seen_secs_ago)}s ago
            </span>
          </div>
        ))}
      </Panel>

      <Panel title="Students">
        <div
          className="kicker"
          style={{
            display: 'grid',
            gridTemplateColumns: '44px minmax(120px,1.1fr) minmax(190px,1.4fr) 52px 118px 32px',
            padding: '8px 14px',
            borderBottom: '1px solid var(--app-line)',
            letterSpacing: '0.12em',
          }}
        >
          <span>#</span>
          <span>Handle</span>
          <span>Activity</span>
          <span style={{ textAlign: 'right' }}>Best</span>
          <span style={{ textAlign: 'right' }}>Box</span>
          <span></span>
        </div>
        {participants.map((p) => {
          // The status pill's tone follows what the box is doing.
          const a = p.activity;
          const tone =
            p.box_state === 'failed' ? 'var(--s-fail)' :
            p.box_state === 'deploying' || p.box_state === 'redeploying' || p.box_state === 'removing' ? 'var(--s-slow)' :
            a === 'no box' ? 'var(--app-ink-3)' :
            a === 'IDLE' ? 'var(--app-ink-2)' :
            a === 'box unhealthy' ? 'var(--s-fail)' :
            'var(--s-bench)';
          const busy = p.box_state === 'deploying' || p.box_state === 'redeploying' || p.box_state === 'removing';
          return (
            <div
              key={p.participant_id}
              className="mono"
              style={{
                display: 'grid',
                gridTemplateColumns: '44px minmax(120px,1.1fr) minmax(190px,1.4fr) 52px 118px 32px',
                alignItems: 'center',
                padding: '7px 14px',
                borderBottom: '1px solid var(--app-line)',
                fontSize: 12,
              }}
            >
              <span style={{ color: 'var(--app-ink-3)' }}>{p.participant_id}</span>
              <button
                onClick={() => setInspect({ pid: p.participant_id, handle: p.handle })}
                title="View this student's submissions and benchmarks"
                style={{
                  appearance: 'none', border: 0, background: 'transparent', padding: 0,
                  font: 'inherit', fontFamily: 'var(--font-body)', fontWeight: 600, fontSize: 13,
                  color: 'var(--app-ink)', textAlign: 'left', cursor: 'pointer',
                  textDecoration: 'underline', textDecorationColor: 'var(--app-line)',
                  textUnderlineOffset: 3,
                }}
              >
                {p.handle}
              </button>
              <span
                style={{
                  color: tone,
                  fontWeight: 700,
                  overflow: 'hidden',
                  textOverflow: 'ellipsis',
                  whiteSpace: 'nowrap',
                }}
                title={p.box ?? ''}
              >
                {busy && '⟳ '}{a}
              </span>
              <span style={{ textAlign: 'right', fontWeight: 700 }}>
                {p.best_level != null ? `L${p.best_level}` : '—'}
              </span>
              <span style={{ textAlign: 'right' }}>
                {p.box_state === 'none' || p.box_state === 'failed' ? (
                  <button
                    onClick={() => {
                      void op.deployBox(p.participant_id)
                        .then(() => { setNotice(`deploying box for ${p.handle}…`); refresh(); })
                        .catch((e) => setNotice(`deploy failed: ${String((e as Error).message ?? e)}`));
                    }}
                    style={boxBtn(false)}
                  >
                    {p.box_state === 'failed' ? 'Retry' : 'Deploy'}
                  </button>
                ) : busy ? (
                  <span style={{ color: 'var(--s-slow)', fontSize: 11 }}>working…</span>
                ) : (
                  <button
                    onClick={() => {
                      if (confirmAction !== `re-${p.participant_id}`) { setConfirmAction(`re-${p.participant_id}`); return; }
                      setConfirmAction(null);
                      void op.redeployBox(p.participant_id)
                        .then(() => { setNotice(`redeploying box for ${p.handle}…`); refresh(); })
                        .catch((e) => setNotice(`redeploy failed: ${String((e as Error).message ?? e)}`));
                    }}
                    style={boxBtn(confirmAction === `re-${p.participant_id}`)}
                  >
                    {confirmAction === `re-${p.participant_id}` ? 'Confirm?' : 'Redeploy'}
                  </button>
                )}
              </span>
              <span style={{ textAlign: 'right' }}>
                <button
                  title="Remove participant and tear down their box"
                  onClick={() => {
                    if (confirmAction !== `rm-${p.participant_id}`) { setConfirmAction(`rm-${p.participant_id}`); return; }
                    setConfirmAction(null);
                    void op.removeParticipant(p.participant_id)
                      .then(() => { setNotice(`removed ${p.handle}`); refresh(); })
                      .catch((e) => setNotice(`remove failed: ${String((e as Error).message ?? e)}`));
                  }}
                  style={{
                    appearance: 'none', border: 0, background: 'transparent',
                    color: confirmAction === `rm-${p.participant_id}` ? 'var(--s-fail)' : 'var(--app-ink-3)',
                    cursor: 'pointer', fontSize: confirmAction === `rm-${p.participant_id}` ? 10 : 14,
                    fontWeight: 700, padding: '2px 4px',
                  }}
                >
                  {confirmAction === `rm-${p.participant_id}` ? 'sure?' : '×'}
                </button>
              </span>
            </div>
          );
        })}
        <form
          onSubmit={(e) => {
            e.preventDefault();
            if (!newHandle.trim()) return;
            void op
              .issue(newHandle.trim())
              .then((r) => {
                setNotice(`credentials for ${r.handle}: ${r.password} — shown once, write it down`);
                setNewHandle('');
                refresh();
              })
              .catch((err) => setNotice(`issue failed: ${String((err as Error).message ?? err)}`));
          }}
          style={{ display: 'flex', gap: 10, padding: '10px 14px', alignItems: 'center' }}
        >
          <input
            value={newHandle}
            onChange={(e) => setNewHandle(e.target.value)}
            placeholder="handle — issue or reset credentials"
            style={{ ...field, width: 280, fontSize: 12, padding: '6px 10px' }}
          />
          <ActionButton label="Issue" onClick={() => {}} />
        </form>
      </Panel>

      {rejudge && rejudge.progress.total > 0 && (
        <Panel
          title="Phase III · golden-box rejudge"
          right={
            <span className="mono" style={{ fontSize: 11, color: 'var(--app-ink-3)' }}>
              {rejudge.golden
                ? `golden ${rejudge.golden.healthy && rejudge.golden.last_seen_secs_ago < 60 ? 'live' : 'offline'} · seen ${Math.round(rejudge.golden.last_seen_secs_ago)}s ago`
                : 'no golden box'}
            </span>
          }
        >
          <div style={{ padding: '12px 14px' }}>
            {/* progress */}
            <div style={{ display: 'flex', alignItems: 'center', gap: 14, flexWrap: 'wrap' }}>
              <span className="mono" style={{ fontSize: 20, fontWeight: 800 }}>
                {rejudge.progress.done}/{rejudge.progress.total}
              </span>
              <span className="kicker" style={{ fontWeight: 400 }}>rejudged</span>
              {rejudge.progress.errored > 0 && (
                <span className="mono" style={{ color: 'var(--s-fail)', fontSize: 12 }}>
                  {rejudge.progress.errored} errored
                </span>
              )}
              <div style={{ flex: 1, minWidth: 120, height: 6, background: 'var(--app-line)' }}>
                <div
                  style={{
                    width: `${rejudge.progress.total ? (100 * rejudge.progress.done) / rejudge.progress.total : 0}%`,
                    height: '100%',
                    background: 'var(--s-pass)',
                  }}
                />
              </div>
            </div>
            {/* who's running now */}
            <div className="mono" style={{ fontSize: 12, marginTop: 10, color: 'var(--s-bench)', fontWeight: 700 }}>
              {rejudge.current ? `▶ ${rejudge.current}` : rejudge.progress.done < rejudge.progress.total ? 'starting…' : 'complete'}
            </div>
          </div>
          {/* the final leaderboard forming live */}
          {rejudge.results.length > 0 && (
            <>
              <div
                className="kicker"
                style={{
                  display: 'grid',
                  gridTemplateColumns: '40px minmax(0,1fr) 100px 90px 90px 120px',
                  padding: '8px 14px',
                  borderTop: '1px solid var(--app-line)',
                  borderBottom: '1px solid var(--app-line)',
                }}
              >
                <span>#</span><span>Handle</span>
                <span style={{ textAlign: 'right' }}>p95 ns</span>
                <span style={{ textAlign: 'right' }}>p50 ns</span>
                <span style={{ textAlign: 'right' }}>p99 ns</span>
                <span style={{ textAlign: 'right' }}>Outcome</span>
              </div>
              {rejudge.results.map((r, i) => (
                <div
                  key={r.handle}
                  className="mono"
                  style={{
                    display: 'grid',
                    gridTemplateColumns: '40px minmax(0,1fr) 100px 90px 90px 120px',
                    padding: '6px 14px',
                    borderBottom: '1px solid var(--app-line)',
                    fontSize: 12,
                  }}
                >
                  <span style={{ fontWeight: 700, color: i < 3 ? 'var(--color-accent)' : 'var(--app-ink-3)' }}>{i + 1}</span>
                  <span style={{ fontFamily: 'var(--font-body)', fontWeight: 600 }}>{r.handle}</span>
                  <span style={{ textAlign: 'right', fontWeight: 700 }}>{r.p95_ns ? Math.round(r.p95_ns) : '—'}</span>
                  <span style={{ textAlign: 'right', color: 'var(--app-ink-2)' }}>{r.p50_ns ? Math.round(r.p50_ns) : '—'}</span>
                  <span style={{ textAlign: 'right', color: 'var(--app-ink-3)' }}>{r.p99_ns ? Math.round(r.p99_ns) : '—'}</span>
                  <span style={{ textAlign: 'right', fontWeight: 700, color: r.finished ? 'var(--s-pass)' : 'var(--s-fail)' }}>
                    {r.finished ? 'finished' : 'DNF'}
                  </span>
                </div>
              ))}
            </>
          )}
        </Panel>
      )}

      <Panel title="Pipeline">
        <div style={{ display: 'flex', gap: 22, padding: '12px 14px', flexWrap: 'wrap' }}>
          {(health?.submissions ?? []).map((s) => (
            <div key={s.state} style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
              <span className="kicker" style={{ fontWeight: 400 }}>{s.state}</span>
              <span className="mono" style={{ fontSize: 20, fontWeight: 800 }}>{s.count}</span>
            </div>
          ))}
          <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
            <span className="kicker" style={{ fontWeight: 400 }}>runs pending</span>
            <span className="mono" style={{ fontSize: 20, fontWeight: 800 }}>{health?.run_jobs_pending ?? 0}</span>
          </div>
          {(health?.rejudge ?? []).map((s) => (
            <div key={`rj-${s.state}`} style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
              <span className="kicker" style={{ fontWeight: 400 }}>rejudge {s.state}</span>
              <span className="mono" style={{ fontSize: 20, fontWeight: 800 }}>{s.count}</span>
            </div>
          ))}
        </div>
      </Panel>

      <Panel title="Recent events">
        {events.slice(0, 20).map((e) => (
          <div
            key={e.id}
            className="mono"
            style={{
              display: 'grid',
              gridTemplateColumns: '150px minmax(0,1fr) 90px',
              padding: '6px 14px',
              borderBottom: '1px solid var(--app-line)',
              fontSize: 12,
            }}
          >
            <span style={{ color: 'var(--app-ink-3)' }}>
              {new Date(e.at).toLocaleTimeString('en-GB', { hour12: false })}
            </span>
            <span>{e.kind}</span>
            <span style={{ textAlign: 'right', color: 'var(--app-ink-3)' }}>
              {e.submission_id != null ? `#${e.submission_id}` : ''}
            </span>
          </div>
        ))}
      </Panel>

      {inspect && (
        <StudentInspector
          pid={inspect.pid}
          handle={inspect.handle}
          onClose={() => setInspect(null)}
        />
      )}
    </div>
  );
}
