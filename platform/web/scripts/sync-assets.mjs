// sync-assets.mjs — runs before every build and before `next dev`.
//
// Two jobs, both about removing a class of mistake rather than adding a feature.
//
// 1. The editor's starting buffer and the read-only header tabs are generated
//    from the REAL files in engine/. The spec requires the editor buffer to be
//    identical to src/engine.cpp in the boilerplate zip; copying it by hand is
//    how those two silently diverge, and a participant who pastes between them
//    would be the one to find out.
//
// 2. Monaco is copied into public/ and loaded from there. @monaco-editor/react
//    fetches it from a CDN by default, which would mean the editor — the SOLE
//    submission path — stops working if the room's network does. The event runs
//    on three nodes we control; nothing user-facing should depend on jsdelivr.

import { cp, mkdir, readFile, writeFile } from 'node:fs/promises';
import { marked } from 'marked';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const web = resolve(here, '..');
const engine = resolve(web, '../../engine');
const spec = resolve(web, '../../spec');

// wire.h is deliberately absent. It is the on-disk stream format, decoded by
// the harness OUTSIDE the timed region — an engine never sees a WireEvent. The
// three enums an engine does need moved to order.h, so these three tabs are
// self-sufficient and there is no fourth tab of things that do not apply.
const HEADERS = ['order.h', 'out.h', 'engine.h'];

function ts(value) {
  return JSON.stringify(value);
}

// Extract the frozen API surface for the editor's static IntelliSense. The
// headers are FROZEN and small, so targeted line matching is reliable here in
// a way it never would be on arbitrary C++ — and generating from the real
// files means the completions cannot drift from the contract, same reason the
// editor buffer is generated.
function parseApi(headers) {
  const symbols = [];
  const push = (s) => symbols.push({ doc: '', parent: '', ...s });

  for (const [file, src] of Object.entries(headers)) {
    const lines = src.split('\n');

    const docAbove = (i) => {
      const doc = [];
      for (let j = i - 1; j >= 0; --j) {
        const t = lines[j].trim();
        if (t.startsWith('//')) doc.unshift(t.replace(/^\/\/ ?/, ''));
        else break;
      }
      return doc.join('\n');
    };

    let currentClass = '';
    let inOutNamespace = false;

    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      let m;

      if (/^namespace out \{/.test(line)) inOutNamespace = true;
      if (/^\}\s*\/\/ namespace out/.test(line)) inOutNamespace = false;
      if ((m = line.match(/^class (\w+)/))) currentClass = m[1];

      // enum class X : uint8_t { A = 0, B = 1 };   (always one line here)
      if ((m = line.match(/^enum class (\w+)\s*:\s*\w+\s*\{([^}]*)\};/))) {
        const [, name, body] = m;
        push({ kind: 'enum', name, qualified: name, signature: line.trim(), doc: docAbove(i), file });
        for (const mm of body.matchAll(/(\w+)\s*=\s*(\d+)/g)) {
          push({
            kind: 'enumMember',
            name: mm[1],
            qualified: `${name}::${mm[1]}`,
            signature: `${name}::${mm[1]} = ${mm[2]}`,
            parent: name,
            file,
          });
        }
        continue;
      }

      // struct X {  (fields until the closing brace; skips methods/operators)
      if ((m = line.match(/^struct (?:__attribute__\(\(packed\)\)\s+)?(\w+) \{/))) {
        const name = m[1];
        push({ kind: 'struct', name, qualified: name, signature: `struct ${name}`, doc: docAbove(i), file });
        for (let j = i + 1; j < lines.length && !/^\};/.test(lines[j]); j++) {
          const f = lines[j].match(/^\s+([A-Za-z_][\w:<>, ]*?)\s+([\w, [\]]+);(?:\s*\/\/\s*(.*))?$/);
          if (!f) continue;
          const [, type, namesRaw, comment] = f;
          for (const fieldName of namesRaw.split(',').map((s) => s.trim().replace(/\[\d+\]$/, ''))) {
            if (!/^[A-Za-z]\w*$/.test(fieldName)) continue;
            push({
              kind: 'field',
              name: fieldName,
              qualified: `${name}::${fieldName}`,
              signature: `${type} ${name}::${fieldName}`,
              doc: comment ?? '',
              parent: name,
              file,
            });
          }
        }
        continue;
      }

      // using LevelVec = ...;
      if ((m = line.match(/^using (\w+) = .+;/))) {
        push({ kind: 'alias', name: m[1], qualified: m[1], signature: line.trim(), doc: docAbove(i), file });
        continue;
      }

      // inline helpers: the out:: constructors and build_book. Signatures may
      // wrap; accumulate lines until the parameter list closes.
      if ((m = line.match(/^inline (\w+) (\w+)\(/))) {
        let sig = line.trim();
        let j = i;
        while (!sig.includes(')') && j + 1 < lines.length) sig += ' ' + lines[++j].trim();
        sig = sig.replace(/\s*\{.*$/, '').replace(/\s+/g, ' ');
        const name = m[2];
        push({
          kind: 'function',
          name,
          qualified: inOutNamespace ? `out::${name}` : name,
          signature: sig,
          doc: docAbove(i),
          parent: inOutNamespace ? 'out' : '',
          file,
        });
        continue;
      }

      // virtual interface methods (on_new, emit, snapshot, bid_levels, ...)
      if ((m = line.match(/^\s*virtual\s+[\w:]+\s+(\w+)\(.*\).*;/))) {
        push({
          kind: 'method',
          name: m[1],
          qualified: `${currentClass}::${m[1]}`,
          signature: line.trim().replace(/^virtual\s+/, '').replace(/\s*=\s*0;$/, ';'),
          doc: docAbove(i),
          parent: currentClass,
          file,
        });
        continue;
      }

      // extern "C" mebench::IMatchingEngine* create_engine();
      if ((m = line.match(/^extern "C" ([\w:*]+ )?(\w+)\(\);/))) {
        push({
          kind: 'function',
          name: m[2],
          qualified: m[2],
          signature: line.trim(),
          doc: docAbove(i),
          file,
        });
      }
    }
  }
  return symbols;
}

