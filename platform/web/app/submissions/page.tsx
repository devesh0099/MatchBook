'use client';

import { useEffect, useState } from 'react';
import { api, stateLabel, stateTone, type Submission } from '@/lib/api';
import { HandleGate } from '@/components/HandleGate';
import { SubmissionPanel } from '@/components/ResultsPanel';
import { HistoryChart, type HistoryPoint } from '@/components/Charts';
import { useIdentity } from '@/lib/identity';

export default function SubmissionsPage() {
  return (
    <main className="main">
      <HandleGate>
        <Inner />
      </HandleGate>
    </main>
  );
}

function Inner() {
  const { identity } = useIdentity();
  const [rows, setRows] = useState<Submission[]>([]);
  const [open, setOpen] = useState<Submission | null>(null);

  useEffect(() => {
    if (!identity) return;
    const load = () => api.mine(identity.id).then(setRows).catch(() => {});
    load();
    const t = setInterval(load, 4000);
    return () => clearInterval(t);
  }, [identity]);

  return (
    <div className="page">
      <h1>My submissions</h1>
      <p className="sub">
        Every Submit is recorded with the exact source that produced it, so &ldquo;what code gave
        this result&rdquo; is always answerable.
      </p>

      {(() => {
        // Oldest to newest, benchmarked submissions only — a submission with no
        // p50 never reached the bench lane and would be a gap, not a datapoint.
        const points: HistoryPoint[] = rows
          .filter((s) => s.p50_ns != null)
          .slice()
          .reverse()
          .map((s) => ({ id: s.id, p50: s.p50_ns as number, p95: s.p95_ns, p99: s.p99_ns }));
        return points.length > 1 ? (
          <div className="card" style={{ marginBottom: 22 }}>
            <HistoryChart points={points} />
          </div>
        ) : null;
      })()}

      {rows.length === 0 ? (
        <div className="card">
          <div className="empty">Nothing submitted yet.</div>
        </div>
      ) : (
        <div className="card" style={{ padding: 0 }}>
          <table>
            <thead>
              <tr>
                <th>#</th>
                <th>State</th>
                <th style={{ textAlign: 'right' }}>p50</th>
                <th>Source</th>
                <th>When</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((s) => (
                <tr
                  key={s.id}
                  onClick={() => setOpen(open?.id === s.id ? null : s)}
                  style={{ cursor: 'pointer' }}
                >
                  <td className="mono">{s.id}</td>
                  <td className={`mono tone-${stateTone(s.state)}`}>{stateLabel(s.state)}</td>
                  <td className="num">{s.p50_ns != null ? `${s.p50_ns.toFixed(1)} ns` : '—'}</td>
                  <td className="mono" style={{ color: 'var(--faint)' }}>
                    {s.source_hash.slice(0, 12)}
                  </td>
                  <td className="mono" style={{ color: 'var(--faint)' }}>
                    {s.created_at ? new Date(s.created_at).toLocaleTimeString() : '—'}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {open && (
        <section style={{ marginTop: 22 }}>
          <h2>Submission #{open.id}</h2>
          <div className="card">
            <SubmissionPanel sub={open} />
          </div>
        </section>
      )}
    </div>
  );
}
