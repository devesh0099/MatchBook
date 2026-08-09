'use client';

// Light and dark are two designed themes, not one inverted. The choice lives on
// <html data-theme>, so every token switches at the root and nothing has to
// thread a theme prop.

import { useCallback, useEffect, useState } from 'react';

export type Theme = 'light' | 'dark';
const KEY = 'mebench.theme';

/// Applied by an inline script before first paint (see app/layout.tsx), so a
/// dark-theme user never sees a light flash. Kept here so the two copies of the
/// rule sit next to each other rather than drifting.
export function resolveTheme(stored: string | null, prefersDark: boolean): Theme {
  if (stored === 'light' || stored === 'dark') return stored;
  return prefersDark ? 'dark' : 'light';
}

export function useTheme() {
  const [theme, setThemeState] = useState<Theme>('light');

  useEffect(() => {
    const attr = document.documentElement.getAttribute('data-theme');
    setThemeState(attr === 'dark' ? 'dark' : 'light');
  }, []);

  const setTheme = useCallback((next: Theme) => {
    document.documentElement.setAttribute('data-theme', next);
    try {
      window.localStorage.setItem(KEY, next);
    } catch {
      /* a private window that refuses storage still gets the theme */
    }
    setThemeState(next);
  }, []);

  return { theme, setTheme };
}