async function generateBoilerplate() {
  const skeleton = await readFile(join(engine, 'boilerplate/src/engine.cpp'), 'utf8');

  const headers = {};
  for (const name of HEADERS) {
    headers[name] = await readFile(join(engine, 'include/mebench', name), 'utf8');
  }

  let specText = '';
  try {
    specText = await readFile(join(spec, 'SPEC.md'), 'utf8');
  } catch {
    specText = '# SPEC.md not found at build time';
  }

  // Rendered here rather than in the browser: the spec is a build-time asset
  // generated from our own repo, so shipping a markdown parser to every
  // participant to re-parse a file we already have would be work for nothing.
  let specHtml = marked.parse(specText, { async: false, gfm: true });

  // Heading ids and a table of contents, derived here rather than in the
  // browser. The spec is one long document and the page needs to be navigable
  // without scrolling through it; doing this at build time keeps the runtime a
  // plain innerHTML with no parsing.
  const toc = [];
  const seen = new Map();
  specHtml = specHtml.replace(/<h([23])>(.*?)<\/h\1>/g, (_m, level, inner) => {
    const text = inner.replace(/<[^>]*>/g, '').trim();
    let id = text
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/^-+|-+$/g, '');
    // Distinct ids even if two headings ever collide, so anchors stay stable.
    const n = (seen.get(id) ?? 0) + 1;
    seen.set(id, n);
    if (n > 1) id = `${id}-${n}`;
    toc.push({ level: Number(level), id, text });
    return `<h${level} id="${id}">${inner}</h${level}>`;
  });

  const api = parseApi(headers);

  const out = `// GENERATED by scripts/sync-assets.mjs — do not edit.
//
// Regenerated on every build from engine/boilerplate/src/engine.cpp and the
// frozen headers, so the editor buffer and the zip cannot drift apart.

export const STARTING_BUFFER = ${ts(skeleton)};

export const HEADERS: Record<string, string> = ${ts(headers)};

export const HEADER_NAMES = ${ts(HEADERS)} as const;

// The frozen API surface, parsed from the headers above, for the editor's
// static completion and hover providers (lib/cppIntellisense.ts).
export type ApiSymbol = {
  kind: 'enum' | 'enumMember' | 'struct' | 'field' | 'alias' | 'function' | 'method';
  name: string;
  qualified: string;
  signature: string;
  doc: string;
  parent: string;
  file: string;
};

export const API_SYMBOLS: ApiSymbol[] = ${ts(api)};

export const SPEC_MARKDOWN = ${ts(specText)};

export const SPEC_HTML = ${ts(specHtml)};

export const SPEC_TOC: { level: number; id: string; text: string }[] = ${JSON.stringify(toc)};
`;
  await mkdir(join(web, 'lib'), { recursive: true });
  await writeFile(join(web, 'lib/boilerplate.generated.ts'), out);
  console.log(
    `sync-assets: engine.cpp (${skeleton.length}B), ${HEADERS.length} headers, spec (${specText.length}B, ${toc.length} headings)`,
  );
}

async function vendorMonaco() {
  const from = join(web, 'node_modules/monaco-editor/min/vs');
  const to = join(web, 'public/monaco/vs');
  await mkdir(dirname(to), { recursive: true });
  await cp(from, to, { recursive: true });
  console.log('sync-assets: monaco vendored to public/monaco/vs (no CDN at runtime)');
}

await generateBoilerplate();
await vendorMonaco();
