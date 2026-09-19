import { Fragment } from 'react';
import type { ReactNode } from 'react';

/**
 * A deliberately small markdown renderer.
 *
 * The analysis endpoint returns its three fields separately now, so the common
 * path never comes through here -- this exists for the degraded one, where the
 * model ignored the JSON instruction and `analysis` is whatever text it felt
 * like emitting. That text still must not reach the user as literal `##` and
 * triple backticks, so the handful of constructs a model actually produces are
 * handled and nothing else: ATX headings, fenced code, unordered lists,
 * paragraphs, and inline bold/code.
 *
 * Pulling in a full markdown library for that would mean shipping a parser and
 * a sanitiser to render text we already control the shape of.
 */

const INLINE = /(\*\*[^*]+\*\*|`[^`]+`)/g;

/** Bold and inline code. Everything else is left as text, which is the right
 *  default: an unrecognised construct should read as itself, not vanish. */
function renderInline(text: string): ReactNode {
  return text.split(INLINE).map((part, i) => {
    if (part.startsWith('**') && part.endsWith('**') && part.length > 4)
      return <strong key={i}>{part.slice(2, -2)}</strong>;
    if (part.startsWith('`') && part.endsWith('`') && part.length > 2)
      return <code key={i}>{part.slice(1, -1)}</code>;
    return <Fragment key={i}>{part}</Fragment>;
  });
}

type Block =
  | { kind: 'code'; lang: string; body: string }
  | { kind: 'heading'; level: number; body: string }
  | { kind: 'list'; items: string[] }
  | { kind: 'para'; body: string };

function parse(src: string): Block[] {
  const lines  = src.replace(/\r\n/g, '\n').split('\n');
  const blocks: Block[] = [];
  let para: string[] = [];
  let list: string[] = [];

  // Paragraphs and lists accumulate across lines, so every other block kind has
  // to close whichever one is open before it can be pushed. Doing that in one
  // place keeps the ordering of blocks correct no matter what follows what.
  const flush = () => {
    if (para.length) { blocks.push({ kind: 'para', body: para.join('\n') }); para = []; }
    if (list.length) { blocks.push({ kind: 'list', items: list }); list = []; }
  };

  for (let i = 0; i < lines.length; ++i) {
    const line = lines[i];

    const fence = /^\s*```(\w*)\s*$/.exec(line);
    if (fence) {
      flush();
      const body: string[] = [];
      // An unterminated fence runs to the end of the text rather than throwing
      // it away -- a reply truncated mid-snippet is exactly the case this
      // renderer exists for.
      for (++i; i < lines.length && !/^\s*```\s*$/.test(lines[i]); ++i) body.push(lines[i]);
      blocks.push({ kind: 'code', lang: fence[1], body: body.join('\n') });
      continue;
    }

    const heading = /^(#{1,6})\s+(.*)$/.exec(line);
    if (heading) {
      flush();
      blocks.push({ kind: 'heading', level: heading[1].length, body: heading[2].trim() });
      continue;
    }

    const item = /^\s*[-*]\s+(.*)$/.exec(line);
    if (item) {
      if (para.length) { blocks.push({ kind: 'para', body: para.join('\n') }); para = []; }
      list.push(item[1]);
      continue;
    }

    if (!line.trim()) { flush(); continue; }
    if (list.length) { blocks.push({ kind: 'list', items: list }); list = []; }
    para.push(line);
  }
  flush();
  return blocks;
}

export default function Markdown({ text }: { text: string }) {
  return (
    <div className="md">
      {parse(text).map((block, i) => {
        switch (block.kind) {
          case 'code':
            return <pre key={i} className="code-snippet" data-lang={block.lang}><code>{block.body}</code></pre>;
          case 'heading': {
            // Clamped to h4..h6: this renders inside a panel whose own heading
            // is an h4, so a literal h1 from the model would outrank it.
            const Tag = (`h${Math.min(6, block.level + 3)}`) as 'h4' | 'h5' | 'h6';
            return <Tag key={i} className="md-heading">{renderInline(block.body)}</Tag>;
          }
          case 'list':
            return (
              <ul key={i} className="md-list">
                {block.items.map((item, j) => <li key={j}>{renderInline(item)}</li>)}
              </ul>
            );
          default:
            return <p key={i} className="md-para">{renderInline(block.body)}</p>;
        }
      })}
    </div>
  );
}
