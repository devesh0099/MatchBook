'use client';

// The operator dashboard (M7's read surface, rendered). Sign in with the
// master credential (ADMIN_EMAIL / ADMIN_PASSWORD, deployment config); every
// panel below is one of the /op endpoints polling every 5s. The actions
// column is the whole event-day control surface: freeze, close, rejudge,
// publish. Students never see this page — the operator session is a separate
// lane from participant sessions by construction.

import { useCallback, useEffect, useState } from 'react';
import { op, type OpHealth, type OpParticipantRow } from '@/lib/api';

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
}: {
  label: string;
  danger?: boolean;
  onClick: () => void;
}) {
  return (
    <button
      onClick={onClick}
      style={{
        appearance: 'none',
        border: `1px solid ${danger ? 'var(--s-fail)' : 'var(--app-rule)'}`,
        background: 'transparent',
        color: danger ? 'var(--s-fail)' : 'var(--app-ink)',
        font: 'inherit',
        fontWeight: 700,
        fontSize: 11,
        letterSpacing: '0.1em',
        textTransform: 'uppercase',
        padding: '6px 12px',
        cursor: 'pointer',
      }}
    >
      {label}
    </button>
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

  const refresh = useCallback(() => {
    op.health().then(setHealth).catch(() => {});
    op.participants().then((r) => setParticipants(r.participants)).catch(() => {});
    op.events().then((r) => setEvents(r.events)).catch(() => {});
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
            onClick={() => guarded('rejudge', 'rejudge')}
          />
          <ActionButton
            label={confirmAction === 'publish-final' ? 'Really publish?' : 'Publish final'}
            danger
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
            p.box_state === 'deploying' || p.box_state === 'redeploying' ? 'var(--s-slow)' :
            a === 'no box' ? 'var(--app-ink-3)' :
            a === 'IDLE' ? 'var(--app-ink-2)' :
            a === 'box unhealthy' ? 'var(--s-fail)' :
            'var(--s-bench)';
          const busy = p.box_state === 'deploying' || p.box_state === 'redeploying';
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
              <span style={{ fontFamily: 'var(--font-body)', fontWeight: 600, fontSize: 13 }}>{p.handle}</span>
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
    </div>
  );
}
