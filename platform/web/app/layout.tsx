import type { Metadata } from 'next';
import './globals.css';
import { TopBar } from '@/components/TopBar';

export const metadata: Metadata = {
  title: 'Matching Engine Challenge',
  description: 'Implement order book matching in C++. Gated on correctness, ranked on latency.',
};

/// Runs before first paint so a dark-theme user never sees a light flash. It
/// duplicates resolveTheme() in lib/theme.ts on purpose: this has to be inline
/// and synchronous, and a bundle import would be neither.
const THEME_BOOT = `(function(){try{
  var s=localStorage.getItem('mebench.theme');
  var d=window.matchMedia('(prefers-color-scheme: dark)').matches;
  document.documentElement.setAttribute('data-theme',(s==='light'||s==='dark')?s:(d?'dark':'light'));
}catch(e){document.documentElement.setAttribute('data-theme','light');}})();`;

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en" suppressHydrationWarning>
      <head>
        {/* The font is same-origin, so it can be preloaded without a
            cross-origin connection on the critical path. */}
        <link
          rel="preload"
          href="/fonts/archivo-var.woff2"
          as="font"
          type="font/woff2"
          crossOrigin="anonymous"
        />
        <script dangerouslySetInnerHTML={{ __html: THEME_BOOT }} />
      </head>
      <body>
        {/* A fixed-height shell: the editor fills the viewport exactly and
            scrolls inside its own columns rather than growing the page. */}
        <div style={{ minHeight: '100vh', display: 'flex', flexDirection: 'column' }}>
          <TopBar />
          <div style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column' }}>
            {children}
          </div>
        </div>
      </body>
    </html>
  );
}
