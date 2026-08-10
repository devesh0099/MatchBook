'use client';

// Light and dark are two designed themes, not one inverted. The choice lives on
// <html data-theme>, so every token switches at the root and nothing has to
// thread a theme prop.
//
// Almost nothing. Monaco renders into its own DOM with its own theme system and
// cannot see our custom properties, so the editor has to be TOLD, which means
// some component needs the current theme as React state.
//
// That is why this hook broadcasts. It used to hold plain per-component state:
// the top bar's toggle set the DOM attribute — so every CSS-variable-driven
// pixel changed — and updated its OWN copy, while the editor's separate copy of
// the same hook never heard about it. Monaco stayed on the old theme until the
// page remounted, so switching to /submissions and back appeared to "fix" it,
// and then the next toggle was stale in the other direction. Subscribing every
// instance to one event keeps them in step.
//
// Same mechanism as lib/identity.ts, deliberately: one pattern for
// cross-component state in this app rather than two.

import { useCallback, useEffect, useState } from 'react';

export type Theme = 'light' | 'dark';
const KEY = 'mebench.theme';
const EVENT = 'mebench:theme';

/// Applied by an inline script before first paint (see app/layout.tsx), so a
/// dark-theme user never sees a light flash. Kept here so the two copies of the
/// rule sit next to each other rather than drifting.
export function resolveTheme(stored: string | null, prefersDark: boolean): Theme {
  if (stored === 'light' || stored === 'dark') return stored;
  return prefersDark ? 'dark' : 'light';
}

function current(): Theme {
  return document.documentElement.getAttribute('data-theme') === 'dark' ? 'dark' : 'light';
}

export function useTheme() {
  const [theme, setThemeState] = useState<Theme>('light');

  useEffect(() => {
    const sync = () => setThemeState(current());
    sync();
    window.addEventListener(EVENT, sync);
    // Another tab switching theme should carry over here too.
    window.addEventListener('storage', sync);
    return () => {
      window.removeEventListener(EVENT, sync);
      window.removeEventListener('storage', sync);
    };
  }, []);

  const setTheme = useCallback((next: Theme) => {
    document.documentElement.setAttribute('data-theme', next);
    try {
      window.localStorage.setItem(KEY, next);
    } catch {
      /* a private window that refuses storage still gets the theme */
    }
    window.dispatchEvent(new Event(EVENT));
  }, []);

  return { theme, setTheme };
}
