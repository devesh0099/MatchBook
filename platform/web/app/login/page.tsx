'use client';

// The login gate. Credentials come from the printed slip issued at check-in
// (ops/issue-credentials.sh); the API sets an httpOnly session cookie and
// everything after this page is implicitly "you". Nothing is stored
// client-side — this form is the only place identity is ever typed.

import { useRouter } from 'next/navigation';
import { useState } from 'react';
import { login } from '@/lib/identity';

export default function LoginPage() {
  const router = useRouter();
  const [handle, setHandle] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  async function onSubmit(e: React.FormEvent) {
    e.preventDefault();
    setError(null);
    setBusy(true);
    try {
      await login(handle.trim(), password);
      router.push('/editor');
    } catch (err) {
      setError(String((err as Error).message ?? err));
      setBusy(false);
    }
  }

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

  return (
    <div style={{ display: 'flex', justifyContent: 'center', paddingTop: '14vh' }}>
      <form onSubmit={onSubmit} style={{ width: 340, display: 'flex', flexDirection: 'column', gap: 14 }}>
        <div>
          <div style={{ fontWeight: 800, letterSpacing: '0.14em', fontSize: 14, textTransform: 'uppercase' }}>
            FlashMatch
          </div>
          <div style={{ fontSize: 12, color: 'var(--app-ink-3)', marginTop: 4 }}>
            Sign in with the handle and password from your printed slip.
          </div>
        </div>

        <label style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
          <span className="kicker">Handle</span>
          <input
            autoFocus
            value={handle}
            onChange={(e) => setHandle(e.target.value)}
            autoComplete="username"
            style={field}
          />
        </label>

        <label style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
          <span className="kicker">Password</span>
          <input
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            autoComplete="current-password"
            style={field}
          />
        </label>

        {error && (
          <div style={{ fontSize: 12, color: 'var(--color-bad, #c0392b)' }}>{error}</div>
        )}

        <button
          type="submit"
          disabled={busy || !handle.trim() || !password}
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
            opacity: busy || !handle.trim() || !password ? 0.6 : 1,
          }}
        >
          {busy ? 'Signing in…' : 'Sign in'}
        </button>

        <div style={{ fontSize: 11, color: 'var(--app-ink-3)' }}>
          Lost your slip? An operator can reissue your password — it signs you
          out everywhere and prints a new slip.
        </div>
      </form>
    </div>
  );
}
