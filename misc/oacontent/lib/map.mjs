// map.mjs: reads the entity blocks of a Q3/q3map2 .map source file (brushes and
// patches inside entities are skipped), for importing the light entities of
// map sources that are available.
import { Entity } from './entities.mjs';

export function parseMapSource(text) {
  const ents = [];
  let i = 0;
  const n = text.length;
  const skip = () => {
    for (;;) {
      while (i < n && /\s/.test(text[i])) i++;
      if (text[i] === '/' && text[i + 1] === '/') { while (i < n && text[i] !== '\n') i++; continue; }
      return;
    }
  };
  for (;;) {
    skip();
    if (i >= n) break;
    if (text[i] !== '{') throw new Error(`map source: expected { at ${i}`);
    i++;
    const keys = [];
    for (;;) {
      skip();
      if (i >= n) throw new Error('map source: unterminated entity');
      if (text[i] === '}') { i++; break; }
      if (text[i] === '{') {                       // a brush or patch: skip to its matching brace
        let depth = 0;
        for (; i < n; i++) { if (text[i] === '{') depth++; else if (text[i] === '}') { depth--; if (!depth) { i++; break; } } }
        continue;
      }
      if (text[i] !== '"') throw new Error(`map source: expected a key at ${i}`);
      const e1 = text.indexOf('"', i + 1); const k = text.slice(i + 1, e1); i = e1 + 1;
      skip();
      if (text[i] !== '"') throw new Error(`map source: expected a value for ${k}`);
      const e2 = text.indexOf('"', i + 1); const v = text.slice(i + 1, e2); i = e2 + 1;
      keys.push([k, v]);
    }
    ents.push(new Entity(keys));
  }
  return ents;
}
