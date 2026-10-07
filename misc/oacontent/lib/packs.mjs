// packs.mjs: the content search path of an OpenArena install as the engine
// sees it: baseoa/*.pk3 sorted by name, a later pack overriding an earlier one
// for the same path (paths compare case-insensitively), plus loose files in
// extra directories searched last-wins as well.
import fs from 'node:fs';
import path from 'node:path';
import { ZipReader } from './zip.mjs';

export const DEFAULT_BASEOA = process.env.OA_BASEOA || '/usr/share/games/openarena/baseoa';

export class ContentSet {
  constructor(dirs = [DEFAULT_BASEOA], { extraPacks = [] } = {}) {
    this.packs = [];                       // { name, file, zip }
    this.index = new Map();                // lower path -> { pack index, name }
    const files = [];
    for (const dir of dirs) {
      if (!fs.existsSync(dir)) continue;
      for (const f of fs.readdirSync(dir).filter((x) => /\.pk3$/i.test(x))) files.push(path.join(dir, f));
    }
    files.sort((a, b) => (path.basename(a).toLowerCase() < path.basename(b).toLowerCase() ? -1 : 1));
    for (const f of [...files, ...extraPacks]) this.#add(f);
  }

  #add(file) {
    const zip = new ZipReader(file);
    const pi = this.packs.length;
    this.packs.push({ name: path.basename(file), file, zip });
    for (const n of zip.names()) this.index.set(n.toLowerCase(), { pack: pi, name: n });
  }

  has(name) { return this.index.has(name.toLowerCase()); }
  packOf(name) { const e = this.index.get(name.toLowerCase()); return e ? this.packs[e.pack].name : null; }
  read(name) {
    const e = this.index.get(name.toLowerCase());
    if (!e) throw new Error(`no file ${name} in the search path`);
    return this.packs[e.pack].zip.read(e.name);
  }
  list(re) { const out = []; for (const [k, v] of this.index) if (re.test(k)) out.push(v.name); return out.sort(); }

  // every stock map: { name, path, pack }
  maps() {
    return this.list(/^maps\/[^/]+\.bsp$/).map((p) => ({ name: path.basename(p, '.bsp'), path: p, pack: this.packOf(p) }));
  }

  shaderFiles() {
    return this.list(/^scripts\/[^/]+\.shader$/).map((p) => ({ name: p.replace(/^scripts\//i, ''), data: this.read(p) }));
  }
}
