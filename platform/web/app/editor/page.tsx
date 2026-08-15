'use client';

// The editor is the SOLE submission path — there is no CLI client (impl spec
// section 8). One editable buffer: the whole engine.cpp translation unit. The
// boundary is the file, not locked sub-regions; the server compiles only this
// TU against read-only headers, so nothing outside it is editable anyway.

import Editor, { loader } from '@monaco-editor/react';
import { useRouter } from 'next/navigation';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api, isTerminal, type RunResult, type Submission } from '@/lib/api';
import { HEADER_NAMES, HEADERS, STARTING_BUFFER } from '@/lib/boilerplate.generated';
import { useIdentity } from '@/lib/identity';
import { useTheme } from '@/lib/theme';
import { inkOf, stateShort } from '@/components/StateTokens';

// Monaco is served from our own origin, not a CDN. The editor is the only way
// to submit; it must not depend on the room's internet.
loader.config({ paths: { vs: '/monaco/vs' } });

// No LSP, no clangd. The compiler's own errors, shown verbatim, are the
// feedback loop (impl spec section 3).
const EDITOR_OPTIONS = {
  minimap: { enabled: false },
  fontSize: 12.5,
  lineHeight: 19,
  fontFamily: 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace',
  scrollBeyondLastLine: false,
  renderWhitespace: 'selection',
  tabSize: 2,
  automaticLayout: true,
  quickSuggestions: false,
  padding: { top: 12, bottom: 12 },
  renderLineHighlight: 'none',
  overviewRulerLanes: 0,
} as const;

/// 800ms, and the copy in the right rail says so. Long enough that a burst of
/// typing is one request, short enough that closing the lid mid-thought is safe.
const AUTOSAVE_MS = 800;
const POLL_MS = 2000;

type Tab = 'engine.cpp' | (typeof HEADER_NAMES)[number];
type SaveState = 'saved' | 'unsaved' | 'saving' | 'error';

const LockIcon = ({ size = 10 }: { size?: number }) => (
  <svg
    width={size}
    height={size * 1.2}
    viewBox="0 0 10 12"
    fill="none"
    stroke="currentColor"
    strokeWidth={1.4}
    aria-hidden
  >
    <rect x="1" y="5" width="8" height="6" />
    <path d="M3 5V3a2 2 0 0 1 4 0v2" />
  </svg>
);

