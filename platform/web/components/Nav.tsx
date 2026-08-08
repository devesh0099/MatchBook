'use client';

import Link from 'next/link';
import { usePathname } from 'next/navigation';
import { clearIdentity, useIdentity } from '@/lib/identity';

const TABS = [
  { href: '/editor', label: 'Editor' },
  { href: '/submissions', label: 'My submissions' },
  { href: '/leaderboard', label: 'Leaderboard' },
  { href: '/spec', label: 'Spec' },
];

export function Nav() {
  const pathname = usePathname();
  const { identity } = useIdentity();

  return (
    <nav className="nav">
      <div className="brand">
        mebench<span>/</span>
      </div>
      {TABS.map((t) => (
        <Link
          key={t.href}
          href={t.href}
          className={`tab${pathname?.startsWith(t.href) ? ' active' : ''}`}
        >
          {t.label}
        </Link>
      ))}
      <div className="spacer" />
      {identity && (
        <div className="who">
          <b>{identity.handle}</b>
          <button
            style={{ padding: '3px 8px', fontSize: 11 }}
            onClick={() => clearIdentity()}
            title="Pick a different handle"
          >
            switch
          </button>
        </div>
      )}
    </nav>
  );
}
