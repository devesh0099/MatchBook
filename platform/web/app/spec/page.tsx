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
    <main className="main">
      <div className="page">
        <h1>Specification</h1>
        <p className="sub">
          This document is normative. Where the reference implementation and this document disagree,
          this document wins and the reference is a bug.
        </p>
        {/* Build-time content from our own repo; there is no untrusted input path here. */}
        <article className="prose" dangerouslySetInnerHTML={{ __html: SPEC_HTML }} />
      </div>
    </main>
  );
}
