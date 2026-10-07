// entities.mjs: the entity lump's text format ({ "key" "value" } blocks), as
// the engine and q3map2 read it, parsed to objects and written back.
export function parseEntities(text) {
  const ents = [];
  let i = 0;
  const n = text.length;
  const skip = () => {
    for (;;) {
      while (i < n && /\s/.test(text[i])) i++;
      if (text[i] === '/' && text[i + 1] === '/') { while (i < n && text[i] !== '\n') i++; continue; }
      if (text[i] === '/' && text[i + 1] === '*') { i = text.indexOf('*/', i + 2); i = i < 0 ? n : i + 2; continue; }
      return;
    }
  };
  const token = () => {
    skip();
    if (i >= n) return null;
    if (text[i] === '"') { const e = text.indexOf('"', i + 1); const t = text.slice(i + 1, e < 0 ? n : e); i = e < 0 ? n : e + 1; return { q: true, t }; }
    const s = i; while (i < n && !/[\s"]/.test(text[i]) && text[i] !== '{' && text[i] !== '}') i++;
    if (i === s) { i++; return { q: false, t: text[i - 1] }; }
    return { q: false, t: text.slice(s, i) };
  };
  for (;;) {
    const t = token();
    if (!t) break;
    if (t.t !== '{' || t.q) throw new Error(`entities: expected { at ${i}`);
    const keys = [];
    for (;;) {
      const k = token();
      if (!k) throw new Error('entities: unterminated block');
      if (!k.q && k.t === '}') break;
      const v = token();
      if (!v || (!v.q && (v.t === '}' || v.t === '{'))) throw new Error(`entities: key ${k.t} has no value`);
      keys.push([k.t, v.t]);
    }
    ents.push(new Entity(keys));
  }
  return ents;
}

export class Entity {
  constructor(keys = []) { this.keys = keys; }
  get(k) { const e = this.keys.find(([a]) => a.toLowerCase() === k.toLowerCase()); return e ? e[1] : undefined; }
  set(k, v) { const e = this.keys.find(([a]) => a.toLowerCase() === k.toLowerCase()); if (e) e[1] = String(v); else this.keys.push([k, String(v)]); return this; }
  get classname() { return this.get('classname'); }
  vec(k) { const v = this.get(k); return v ? v.trim().split(/\s+/).map(Number) : null; }
  get origin() { return this.vec('origin'); }
}

export function serializeEntities(ents) {
  return ents.map((e) => `{\n${e.keys.map(([k, v]) => `"${k}" "${v}"`).join('\n')}\n}\n`).join('');
}

export function entity(classname, props = {}) {
  const e = new Entity([['classname', classname]]);
  for (const [k, v] of Object.entries(props)) e.set(k, Array.isArray(v) ? v.map((x) => +(+x).toFixed(3)).join(' ') : v);
  return e;
}
