'use client';

// The specification, one click away at all times. Nobody should be re-typing
// structs from a PDF, and a rule that is hard to find becomes a rule that gets
// guessed at.
//
// The markdown is rendered to HTML at BUILD time by scripts/sync-assets.mjs
// from the same spec/SPEC.md that ships in the boilerplate zip — so the page and
// the zip cannot disagree about what the rules are, and no markdown parser is
// shipped to the browser to re-parse a file we already have.
//
// The document is one continuous read, which is right for a normative text and
// wrong for finding one rule under time pressure. Hence the contents rail: it
// is sticky, it tracks the section you are actually looking at, and it turns
// "scroll until you see it" into one click. Everything else here is layout.

import { useEffect, useState } from 'react';
import { SPEC_HTML, SPEC_TOC } from '@/lib/boilerplate.generated';

export default function SpecPage() {
  const [active, setActive] = useState<string>(SPEC_TOC[0]?.id ?? '');

  useEffect(() => {
    const headings = SPEC_TOC.map((t) => document.getElementById(t.id)).filter(
      (el): el is HTMLElement => el !== null,
    );
    if (headings.length === 0) return;

    // Track the last heading whose top has passed the reading line, rather than
    // whichever is intersecting — with short sections several are on screen at
    // once and "topmost intersecting" flickers between them while scrolling.
    const onScroll = () => {
      const line = 120;
      let current = headings[0].id;
      for (const h of headings) {
        if (h.getBoundingClientRect().top <= line) current = h.id;
        else break;
      }
      setActive(current);
    };
    onScroll();
    window.addEventListener('scroll', onScroll, { passive: true });
    return () => window.removeEventListener('scroll', onScroll);
  }, []);

  return (
    // No scroll-y on this page: it sets overflow-y:auto, which makes this
    // element the sticky scroll CONTEXT for the contents rail while the WINDOW
    // is what actually scrolls — so the rail never sticks and slides away after
    // the first screen. The element is not height-constrained, so it never
    // scrolled itself and the class was doing nothing else.
    <div className="page spec-page">
      <div style={{ paddingTop: 34 }}>
        <div style={{ fontWeight: 800, fontSize: 38, letterSpacing: '-0.02em', lineHeight: 1 }}>
          The contract
        </div>
        <div style={{ height: 2, background: 'var(--app-rule)', margin: '18px 0 26px' }} />

        <div className="spec-layout">
          {/* Collapses away under 1100px, where a rail would cost more width than
              it saves. The document is still fully navigable by scrolling. */}
          <nav className="spec-toc" aria-label="Contents">
            <div className="kicker" style={{ color: 'var(--app-ink-3)', marginBottom: 10 }}>
              Contents
            </div>
            {SPEC_TOC.map((t) => (
              <a
                key={t.id}
                href={`#${t.id}`}
                className={`spec-toc-link${active === t.id ? ' is-active' : ''}`}
                style={{ paddingLeft: t.level === 3 ? 14 : 0 }}
              >
                {t.text}
              </a>
            ))}
          </nav>

          <div style={{ minWidth: 0 }}>
            <div
              style={{
                borderLeft: '3px solid var(--color-accent)',
                padding: '4px 0 4px 16px',
                margin: '20px 0 8px',
                fontSize: 15,
                lineHeight: 1.6,
                maxWidth: '66ch',
              }}
            >
              Ranking is p95 latency per event, taken as the median of the per-run p95s
              (p50 then p99 break ties). Correctness is binary and comes first: an engine
              that is wrong is not ranked at all, however fast it is.
            </div>

            {/* Build-time content from our own repo; there is no untrusted input path here. */}
            <article className="prose spec-body" dangerouslySetInnerHTML={{ __html: SPEC_HTML }} />
          </div>
        </div>
      </div>
    </div>
  );
}
