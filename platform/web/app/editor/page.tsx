'use client';

// The editor is the SOLE submission path — there is no CLI client (impl spec
// section 8). One editable buffer: the whole engine.cpp translation unit. The
// boundary is the file, not locked sub-regions; the server compiles only this
// TU against read-only headers, so nothing outside it is editable anyway.

import Editor, { loader } from '@monaco-editor/react';
import { useCallback, useEffect, useRef, useState } from 'react';
import { api, isTerminal, type RunResult, type Submission, type SubmitResponse } from '@/lib/api';
import { HEADER_NAMES, HEADERS, STARTING_BUFFER } from '@/lib/boilerplate.generated';
import { useIdentity } from '@/lib/identity';
import { HandleGate } from '@/components/HandleGate';
import { RunPanel, SubmissionPanel } from '@/components/ResultsPanel';

// Monaco is served from our own origin, not a CDN. The editor is the only way
// to submit; it must not depend on the room's internet.
loader.config({ paths: { vs: '/monaco/vs' } });

// No LSP, no clangd. The compiler's own errors, shown verbatim in the results
// panel, are the feedback loop (impl spec section 3).
const EDITOR_OPTIONS = {
  minimap: { enabled: false },
  fontSize: 13,
  fontFamily: "ui-monospace, 'JetBrains Mono', Menlo, monospace",
  scrollBeyondLastLine: false,
  renderWhitespace: 'selection',
  tabSize: 2,
  automaticLayout: true,
  quickSuggestions: false,
} as const;

const AUTOSAVE_MS = 3000;
const POLL_MS = 2000;

type Tab = 'engine.cpp' | (typeof HEADER_NAMES)[number];

export default function EditorPage() {
  return (
    <main className="main fill">
      <HandleGate>
        <EditorInner />
      </HandleGate>
    </main>
  );
}