export default function EditorPage() {
  const { identity, ready } = useIdentity();
  const router = useRouter();
  const { theme } = useTheme();

  const [tab, setTab] = useState<Tab>('engine.cpp');
  const [source, setSource] = useState<string>(STARTING_BUFFER);
  const [loaded, setLoaded] = useState(false);
  const [save, setSave] = useState<SaveState>('saved');
  const [confirmReset, setConfirmReset] = useState(false);

  const [running, setRunning] = useState(false);
  const [runId, setRunId] = useState<number | null>(null);
  const [runResult, setRunResult] = useState<RunResult | null>(null);
  const [runAt, setRunAt] = useState<number | null>(null);
  const [mine, setMine] = useState<Submission[]>([]);
  const [gate, setGate] = useState<{ ready: boolean; wait: number; reason: string } | null>(null);
  const [error, setError] = useState<string | null>(null);

  const saveTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const latest = useRef(source);
  latest.current = source;

  // Belt and braces. The two-editor layout means onChange can only ever carry
  // the engine.cpp buffer — but an earlier build wired the headers through the
  // same editor, and its onChange quietly SAVED header content over people's
  // drafts. The render bug was fixable in an afternoon; the saved drafts were
  // not. A guard that can never legitimately fire is cheap next to that.
  const isFrozenHeader = useCallback(
    (text: string) => HEADER_NAMES.some((h) => HEADERS[h] === text),
    [],
  );

  // The server owns the buffer, so a browser crash loses nothing and a
  // participant returning on another machine picks up where they left off.
  useEffect(() => {
    if (!identity) return;
    let alive = true;
    api
      .getDraft()
      .then((d) => {
        if (!alive) return;
        // A stored draft byte-identical to a frozen header is corruption from
        // the earlier editor bug, not somebody's work.
        if (d.source && d.source.trim().length > 0 && !isFrozenHeader(d.source)) {
          setSource(d.source);
        }
        setLoaded(true);
      })
      .catch(() => alive && setLoaded(true));
    return () => {
      alive = false;
    };
  }, [identity, isFrozenHeader]);

  const refreshMine = useCallback(() => {
    if (!identity) return;
    api
      .mine()
      .then(setMine)
      .catch(() => {});
    api
      .queue()
      .then((q) => setGate({ ready: !q.evaluating, wait: 0, reason: q.reason }))
      .catch(() => {});
  }, [identity]);

  useEffect(refreshMine, [refreshMine]);

  // Poll while anything of mine is still moving. Submit is blocked only while a
  // benchmark of mine is already queued, and that clears through this poll, so
  // there is no clock to tick between refreshes.
  const anyMoving = useMemo(() => mine.some((s) => !isTerminal(s.state)), [mine]);
  useEffect(() => {
    if (!identity) return;
    const t = setInterval(refreshMine, anyMoving ? POLL_MS : 15000);
    return () => clearInterval(t);
  }, [identity, anyMoving, refreshMine]);

  // The pool claims run_jobs ahead of submissions, so this comes back quickly.
  useEffect(() => {
    if (runId == null) return;
    const t = setInterval(() => {
      api
        .runResult(runId)
        .then((r) => {
          if (!r.terminal) return;
          clearInterval(t);
          setRunning(false);
          setRunId(null);
          setRunAt(Date.now());
          setRunResult(r.run.result ?? { compiled: false, error: 'no result recorded' });
        })
        .catch(() => {});
    }, 1000);
    return () => clearInterval(t);
  }, [runId]);

  const scheduleSave = useCallback(
    (next: string) => {
      if (!identity) return;
      if (isFrozenHeader(next)) {
        console.warn('refusing to save a frozen header as a draft');
        return;
      }
      setSave('unsaved');
      if (saveTimer.current) clearTimeout(saveTimer.current);
      saveTimer.current = setTimeout(() => {
        setSave('saving');
        api
          .saveDraft(next)
          .then(() => setSave('saved'))
          .catch(() => setSave('error'));
      }, AUTOSAVE_MS);
    },
    [identity, isFrozenHeader],
  );

  const onRun = useCallback(async () => {
    if (!identity || running) return;
    setError(null);
    setRunning(true);
    setRunResult(null);
    try {
      await api.saveDraft(latest.current);
      setSave('saved');
      const { run_id } = await api.run(latest.current);
      setRunId(run_id);
    } catch (e) {
      setError(String((e as Error).message ?? e));
      setRunning(false);
    }
  }, [identity, running]);

  async function onSubmit() {
    if (!identity || !gate?.ready) return;
    setError(null);
    try {
      await api.saveDraft(latest.current);
      setSave('saved');
      const res = await api.submit(latest.current);
      refreshMine();
      router.push(`/submissions/${res.submission_id}`);
    } catch (e) {
      setError(String((e as Error).message ?? e));
    }
  }

  // ⌘↵ / Ctrl+↵ runs, which is what the button's own hint promises.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key === 'Enter') {
        e.preventDefault();
        void onRun();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onRun]);

  const readOnly = tab !== 'engine.cpp';
  const shown = readOnly ? HEADERS[tab as (typeof HEADER_NAMES)[number]] : source;
  const lineCount = shown.split('\n').length;
  const saveLabel = readOnly ? 'read-only' : save;
  const saveDot = readOnly
    ? 'var(--s-idle)'
    : save === 'saved'
      ? 'var(--s-pass)'
      : save === 'saving'
        ? 'var(--s-slow)'
        : 'var(--s-fail)';

  if (ready && !identity) {
    return (
      <div style={{ padding: 48, maxWidth: 560 }}>
        <div style={{ fontWeight: 800, fontSize: 24, marginBottom: 8 }}>Pick your handle</div>
        <p style={{ color: 'var(--app-ink-2)', fontSize: 14 }}>
          Choose it from the <strong>Handle</strong> menu in the top bar. There is no password —
          everything you submit is attributed to whatever you pick, so pick your own.
        </p>
      </div>
    );
  }

  return (
    <div
      style={{
        display: 'grid',
        gridTemplateColumns: 'minmax(0,1fr) 336px',
        flex: 1,
        minHeight: 0,
      }}
    >
      {/* ── code column ─────────────────────────────────────────── */}
      <div
        style={{
          display: 'flex',
          flexDirection: 'column',
          minWidth: 0,
          overflow: 'hidden',
          borderRight: '2px solid var(--app-rule)',
        }}
      >
        <div
          style={{
            display: 'flex',
            alignItems: 'stretch',
            height: 38,
            flex: 'none',
            borderBottom: '1px solid var(--app-line)',
            background: 'var(--app-panel)',
          }}
        >
          <button
            onClick={() => setTab('engine.cpp')}
            className="mono"
            style={{
              appearance: 'none',
              border: 0,
              borderRight: '1px solid var(--app-line)',
              borderBottom: `3px solid ${readOnly ? 'transparent' : 'var(--color-accent)'}`,
              background: readOnly ? 'transparent' : 'var(--app-code)',
              color: 'var(--app-ink)',
              fontSize: 12,
              fontWeight: 700,
              padding: '0 16px',
              cursor: 'pointer',
              display: 'flex',
              alignItems: 'center',
              gap: 8,
            }}
          >
            <span
              style={{ width: 7, height: 7, background: 'var(--color-accent)', display: 'inline-block' }}
            />
            engine.cpp
          </button>

          {HEADER_NAMES.map((h) => (
            <button
              key={h}
              onClick={() => setTab(h)}
              className="mono"
              title={`${h} is frozen — compiled as-is on the server`}
              style={{
                appearance: 'none',
                border: 0,
                borderRight: '1px solid var(--app-line)',
                borderBottom: `3px solid ${tab === h ? 'var(--app-ink-3)' : 'transparent'}`,
                background: tab === h ? 'var(--app-code)' : 'transparent',
                color: 'var(--app-ink-2)',
                fontSize: 12,
                padding: '0 14px',
                cursor: 'pointer',
                display: 'flex',
                alignItems: 'center',
                gap: 7,
              }}
            >
              <LockIcon />
              {h}
            </button>
          ))}

          <div style={{ flex: 1, borderRight: '1px solid var(--app-line)' }} />
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '0 14px' }}>
            <span style={{ width: 7, height: 7, borderRadius: '50%', background: saveDot }} />
            <span
              style={{
                fontSize: 10,
                letterSpacing: '0.12em',
                textTransform: 'uppercase',
                fontWeight: 700,
                color: 'var(--app-ink-2)',
              }}
            >
              {saveLabel}
            </span>
          </div>
        </div>

        {/* The read-only-ness of a header is a correctness requirement, not a
            hint, so it is stated in a band the eye cannot miss rather than
            inferred from a greyed-out cursor. */}
        {readOnly && (
          <div
            style={{
              display: 'flex',
              alignItems: 'center',
              gap: 12,
              padding: '9px 16px',
              flex: 'none',
              background:
                'repeating-linear-gradient(135deg,var(--s-idle-bg),var(--s-idle-bg) 8px,transparent 8px,transparent 16px)',
              borderBottom: '1px solid var(--app-line)',
            }}
          >
            <LockIcon size={13} />
            <span
              style={{
                fontWeight: 800,
                fontSize: 11,
                letterSpacing: '0.12em',
                textTransform: 'uppercase',
              }}
            >
              Frozen contract · read-only
            </span>
            <span style={{ color: 'var(--app-ink-2)', fontSize: 12 }}>
              {tab} is compiled as-is on the server. Edits here would be discarded.
            </span>
          </div>
        )}

        {/*
          TWO editors, and the split is deliberate.

          The editable buffer is bound to engine.cpp and stays mounted for the
          whole session; the headers get their own read-only instance with NO
          onChange handler at all. Different paths mean different models, so
          they cannot interfere.

          What corrupted people's drafts earlier was a single editor whose
          `path` followed the active tab: its onChange closed over a stale
          `readOnly`, so when Monaco loaded a header into the model the "don't
          save read-only content" guard had not caught up, and the autosave
          wrote out.h over the buffer. The fix is not fewer editors — it is that
          the instance showing a header has no way to write anything.
        */}
        <div style={{ flex: '1 1 0', minHeight: 0, background: 'var(--app-code)' }}>
          <div style={{ height: '100%', display: readOnly ? 'none' : 'block' }}>
            <Editor
              height="100%"
              theme={theme === 'dark' ? 'vs-dark' : 'vs'}
              defaultLanguage="cpp"
              path="engine.cpp"
              keepCurrentModel
              value={source}
              onChange={(v) => {
                const next = v ?? '';
                setSource(next);
                scheduleSave(next);
              }}
              options={EDITOR_OPTIONS}
            />
          </div>
          {readOnly && (
            <div style={{ height: '100%' }}>
              <Editor
                height="100%"
                theme={theme === 'dark' ? 'vs-dark' : 'vs'}
                defaultLanguage="cpp"
                path={tab}
                value={shown}
                options={{ ...EDITOR_OPTIONS, readOnly: true, domReadOnly: true }}
              />
            </div>
          )}
        </div>

        <div
          className="mono"
          style={{
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
            height: 30,
            flex: 'none',
            padding: '0 14px',
            borderTop: '1px solid var(--app-line)',
            background: 'var(--app-panel)',
            fontSize: 11,
            color: 'var(--app-ink-2)',
          }}
        >
          <span>C++20 · -O2 -march=x86-64-v3 -fno-omit-frame-pointer</span>
          <span>
            {lineCount} lines · {shown.length} bytes
          </span>
        </div>
      </div>

      {/* ── right rail ──────────────────────────────────────────── */}
      <div className="scroll-y" style={{ display: 'flex', flexDirection: 'column', minWidth: 0 }}>
        <div
          style={{
            padding: 14,
            borderBottom: '1px solid var(--app-line)',
            display: 'flex',
            flexDirection: 'column',
            gap: 10,
          }}
        >
          <button
            onClick={onRun}
            disabled={running || !loaded}
            style={{
              appearance: 'none',
              width: '100%',
              border: '1px solid var(--app-rule)',
              background: 'transparent',
              color: 'var(--app-ink)',
              font: 'inherit',
              fontWeight: 800,
              fontSize: 13,
              letterSpacing: '0.06em',
              textTransform: 'uppercase',
              padding: '11px 14px',
              cursor: running ? 'progress' : 'pointer',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'space-between',
              textAlign: 'left',
              opacity: running || !loaded ? 0.6 : 1,
            }}
          >
            <span>{running ? 'Compiling…' : 'Run'}</span>
            <span
              style={{
                fontWeight: 500,
                fontSize: 11,
                color: 'var(--app-ink-2)',
                letterSpacing: '0.04em',
                textTransform: 'none',
              }}
            >
              41 visible tests · ⌘↵
            </span>
          </button>

          {/* Submit is the consequential action, so it is the only accent-filled
              control on the page. It is never disabled: holding an outstanding
              benchmark does not stop you submitting, it only changes what
              happens next — a waiting job is replaced, a running one is left
              alone and yours queues behind it. The line underneath says which,
              because the button cannot. */}
          <button
            onClick={onSubmit}
            disabled={!loaded}
            style={{
              appearance: 'none',
              width: '100%',
              border: '2px solid var(--color-accent)',
              background: 'var(--color-accent)',
              color: '#fff',
              font: 'inherit',
              fontWeight: 800,
              fontSize: 13,
              letterSpacing: '0.06em',
              textTransform: 'uppercase',
              padding: '13px 14px',
              cursor: loaded ? 'pointer' : 'progress',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'space-between',
              textAlign: 'left',
              opacity: loaded ? 1 : 0.6,
            }}
          >
            <span>Submit</span>
            <span className="mono" style={{ fontWeight: 700, fontSize: 13 }}>
              {gate == null ? '…' : gate.ready ? 'unlimited' : 'queues after this one'}
            </span>
          </button>

          <div style={{ fontSize: 11, color: 'var(--app-ink-2)', lineHeight: 1.5 }}>
            {gate == null
              ? 'Checking your benchmark slot…'
              : gate.ready
                ? 'Runs the hidden correctness check, then queues a ranked benchmark. Submit as often as you like — the only limit is one benchmark of yours in the queue at a time.'
                : `Runs the hidden correctness check. Your ${gate.reason} — nothing is lost and you do not need to resubmit.`}
          </div>

          {error && (
            <div
              className="mono"
              style={{
                fontSize: 11.5,
                color: 'var(--s-fail)',
                background: 'var(--s-fail-bg)',
                padding: '8px 10px',
              }}
            >
              {error}
            </div>
          )}
        </div>

        <LastRun result={runResult} running={running} at={runAt} />

        <div style={{ padding: 14, borderBottom: '1px solid var(--app-line)' }}>
          <div className="kicker" style={{ marginBottom: 10 }}>
            Your submissions
          </div>
          {mine.length === 0 ? (
            <div style={{ fontSize: 12, color: 'var(--app-ink-3)' }}>
              Nothing submitted yet. Run is unlimited; submit when the visible tests pass.
            </div>
          ) : (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 1 }}>
              {mine.slice(0, 6).map((s) => (
                <button
                  key={s.id}
                  onClick={() => router.push(`/submissions/${s.id}`)}
                  style={{
                    appearance: 'none',
                    border: 0,
                    borderLeft: `3px solid ${inkOf(s.state)}`,
                    background: 'var(--app-panel)',
                    color: 'var(--app-ink)',
                    font: 'inherit',
                    padding: '8px 10px',
                    cursor: 'pointer',
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'space-between',
                    gap: 8,
                    textAlign: 'left',
                  }}
                >
                  <span style={{ display: 'flex', flexDirection: 'column', gap: 2, minWidth: 0 }}>
                    <span
                      style={{
                        fontWeight: 700,
                        fontSize: 11,
                        letterSpacing: '0.08em',
                        textTransform: 'uppercase',
                        color: inkOf(s.state),
                      }}
                    >
                      {stateShort(s.state)}
                    </span>
                    <span className="mono" style={{ fontSize: 10, color: 'var(--app-ink-3)' }}>
                      #{s.id} · {hhmm(s.created_at)}
                    </span>
                  </span>
                  <span className="mono" style={{ fontSize: 14, fontWeight: 700 }}>
                    {s.max_level != null ? `L${s.max_level}` : '—'}
                  </span>
                </button>
              ))}
            </div>
          )}
        </div>

        <div style={{ padding: 14, display: 'flex', flexDirection: 'column', gap: 8 }}>
          <div className="kicker">Buffer</div>
          <button
            className="btn btn-secondary"
            onClick={() => setConfirmReset(true)}
            style={{ width: '100%' }}
          >
            Reset to skeleton
          </button>
          <div style={{ fontSize: 11, color: 'var(--app-ink-3)', lineHeight: 1.5 }}>
            Drafts save to the server {AUTOSAVE_MS}ms after you stop typing. Closing the lid is safe.
          </div>
        </div>
      </div>

      {confirmReset && (
        <div className="dialog-backdrop" onClick={() => setConfirmReset(false)}>
          <div className="dialog" onClick={(e) => e.stopPropagation()}>
            <div className="dialog-title">Reset engine.cpp?</div>
            <div className="dialog-body">
              Your current buffer ({source.split('\n').length} lines · {source.length} bytes) is
              replaced by the original skeleton. The draft on the server is overwritten. This cannot
              be undone.
            </div>
            <div className="dialog-actions">
              <button className="btn btn-secondary" onClick={() => setConfirmReset(false)}>
                Keep editing
              </button>
              <button
                className="btn btn-primary"
                onClick={() => {
                  setSource(STARTING_BUFFER);
                  setTab('engine.cpp');
                  setConfirmReset(false);
                  if (identity) {
                    setSave('saving');
                    api
                      .saveDraft(STARTING_BUFFER)
                      .then(() => setSave('saved'))
                      .catch(() => setSave('error'));
                  }
                }}
              >
                Reset
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

/// The Run result: a score, a bar per visible test, and the failures named.
/// Judged only on what the run actually reported — a run that did not compile
/// shows the compiler, not a score of zero, because those are different facts.
function LastRun({
  result,
  running,
  at,
}: {
  result: RunResult | null;
  running: boolean;
  at: number | null;
}) {
  const [, tick] = useState(0);
  useEffect(() => {
    const t = setInterval(() => tick((n) => n + 1), 5000);
    return () => clearInterval(t);
  }, []);

  const tests = result?.tests;
  const total = tests?.total ?? 41;
  const passed = tests?.passed ?? 0;
  const failing = tests ? total - passed : 0;

  return (
    <div style={{ padding: 14, borderBottom: '1px solid var(--app-line)' }}>
      <div
        style={{
          display: 'flex',
          alignItems: 'baseline',
          justifyContent: 'space-between',
          marginBottom: 10,
        }}
      >
        <span className="kicker">Last run</span>
        <span className="mono" style={{ fontSize: 11, color: 'var(--app-ink-3)' }}>
          {running ? 'running…' : at ? `${Math.round((Date.now() - at) / 1000)}s ago` : '—'}
        </span>
      </div>

      {!result ? (
        <div style={{ fontSize: 12, color: 'var(--app-ink-3)' }}>
          {running
            ? 'Compiling and running the 41 visible tests.'
            : 'Press Run. It is unlimited and takes a couple of seconds.'}
        </div>
      ) : !result.compiled ? (
        <>
          <div
            style={{
              fontWeight: 800,
              fontSize: 12,
              letterSpacing: '0.1em',
              textTransform: 'uppercase',
              color: 'var(--s-fail)',
            }}
          >
            Did not compile
          </div>
          <pre
            className="mono"
            style={{
              margin: '10px 0 0',
              padding: 10,
              background: 'var(--s-fail-bg)',
              color: 'var(--app-ink)',
              fontSize: 11,
              lineHeight: 1.5,
              maxHeight: 220,
              overflow: 'auto',
              whiteSpace: 'pre-wrap',
            }}
          >
            {result.stderr ?? result.error ?? 'no compiler output recorded'}
          </pre>
        </>
      ) : (
        <>
          <div style={{ display: 'flex', alignItems: 'baseline', gap: 10 }}>
            <span
              className="mono"
              style={{
                fontSize: 34,
                fontWeight: 700,
                lineHeight: 1,
                color: failing ? 'var(--s-fail)' : 'var(--s-pass)',
              }}
            >
              {passed}/{total}
            </span>
            <span
              style={{
                fontWeight: 800,
                fontSize: 12,
                letterSpacing: '0.1em',
                textTransform: 'uppercase',
                color: failing ? 'var(--s-fail)' : 'var(--s-pass)',
              }}
            >
              {failing ? `${failing} failing` : 'all passing'}
            </span>
          </div>

          {/* One bar per test, in file order. At a glance: how many, and
              whether the failures are clustered or scattered. */}
          <div style={{ display: 'flex', height: 6, gap: 2, marginTop: 12 }}>
            {(tests?.tests ?? []).map((t, i) => (
              <div
                key={i}
                title={`${t.name} — ${t.passed ? 'pass' : 'fail'}`}
                style={{ flex: 1, background: t.passed ? 'var(--s-pass)' : 'var(--s-fail)' }}
              />
            ))}
          </div>

          {failing > 0 && (
            <div style={{ display: 'flex', flexDirection: 'column', gap: 1, marginTop: 14 }}>
              {(tests?.tests ?? [])
                .filter((t) => !t.passed)
                .slice(0, 8)
                .map((t) => (
                  <div
                    key={t.name}
                    style={{
                      display: 'flex',
                      alignItems: 'center',
                      justifyContent: 'space-between',
                      gap: 8,
                      padding: '6px 8px',
                      background: 'var(--s-fail-bg)',
                    }}
                  >
                    <span
                      style={{
                        display: 'flex',
                        alignItems: 'center',
                        gap: 8,
                        fontSize: 12,
                        fontWeight: 600,
                        minWidth: 0,
                      }}
                    >
                      <span className="mono" style={{ fontWeight: 700, color: 'var(--s-fail)' }}>
                        ✕
                      </span>
                      <span
                        style={{
                          overflow: 'hidden',
                          textOverflow: 'ellipsis',
                          whiteSpace: 'nowrap',
                        }}
                      >
                        {t.name}
                      </span>
                    </span>
                    <span className="mono" style={{ fontSize: 11, color: 'var(--app-ink-2)' }}>
                      {t.rule}
                    </span>
                  </div>
                ))}
              {failing > 8 && (
                <div style={{ fontSize: 11, color: 'var(--app-ink-3)', padding: '4px 8px' }}>
                  and {failing - 8} more
                </div>
              )}
            </div>
          )}
        </>
      )}
    </div>
  );
}

function hhmm(iso: string | null) {
  if (!iso) return '—';
  return new Date(iso).toLocaleTimeString('en-GB', {
    hour: '2-digit',
    minute: '2-digit',
    hour12: false,
  });
}
