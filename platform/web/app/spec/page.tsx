'use client';

// The specification, one click away at all times. Nobody should be re-typing
// structs from a PDF, and a rule that is hard to find becomes a rule that gets
// guessed at.

import { SPEC_MARKDOWN } from '@/lib/boilerplate.generated';

export default function SpecPage() {
  return (
    <main className="main">
      <div className="page">
        <h1>Specification</h1>
        <p className="sub">
          This document is normative. Where the reference implementation and this document disagree,
          this document wins and the reference is a bug.
        </p>
        <div className="card">
          <pre
            className="out"
            style={{ whiteSpace: 'pre-wrap', border: 'none', background: 'transparent', padding: 0 }}
          >
            {SPEC_MARKDOWN}
          </pre>
        </div>
      </div>
    </main>
  );
}
