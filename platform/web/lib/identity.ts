'use client';

// Identity is a SERVER session, nothing else. The browser proves who it is
// once — POST /login with the printed handle + password — and from then on
// the httpOnly session cookie (which this code cannot even read) is the only
// identity anything carries. localStorage holds nothing; the roster is not
// downloadable; the API derives the participant from the cookie on every
// request, so the client never says who it is.

import { useCallback, useEffect, useState } from 'react';
import { api } from './api';

export interface Identity {
  id: number;
  handle: string;
}

/// Fired after login/logout so every mounted useIdentity refreshes at once.
const EVENT = 'mebench:session';

export async function login(handle: string, password: string): Promise<Identity> {
  const r = await api.login(handle, password);
  const id = { id: r.participant_id, handle: r.handle };
  window.dispatchEvent(new Event(EVENT));
  return id;
}

export async function logout() {
  try {
    await api.logout();
  } finally {
    window.dispatchEvent(new Event(EVENT));
  }
}

export function useIdentity() {
  const [identity, setIdentity] = useState<Identity | null>(null);
  const [ready, setReady] = useState(false);

  const sync = useCallback(() => {
    api
      .session()
      .then((s) => {
        setIdentity({ id: s.participant_id, handle: s.handle });
        setReady(true);
      })
      .catch(() => {
        setIdentity(null);
        setReady(true);
      });
  }, []);

  useEffect(() => {
    sync();
    window.addEventListener(EVENT, sync);
    return () => window.removeEventListener(EVENT, sync);
  }, [sync]);

  return { identity, ready };
}