function EditorInner() {
  const { identity } = useIdentity();
  const [tab, setTab] = useState<Tab>('engine.cpp');
  const [source, setSource] = useState<string>(STARTING_BUFFER);
  const [loaded, setLoaded] = useState(false);
  const [saved, setSaved] = useState<'idle' | 'saving' | 'saved' | 'error'>('idle');

  const [running, setRunning] = useState(false);
  const [runId, setRunId] = useState<number | null>(null);
  const [runResult, setRunResult] = useState<RunResult | null>(null);
  const [submitInfo, setSubmitInfo] = useState<SubmitResponse | null>(null);
  const [submission, setSubmission] = useState<Submission | null>(null);
  const [error, setError] = useState<string | null>(null);

  const saveTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const latest = useRef(source);
  latest.current = source;

  // The server is the source of truth for the buffer, so a browser crash loses
  // nothing. A participant returning on another machine picks up where they
  // left off.
  useEffect(() => {
    if (!identity) return;
    let alive = true;
    api
      .getDraft(identity.id)
      .then((d) => {
        if (!alive) return;
        if (d.source && d.source.trim().length > 0) setSource(d.source);
        setLoaded(true);
      })
      .catch(() => alive && setLoaded(true));
    return () => {
      alive = false;
    };
  }, [identity]);

  const scheduleSave = useCallback(
    (next: string) => {
      if (!identity) return;
      setSaved('saving');
      if (saveTimer.current) clearTimeout(saveTimer.current);
      saveTimer.current = setTimeout(() => {
        api
          .saveDraft(identity.id, next)
          .then(() => setSaved('saved'))
          .catch(() => setSaved('error'));
      }, AUTOSAVE_MS);
    },
    [identity],
  );

  // Poll while the submission is still moving. Plain JSON every 2s — no
  // websockets for a four-hour event with 18 people.
  useEffect(() => {
    if (!submission || isTerminal(submission.state)) return;
    const t = setInterval(() => {
      api
        .submission(submission.id)
        .then((r) => setSubmission(r.submission))
        .catch(() => {});
    }, POLL_MS);
    return () => clearInterval(t);
  }, [submission]);

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
          setRunResult(r.run.result ?? { compiled: false, error: 'no result recorded' });
        })
        .catch(() => {});
    }, 1000);
    return () => clearInterval(t);
  }, [runId]);

  async function onRun() {
    if (!identity) return;
    setError(null);
    setRunning(true);
    setRunResult(null);
    setSubmission(null);
    try {
      await api.saveDraft(identity.id, latest.current);
      const { run_id } = await api.run(identity.id, latest.current);
      setRunId(run_id);
    } catch (e) {
      setError(String((e as Error).message ?? e));
      setRunning(false);
    }
  }

  async function onSubmit() {
    if (!identity) return;
    setError(null);
    setSubmitInfo(null);
    setSubmission(null);
    try {
      await api.saveDraft(identity.id, latest.current);
      const res = await api.submit(identity.id, latest.current);
      setSubmitInfo(res);
      setRunResult(null);
      const full = await api.submission(res.submission_id);
      setSubmission(full.submission);
    } catch (e) {
      setError(String((e as Error).message ?? e));
    }
  }

  const readOnly = tab !== 'engine.cpp';
  const value = tab === 'engine.cpp' ? source : HEADERS[tab];

  return (
    <div style={{ display: 'grid', gridTemplateColumns: '1fr 420px', height: '100%', minHeight: 0 }}>
      <section style={{ display: 'flex', flexDirection: 'column', minWidth: 0, borderRight: '1px solid var(--line)' }}>
        <div
          style={{
            display: 'flex',
            alignItems: 'center',
            gap: 2,
            padding: '0 10px',
            height: 38,
            borderBottom: '1px solid var(--line)',
            background: 'var(--panel)',
          }}
        >
          <FileTab active={tab === 'engine.cpp'} onClick={() => setTab('engine.cpp')}>
            engine.cpp
          </FileTab>
          {HEADER_NAMES.map((h) => (
            <FileTab key={h} active={tab === h} onClick={() => setTab(h)} muted>
              {h}
            </FileTab>
          ))}
          <div style={{ flex: 1 }} />
          <span className="mono" style={{ fontSize: 11, color: 'var(--faint)' }}>
            {readOnly ? 'read-only' : saveLabel(saved)}
          </span>
        </div>

        {/*
          The editable buffer stays MOUNTED for the whole session and is merely
          hidden behind a header tab. @monaco-editor/react disposes a model when
          `path` changes (keepCurrentModel defaults to false), so driving one
          editor across tabs threw away the participant's work — and their undo
          history and cursor with it. Headers get their own instance instead.
        */}
        <div style={{ flex: 1, minHeight: 0, display: readOnly ? 'none' : 'block' }}>
          <Editor
            height="100%"
            theme="vs-dark"
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
          <div style={{ flex: 1, minHeight: 0 }}>
            <Editor
              height="100%"
              theme="vs-dark"
              defaultLanguage="cpp"
              path={tab}
              value={value}
              options={{ ...EDITOR_OPTIONS, readOnly: true }}
            />
          </div>
        )}
      </section>

      <aside style={{ display: 'flex', flexDirection: 'column', minWidth: 0, background: 'var(--panel)' }}>
        <div
          style={{
            display: 'flex',
            gap: 8,
            padding: 12,
            borderBottom: '1px solid var(--line)',
            alignItems: 'center',
          }}
        >
          <button onClick={onRun} disabled={running || !loaded}>
            Run
          </button>
          <button className="primary" onClick={onSubmit} disabled={!loaded}>
            Submit
          </button>
          <span className="mono" style={{ fontSize: 11, color: 'var(--faint)', marginLeft: 'auto' }}>
            Run = visible tests · Submit = the gate
          </span>
        </div>

        <div style={{ flex: 1, overflow: 'auto', padding: 14, display: 'flex', flexDirection: 'column', gap: 14 }}>
          {error && <pre className="out tone-bad">{error}</pre>}

          {submitInfo && (
            <div className={`mono ${submitInfo.bench_will_queue ? 'tone-good' : 'tone-warn'}`} style={{ fontSize: 12.5 }}>
              {submitInfo.bench_will_queue
                ? 'accepted — will queue for the benchmark once correctness passes'
                : `accepted for correctness · benchmark: ${submitInfo.bench_reason}`}
            </div>
          )}

          {submission ? <SubmissionPanel sub={submission} /> : <RunPanel result={runResult} running={running} />}
        </div>
      </aside>
    </div>
  );
}

function FileTab({
  active,
  muted,
  onClick,
  children,
}: {
  active: boolean;
  muted?: boolean;
  onClick: () => void;
  children: React.ReactNode;
}) {
  return (
    <button
      onClick={onClick}
      className="mono"
      style={{
        border: 'none',
        background: active ? 'var(--panel-2)' : 'transparent',
        color: active ? 'var(--text)' : muted ? 'var(--faint)' : 'var(--muted)',
        padding: '5px 10px',
        borderRadius: 4,
        fontSize: 12,
      }}
    >
      {children}
    </button>
  );
}

function saveLabel(s: 'idle' | 'saving' | 'saved' | 'error') {
  switch (s) {
    case 'saving':
      return 'saving…';
    case 'saved':
      return 'saved';
    case 'error':
      return 'save failed';
    default:
      return '';
  }
}
