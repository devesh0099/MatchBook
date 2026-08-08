'use client';

// Identity is a roster pick remembered in localStorage. There is no auth
// anywhere in this platform: 18 known people in a supervised room, reputations
// attached to every submission, honour system. See impl spec section 3.

import { useEffect, useState } from 'react';
import { api, type Participant } from './api';

const KEY = 'mebench.participant';

export interface Identity {
  id: number;
  handle: string;
}

export function readIdentity(): Identity | null {
  if (typeof window === 'undefined') return null;
  try {
    const raw = window.localStorage.getItem(KEY);
    return raw ? (JSON.parse(raw) as Identity) : null;
  } catch {
    return null;
  }
}

export function writeIdentity(id: Identity) {
  window.localStorage.setItem(KEY, JSON.stringify(id));
  window.dispatchEvent(new Event('mebench:identity'));
}

export function clearIdentity() {
  window.localStorage.removeItem(KEY);
  window.dispatchEvent(new Event('mebench:identity'));
}

export function useIdentity() {
  const [identity, setIdentity] = useState<Identity | null>(null);
  const [ready, setReady] = useState(false);

  useEffect(() => {
    const sync = () => setIdentity(readIdentity());
    sync();
    setReady(true);
    window.addEventListener('mebench:identity', sync);
    window.addEventListener('storage', sync);
    return () => {
      window.removeEventListener('mebench:identity', sync);
      window.removeEventListener('storage', sync);
    };
  }, []);

  return { identity, ready };
}

export function useRoster() {
  const [roster, setRoster] = useState<Participant[]>([]);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    api
      .participants()
      .then((r) => alive && setRoster(r))
      .catch((e) => alive && setError(String(e.message ?? e)));
    return () => {
      alive = false;
    };
  }, []);

  return { roster, error };
}
