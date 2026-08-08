'use client';

// First visit: pick a handle from the preloaded roster. That is the entire
// identity system — no accounts, no passwords, no reset flow. 18 known people
// in a supervised room.

import { useState } from 'react';
import { useIdentity, useRoster, writeIdentity } from '@/lib/identity';

export function HandleGate({ children }: { children: React.ReactNode }) {
  const { identity, ready } = useIdentity();
  const { roster, error } = useRoster();
  const [picked, setPicked] = useState<number | ''>('');

  if (!ready) return null;
  if (identity) return <>{children}</>;

  return (
    <div className="page" style={{ maxWidth: 460 }}>
      <h1>Who are you?</h1>
      <p className="sub">
        Pick your handle from the roster. It is remembered in this browser. There are no
        passwords — the room is supervised and your name is on every submission.
      </p>

      {error && <p className="tone-bad mono">could not load the roster: {error}</p>}

      <div className="card" style={{ display: 'flex', gap: 10 }}>
        <select
          value={picked}
          onChange={(e) => setPicked(e.target.value ? Number(e.target.value) : '')}
          style={{ flex: 1 }}
        >
          <option value="">Select your handle…</option>
          {roster.map((p) => (
            <option key={p.id} value={p.id}>
              {p.handle}
            </option>
          ))}
        </select>
        <button
          className="primary"
          disabled={picked === ''}
          onClick={() => {
            const p = roster.find((r) => r.id === picked);
            if (p) writeIdentity({ id: p.id, handle: p.handle });
          }}
        >
          Start
        </button>
      </div>
    </div>
  );
}
