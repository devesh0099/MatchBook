'use client';

// The specification, one click away at all times. Nobody should be re-typing
// structs from a PDF, and a rule that is hard to find becomes a rule that gets
// guessed at.
//
// The markdown is rendered to HTML at BUILD time by scripts/sync-assets.mjs
// from the same spec/SPEC.md that ships in the boilerplate zip — so the page and
// the zip cannot disagree about what the rules are, and no markdown parser is
// shipped to the browser to re-parse a file we already have.

import { SPEC_HTML } from '@/lib/boilerplate.generated';

export default function SpecPage() {
  return (
    <div className="page scroll-y" style={{ maxWidth: 900 }}>
      <div style={{ paddingTop: 34 }}>
        <div style={{ fontWeight: 800, fontSize: 38, letterSpacing: '-0.02em', lineHeight: 1 }}>
          The contract
        </div>
        <div style={{ height: 2, background: 'var(--app-rule)', margin: '18px 0 26px' }} />
        <p style={{ fontSize: 15, lineHeight: 1.65, maxWidth: '68ch' }}>
          This document is normative. Where the reference implementation and this document disagree,
          this document wins and the reference is a bug. Everything in the four header tabs is
          frozen and compiled as-is on the server, which is why your copy of them is read-only.
        </p>
        <div
          style={{
            borderLeft: '3px solid var(--color-accent)',
            padding: '4px 0 4px 16px',
            margin: '24px 0 32px',
            fontSize: 15,
            lineHeight: 1.6,
            maxWidth: '66ch',
          }}
        >
          Ranking is p50 latency per event, taken as the median of nine per-run medians. Correctness
          is binary and comes first: an engine that is wrong is not ranked at all, however fast it
          is.
        </div>

        {/* Build-time content from our own repo; there is no untrusted input path here. */}
        <article className="prose" dangerouslySetInnerHTML={{ __html: SPEC_HTML }} />
      </div>
    </div>
  );
}
