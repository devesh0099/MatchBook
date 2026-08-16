'use client';

// The 46px bar across every view: brand, the four routes, the event clock, the
// signed-in handle, and the theme switch.
//
// Identity is a server session (lib/identity.ts): the bar shows who the
// cookie says you are and offers sign out; signing in lives at /login.

import Link from 'next/link';
import { usePathname, useRouter } from 'next/navigation';
import { useEffect, useState } from 'react';
import { logout, useIdentity } from '@/lib/identity';
import { useTheme } from '@/lib/theme';

const ROUTES = [
  { href: '/editor', label: '/editor' },
  { href: '/submissions', label: '/submissions' },
  { href: '/leaderboard', label: '/leaderboard' },
  { href: '/spec', label: '/spec' },
];

/// Wall clock, not an event countdown. The event has no fixed start time known
/// to the frontend, and inventing one would be a number that quietly lies.
function useClock() {
  const [now, setNow] = useState<string>('');
  useEffect(() => {
    const tick = () =>
      setNow(
        new Date().toLocaleTimeString('en-GB', {
          hour: '2-digit',
          minute: '2-digit',
          second: '2-digit',
          hour12: false,
        }),
      );
    tick();
    const t = setInterval(tick, 1000);
    return () => clearInterval(t);
  }, []);
  return now;
}

const cellBase: React.CSSProperties = {
  appearance: 'none',
  border: 0,
  borderRight: '1px solid var(--app-line)',
  color: 'var(--app-ink)',
  font: 'inherit',
  fontWeight: 700,
  fontSize: 11,
  letterSpacing: '0.12em',
  textTransform: 'uppercase',
  padding: '0 18px',
  cursor: 'pointer',
  textDecoration: 'none',
  display: 'flex',
  alignItems: 'center',
};

export function TopBar() {
  const pathname = usePathname();
  const router = useRouter();
  const { identity } = useIdentity();
  const { theme, setTheme } = useTheme();
  const clock = useClock();

  return (
    <div
      style={{
        display: 'flex',
        alignItems: 'stretch',
        height: 46,
        flex: 'none',
        borderBottom: '2px solid var(--app-rule)',
        background: 'var(--app-bg)',
        position: 'sticky',
        top: 0,
        zIndex: 40,
      }}
    >
      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          gap: 10,
          padding: '0 16px',
          borderRight: '1px solid var(--app-line)',
        }}
      >
        <div style={{ width: 12, height: 12, background: 'var(--color-accent)' }} />
        <div style={{ fontWeight: 800, letterSpacing: '0.14em', fontSize: 12, textTransform: 'uppercase' }}>
          FlashMatch
        </div>
        <div
          style={{
            fontSize: 10,
            letterSpacing: '0.12em',
            color: 'var(--app-ink-3)',
            textTransform: 'uppercase',
          }}
        >
          p95 · ns/event
        </div>
      </div>

      <nav style={{ display: 'flex', alignItems: 'stretch' }}>
        {ROUTES.map((r) => {
          const active = pathname.startsWith(r.href);
          return (
            <Link
              key={r.href}
              href={r.href}
              aria-current={active ? 'page' : undefined}
              style={{ ...cellBase, background: active ? 'var(--app-panel)' : 'transparent' }}
            >
              {r.label}
            </Link>
          );
        })}
      </nav>

      <div style={{ flex: 1 }} />

      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          gap: 14,
          padding: '0 14px',
          borderLeft: '1px solid var(--app-line)',
        }}
      >
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <span className="kicker" style={{ fontWeight: 400 }}>
            Clock
          </span>
          <span className="mono" style={{ fontSize: 13, fontWeight: 700 }} suppressHydrationWarning>
            {clock || '--:--:--'}
          </span>
        </div>

        <div style={{ width: 1, height: 20, background: 'var(--app-line)' }} />

        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          {identity ? (
            <>
              <span className="kicker" style={{ fontWeight: 400 }}>
                Signed in
              </span>
              <span className="mono" style={{ fontSize: 13, fontWeight: 700 }}>
                {identity.handle}
              </span>
              <button
                onClick={async () => {
                  await logout();
                  router.push('/login');
                }}
                style={{
                  appearance: 'none',
                  border: '1px solid var(--app-rule)',
                  background: 'transparent',
                  color: 'var(--app-ink-2)',
                  font: 'inherit',
                  fontWeight: 700,
                  fontSize: 10,
                  letterSpacing: '0.1em',
                  textTransform: 'uppercase',
                  padding: '4px 9px',
                  cursor: 'pointer',
                }}
              >
                Sign out
              </button>
            </>
          ) : (
            <Link
              href="/login"
              style={{
                fontWeight: 700,
                fontSize: 11,
                letterSpacing: '0.12em',
                textTransform: 'uppercase',
                color: 'var(--app-ink)',
                border: '1px solid var(--app-rule)',
                padding: '4px 10px',
                textDecoration: 'none',
              }}
            >
              Sign in
            </Link>
          )}
        </div>

        <div style={{ display: 'flex', border: '1px solid var(--app-rule)' }}>
          {(['light', 'dark'] as const).map((t, i) => (
            <button
              key={t}
              onClick={() => setTheme(t)}
              aria-pressed={theme === t}
              style={{
                appearance: 'none',
                border: 0,
                borderLeft: i ? '1px solid var(--app-rule)' : 0,
                background: theme === t ? 'var(--app-ink)' : 'transparent',
                color: theme === t ? 'var(--app-bg)' : 'var(--app-ink-2)',
                font: 'inherit',
                fontSize: 10,
                fontWeight: 700,
                letterSpacing: '0.1em',
                textTransform: 'uppercase',
                padding: '5px 9px',
                cursor: 'pointer',
              }}
            >
              {t}
            </button>
          ))}
        </div>
      </div>
    </div>
  );
}
