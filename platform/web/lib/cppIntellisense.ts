// Static IntelliSense for the frozen mebench API.
//
// No LSP and no clangd, deliberately: diagnostics stay with the compiler,
// whose errors are shown verbatim after Run (impl spec section 3). What this
// adds is completion and hover for the API surface a participant cannot
// change — the structs, enums, out:: helpers and interface methods parsed out
// of the frozen headers at build time by scripts/sync-assets.mjs. Because the
// data is generated from the real headers, it cannot drift from the contract.
//
// Anything about the participant's OWN code is covered only by Monaco's
// word-based suggestions, which is the honest limit of a static provider.

import type { Monaco } from '@monaco-editor/react';
import type { editor, Position } from 'monaco-editor';
import { API_SYMBOLS, type ApiSymbol } from './boilerplate.generated';

let registered = false;

// "inline OutEvent ack(uint64_t in_seq, OrderRef taker, ...)" -> snippet
// "ack(${1:in_seq}, ${2:taker}, ...)" so accepting a completion drops the
// caret through the arguments in order.
function callSnippet(s: ApiSymbol): string {
  const paren = s.signature.indexOf('(');
  if (paren < 0) return s.name;
  const params = s.signature
    .slice(paren + 1, s.signature.lastIndexOf(')'))
    .split(',')
    .map((p) => p.trim())
    .filter(Boolean)
    .map((p) => p.replace(/=[^,]+$/, '').trim().split(/[\s&*]+/).pop() ?? '');
  if (params.length === 0) return `${s.name}()`;
  return `${s.name}(${params.map((p, i) => `\${${i + 1}:${p}}`).join(', ')})`;
}

function docMarkdown(s: ApiSymbol) {
  return {
    value:
      '```cpp\n' + s.signature + '\n```\n' + (s.doc ? '\n' + s.doc + '\n' : '') + `\n*${s.file}*`,
  };
}

export function registerMebenchIntellisense(monaco: Monaco) {
  if (registered) return;
  registered = true;

  const kindMap = (s: ApiSymbol) => {
    const K = monaco.languages.CompletionItemKind;
    switch (s.kind) {
      case 'enum': return K.Enum;
      case 'enumMember': return K.EnumMember;
      case 'struct': return K.Struct;
      case 'field': return K.Field;
      case 'alias': return K.TypeParameter;
      case 'method': return K.Method;
      default: return K.Function;
    }
  };

  monaco.languages.registerCompletionItemProvider('cpp', {
    triggerCharacters: [':', '.', '>'],
    provideCompletionItems(model: editor.ITextModel, position: Position) {
      const line = model.getValueInRange({
        startLineNumber: position.lineNumber,
        startColumn: 1,
        endLineNumber: position.lineNumber,
        endColumn: position.column,
      });
      const word = model.getWordUntilPosition(position);
      const range = {
        startLineNumber: position.lineNumber,
        endLineNumber: position.lineNumber,
        startColumn: word.startColumn,
        endColumn: word.endColumn,
      };
      const beforeWord = line.slice(0, word.startColumn - 1);

      let pool: ApiSymbol[];
      let qualifyMembers = false;

      const scope = beforeWord.match(/(\w+)::$/);
      if (scope) {
        // `out::`, `TIF::`, `Side::`, ... — members of that scope only.
        pool = API_SYMBOLS.filter((s) => s.parent === scope[1]);
      } else if (/(\.|->)$/.test(beforeWord)) {
        // Member access. A static provider does not know the object's type, so
        // offer every struct field and interface method, labelled by owner.
        pool = API_SYMBOLS.filter((s) => s.kind === 'field' || s.kind === 'method');
      } else {
        // Top level: types, qualified enum members, out:: helpers, free fns.
        pool = API_SYMBOLS.filter((s) => s.kind !== 'field' && s.kind !== 'method');
        qualifyMembers = true;
      }

      return {
        suggestions: pool.map((s) => {
          const label = qualifyMembers ? s.qualified : s.name;
          const isCall = s.kind === 'function' || s.kind === 'method';
          const insert = isCall
            ? (qualifyMembers && s.parent ? `${s.parent}::` : '') + callSnippet(s)
            : label;
          return {
            label,
            kind: kindMap(s),
            detail: s.signature,
            documentation: docMarkdown(s),
            insertText: insert,
            insertTextRules: isCall
              ? monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet
              : undefined,
            range,
            // The frozen API above Monaco's word-based echo of the buffer.
            sortText: '0' + label,
          };
        }),
      };
    },
  });

  monaco.languages.registerHoverProvider('cpp', {
    provideHover(model: editor.ITextModel, position: Position) {
      const word = model.getWordAtPosition(position);
      if (!word) return null;
      const line = model.getLineContent(position.lineNumber);
      const qualifier = line.slice(0, word.startColumn - 1).match(/(\w+)::$/)?.[1];

      const matches = API_SYMBOLS.filter((s) => s.name === word.word);
      if (matches.length === 0) return null;
      // Prefer the symbol whose parent matches an explicit `X::` qualifier;
      // otherwise show every homonym (e.g. `price` on Order and OutEvent).
      const chosen = qualifier ? matches.filter((s) => s.parent === qualifier) : matches;
      const shown = (chosen.length ? chosen : matches).slice(0, 4);
      return {
        range: new monaco.Range(
          position.lineNumber,
          word.startColumn,
          position.lineNumber,
          word.endColumn,
        ),
        contents: shown.map(docMarkdown),
      };
    },
  });
}
