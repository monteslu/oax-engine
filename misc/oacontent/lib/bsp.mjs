// bsp.mjs: a reader for Quake III BSP (IBSP 46) files, the format of every
// stock OpenArena map: all the lumps the content tools need, as plain
// arrays and typed arrays, plus the spatial helpers built on them
// (point in leaf, upward rays through the brushes, surface triangles).
export const LUMP = { ENTITIES: 0, SHADERS: 1, PLANES: 2, NODES: 3, LEAFS: 4, LEAFSURFACES: 5, LEAFBRUSHES: 6, MODELS: 7, BRUSHES: 8, BRUSHSIDES: 9, DRAWVERTS: 10, DRAWINDEXES: 11, FOGS: 12, SURFACES: 13, LIGHTMAPS: 14, LIGHTGRID: 15, VISIBILITY: 16 };
export const SURF = { NODAMAGE: 1, SLICK: 2, SKY: 4, LADDER: 8, NOIMPACT: 0x10, NOMARKS: 0x20, FLESH: 0x40, NODRAW: 0x80, HINT: 0x100, SKIP: 0x200, NOLIGHTMAP: 0x400, POINTLIGHT: 0x800, METALSTEPS: 0x1000, NOSTEPS: 0x2000, NONSOLID: 0x4000, LIGHTFILTER: 0x8000, ALPHASHADOW: 0x10000, NODLIGHT: 0x20000 };
export const CONTENTS = { SOLID: 1, LAVA: 8, SLIME: 16, WATER: 32, FOG: 64, PLAYERCLIP: 0x10000, MONSTERCLIP: 0x20000, TRIGGER: 0x40000000, STRUCTURAL: 0x10000000, DETAIL: 0x8000000 };
export const MST = { BAD: 0, PLANAR: 1, PATCH: 2, TRIANGLE_SOUP: 3, FLARE: 4 };

const cstr = (b, o, n) => { let e = o; const end = o + n; while (e < end && b[e]) e++; return b.toString('latin1', o, e); };

export class Bsp {
  constructor(buf, name = '') {
    this.buf = buf;
    this.name = name;
    if (buf.toString('latin1', 0, 4) !== 'IBSP') throw new Error(`${name}: not an IBSP file`);
    this.version = buf.readInt32LE(4);
    if (this.version !== 46) throw new Error(`${name}: IBSP version ${this.version}, expected 46`);
    this.lumps = [];
    for (let i = 0; i < 17; i++) this.lumps.push({ ofs: buf.readInt32LE(8 + i * 8), len: buf.readInt32LE(12 + i * 8) });
    this._c = {};
  }

  #lump(i) { const l = this.lumps[i]; return this.buf.subarray(l.ofs, l.ofs + l.len); }
  #cache(key, fn) { return key in this._c ? this._c[key] : (this._c[key] = fn()); }

  get entityText() { return this.#cache('ent', () => cstr(this.buf, this.lumps[0].ofs, this.lumps[0].len)); }

  get shaders() {
    return this.#cache('shaders', () => {
      const b = this.buf, l = this.lumps[LUMP.SHADERS], out = [];
      for (let o = l.ofs; o < l.ofs + l.len; o += 72) out.push({ name: cstr(b, o, 64), surfaceFlags: b.readInt32LE(o + 64), contents: b.readInt32LE(o + 68) });
      return out;
    });
  }

  get planes() {
    return this.#cache('planes', () => {
      const b = this.buf, l = this.lumps[LUMP.PLANES], n = l.len / 16, nx = new Float32Array(n * 4);
      for (let i = 0; i < n; i++) for (let k = 0; k < 4; k++) nx[i * 4 + k] = b.readFloatLE(l.ofs + i * 16 + k * 4);
      return nx; // nx, ny, nz, dist per plane
    });
  }

  get nodes() {
    return this.#cache('nodes', () => {
      const b = this.buf, l = this.lumps[LUMP.NODES], n = l.len / 36, out = [];
      for (let i = 0; i < n; i++) { const o = l.ofs + i * 36; out.push({ plane: b.readInt32LE(o), children: [b.readInt32LE(o + 4), b.readInt32LE(o + 8)], mins: [b.readInt32LE(o + 12), b.readInt32LE(o + 16), b.readInt32LE(o + 20)], maxs: [b.readInt32LE(o + 24), b.readInt32LE(o + 28), b.readInt32LE(o + 32)] }); }
      return out;
    });
  }

  get leafs() {
    return this.#cache('leafs', () => {
      const b = this.buf, l = this.lumps[LUMP.LEAFS], n = l.len / 48, out = [];
      for (let i = 0; i < n; i++) { const o = l.ofs + i * 48; const r = (k) => b.readInt32LE(o + k * 4); out.push({ cluster: r(0), area: r(1), mins: [r(2), r(3), r(4)], maxs: [r(5), r(6), r(7)], firstSurface: r(8), numSurfaces: r(9), firstBrush: r(10), numBrushes: r(11) }); }
      return out;
    });
  }

  get leafSurfaces() { return this.#cache('ls', () => { const l = this.lumps[LUMP.LEAFSURFACES]; return new Int32Array(this.buf.buffer.slice(this.buf.byteOffset + l.ofs, this.buf.byteOffset + l.ofs + l.len)); }); }
  get leafBrushes() { return this.#cache('lb', () => { const l = this.lumps[LUMP.LEAFBRUSHES]; return new Int32Array(this.buf.buffer.slice(this.buf.byteOffset + l.ofs, this.buf.byteOffset + l.ofs + l.len)); }); }

  get models() {
    return this.#cache('models', () => {
      const b = this.buf, l = this.lumps[LUMP.MODELS], out = [];
      for (let o = l.ofs; o < l.ofs + l.len; o += 40) out.push({ mins: [0, 1, 2].map((k) => b.readFloatLE(o + k * 4)), maxs: [3, 4, 5].map((k) => b.readFloatLE(o + k * 4)), firstSurface: b.readInt32LE(o + 24), numSurfaces: b.readInt32LE(o + 28), firstBrush: b.readInt32LE(o + 32), numBrushes: b.readInt32LE(o + 36) });
      return out;
    });
  }

  get brushes() {
    return this.#cache('brushes', () => {
      const b = this.buf, l = this.lumps[LUMP.BRUSHES], out = [];
      for (let o = l.ofs; o < l.ofs + l.len; o += 12) out.push({ firstSide: b.readInt32LE(o), numSides: b.readInt32LE(o + 4), shader: b.readInt32LE(o + 8) });
      return out;
    });
  }

  get brushSides() {
    return this.#cache('sides', () => {
      const b = this.buf, l = this.lumps[LUMP.BRUSHSIDES], out = [];
      for (let o = l.ofs; o < l.ofs + l.len; o += 8) out.push({ plane: b.readInt32LE(o), shader: b.readInt32LE(o + 4) });
      return out;
    });
  }

  // xyz, st, lightmap st, normal, rgba in flat typed arrays
  get verts() {
    return this.#cache('verts', () => {
      const b = this.buf, l = this.lumps[LUMP.DRAWVERTS], n = l.len / 44;
      const xyz = new Float32Array(n * 3), normal = new Float32Array(n * 3), st = new Float32Array(n * 2), lm = new Float32Array(n * 2), rgba = new Uint8Array(n * 4);
      for (let i = 0; i < n; i++) {
        const o = l.ofs + i * 44;
        xyz[i * 3] = b.readFloatLE(o); xyz[i * 3 + 1] = b.readFloatLE(o + 4); xyz[i * 3 + 2] = b.readFloatLE(o + 8);
        st[i * 2] = b.readFloatLE(o + 12); st[i * 2 + 1] = b.readFloatLE(o + 16);
        lm[i * 2] = b.readFloatLE(o + 20); lm[i * 2 + 1] = b.readFloatLE(o + 24);
        normal[i * 3] = b.readFloatLE(o + 28); normal[i * 3 + 1] = b.readFloatLE(o + 32); normal[i * 3 + 2] = b.readFloatLE(o + 36);
        rgba[i * 4] = b[o + 40]; rgba[i * 4 + 1] = b[o + 41]; rgba[i * 4 + 2] = b[o + 42]; rgba[i * 4 + 3] = b[o + 43];
      }
      return { n, xyz, normal, st, lm, rgba };
    });
  }

  get indexes() { return this.#cache('idx', () => { const l = this.lumps[LUMP.DRAWINDEXES]; return new Int32Array(this.buf.buffer.slice(this.buf.byteOffset + l.ofs, this.buf.byteOffset + l.ofs + l.len)); }); }

  get surfaces() {
    return this.#cache('surfaces', () => {
      const b = this.buf, l = this.lumps[LUMP.SURFACES], out = [];
      for (let o = l.ofs; o < l.ofs + l.len; o += 104) {
        const r = (k) => b.readInt32LE(o + k * 4);
        const f = (k) => b.readFloatLE(o + k * 4);
        out.push({
          shader: r(0), fog: r(1), type: r(2), firstVert: r(3), numVerts: r(4), firstIndex: r(5), numIndexes: r(6),
          lightmap: r(7), lmX: r(8), lmY: r(9), lmW: r(10), lmH: r(11),
          lmOrigin: [f(12), f(13), f(14)], lmVecs: [[f(15), f(16), f(17)], [f(18), f(19), f(20)], [f(21), f(22), f(23)]],
          patchWidth: r(24), patchHeight: r(25),
        });
      }
      return out;
    });
  }

  // one 128 x 128 page of the lightmap lump as a Buffer of RGB bytes
  lightmapPage(i) { const l = this.lumps[LUMP.LIGHTMAPS]; return this.buf.subarray(l.ofs + i * 49152, l.ofs + (i + 1) * 49152); }

  get numLightmaps() { return this.lumps[LUMP.LIGHTMAPS].len / (128 * 128 * 3); }

  shaderOf(surf) { return this.shaders[surf.shader]; }

  // sky by the surface flag the compiler wrote, or by name for shaders whose
  // script says sky (set bsp.skyNames = Set of lower-case names): a map's
  // shader lump does not always carry the flag
  isSky(shaderIndex) {
    const sh = this.shaders[shaderIndex];
    return !!(sh && ((sh.surfaceFlags & SURF.SKY) || this.skyNames?.has(sh.name.toLowerCase())));
  }

  // world triangles of a surface (planar faces and triangle soups index their
  // verts; a patch's control mesh is returned as its grid cells, two triangles
  // each, which is the coarse hull, not the tessellated surface)
  surfaceTriangles(surf) {
    const v = this.verts.xyz, tris = [];
    const P = (i) => [v[i * 3], v[i * 3 + 1], v[i * 3 + 2]];
    if (surf.type === MST.PLANAR || surf.type === MST.TRIANGLE_SOUP) {
      for (let t = 0; t + 2 < surf.numIndexes; t += 3) {
        tris.push([0, 1, 2].map((k) => P(this.indexes[surf.firstIndex + t + k] + surf.firstVert)));
      }
    } else if (surf.type === MST.PATCH) {
      const w = surf.patchWidth, h = surf.patchHeight;
      for (let y = 0; y + 1 < h; y++) for (let x = 0; x + 1 < w; x++) {
        const a = P(surf.firstVert + y * w + x), b2 = P(surf.firstVert + y * w + x + 1), c = P(surf.firstVert + (y + 1) * w + x), d = P(surf.firstVert + (y + 1) * w + x + 1);
        tris.push([a, b2, c], [b2, d, c]);
      }
    }
    return tris;
  }

  pointLeaf(p) {
    const nodes = this.nodes, planes = this.planes;
    let n = 0;
    while (n >= 0) {
      const nd = nodes[n], q = nd.plane * 4;
      const d = planes[q] * p[0] + planes[q + 1] * p[1] + planes[q + 2] * p[2] - planes[q + 3];
      n = nd.children[d >= 0 ? 0 : 1];
    }
    return -n - 1;
  }

  // brush planes as [nx, ny, nz, dist, shaderIndex] arrays, cached
  brushPlanes(i) {
    this._bp ||= new Map();
    if (this._bp.has(i)) return this._bp.get(i);
    const br = this.brushes[i], out = [];
    for (let s = 0; s < br.numSides; s++) {
      const side = this.brushSides[br.firstSide + s], q = side.plane * 4;
      out.push([this.planes[q], this.planes[q + 1], this.planes[q + 2], this.planes[q + 3], side.shader]);
    }
    this._bp.set(i, out);
    return out;
  }

  // the brush bounds (from its planes' box faces: Q3 brushes carry axial planes first)
  brushBounds(i) {
    this._bb ||= new Map();
    if (this._bb.has(i)) return this._bb.get(i);
    const pl = this.brushPlanes(i);
    const bb = [-Infinity, -Infinity, -Infinity, Infinity, Infinity, Infinity];
    for (const [nx, ny, nz, d] of pl) {
      if (nx === 1) bb[3] = Math.min(bb[3], d); else if (nx === -1) bb[0] = Math.max(bb[0], -d);
      if (ny === 1) bb[4] = Math.min(bb[4], d); else if (ny === -1) bb[1] = Math.max(bb[1], -d);
      if (nz === 1) bb[5] = Math.min(bb[5], d); else if (nz === -1) bb[2] = Math.max(bb[2], -d);
    }
    this._bb.set(i, bb);
    return bb;
  }

  // The first world brush a ray from `from` along unit vector `dir` meets
  // within maxT units: { t, point, brush, shader: shader index of the entered
  // side, sky } or null. Only the world model's brushes (model 0) count: brushes
  // whose contents intersect `mask` block (solid by default: player clip is
  // invisible and blocks no light), sky brushes count as hits with sky: true.
  // Brush entities (doors, movers) are not part of the world.
  // the world brushes by 256-unit cells of the xy plane, for rays that would
  // otherwise test every brush
  #buildGrid() {
    const CELL = 256;
    const m = this.models[0];
    const gx0 = Math.floor(m.mins[0] / CELL) - 1, gy0 = Math.floor(m.mins[1] / CELL) - 1;
    const gw = Math.ceil((m.maxs[0] - m.mins[0]) / CELL) + 3, gh = Math.ceil((m.maxs[1] - m.mins[1]) / CELL) + 3;
    const cells = Array.from({ length: gw * gh }, () => []);
    this._bbs.forEach((e, bi) => {
      const bb = e[1];
      const x0 = Math.max(0, Math.floor(Math.max(bb[0], m.mins[0] - CELL) / CELL) - gx0), x1 = Math.min(gw - 1, Math.floor(Math.min(bb[3], m.maxs[0] + CELL) / CELL) - gx0);
      const y0 = Math.max(0, Math.floor(Math.max(bb[1], m.mins[1] - CELL) / CELL) - gy0), y1 = Math.min(gh - 1, Math.floor(Math.min(bb[4], m.maxs[1] + CELL) / CELL) - gy0);
      for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) cells[y * gw + x].push(bi);
    });
    this._grid = { CELL, gx0, gy0, gw, gh, cells, stamp: new Int32Array(this._bbs.length), n: 0 };
  }

  #brushesAlong(from, end) {
    const g = this._grid;
    g.n++;
    const out = [];
    const len = Math.hypot(end[0] - from[0], end[1] - from[1]);
    const steps = Math.max(1, Math.ceil(len / (g.CELL / 2)));
    for (let k = 0; k <= steps; k++) {
      const x = from[0] + (end[0] - from[0]) * (k / steps), y = from[1] + (end[1] - from[1]) * (k / steps);
      const cx = Math.floor(x / g.CELL) - g.gx0, cy = Math.floor(y / g.CELL) - g.gy0;
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        const X = cx + dx, Y = cy + dy;
        if (X < 0 || Y < 0 || X >= g.gw || Y >= g.gh) continue;
        for (const bi of g.cells[Y * g.gw + X]) if (g.stamp[bi] !== g.n) { g.stamp[bi] = g.n; out.push(bi); }
      }
    }
    return out;
  }

  ray(from, dir, maxT = 100000, mask = CONTENTS.SOLID) {
    this._worldBrushes ||= (() => { const m = this.models[0]; const a = []; for (let i = 0; i < m.numBrushes; i++) a.push(m.firstBrush + i); return a; })();
    this._bbs ||= this._worldBrushes.map((i) => [i, this.brushBounds(i)]);
    if (!this._grid && this.useGrid !== false && this._bbs.length > 200) this.#buildGrid();
    const end = [from[0] + dir[0] * maxT, from[1] + dir[1] * maxT, from[2] + dir[2] * maxT];
    const lo = [Math.min(from[0], end[0]), Math.min(from[1], end[1]), Math.min(from[2], end[2])];
    const hi = [Math.max(from[0], end[0]), Math.max(from[1], end[1]), Math.max(from[2], end[2])];
    let best = null;
    const list = this._grid && this.useGrid !== false ? this.#brushesAlong(from, end).map((bi) => this._bbs[bi]) : this._bbs;
    for (const [i, bb] of list) {
      if (bb[3] < lo[0] || bb[0] > hi[0] || bb[4] < lo[1] || bb[1] > hi[1] || bb[5] < lo[2] || bb[2] > hi[2]) continue;
      const sh = this.shaders[this.brushes[i].shader];
      if (!(sh.contents & mask) && !this.isSky(this.brushes[i].shader)) continue;
      // slab test against the brush's box first: the box is the cheap reject
      let t0 = 0, t1 = maxT, ok = true;
      for (let a = 0; a < 3 && ok; a++) {
        if (Math.abs(dir[a]) < 1e-9) { if (from[a] < bb[a] || from[a] > bb[a + 3]) ok = false; continue; }
        let ta = (bb[a] - from[a]) / dir[a], tb = (bb[a + 3] - from[a]) / dir[a];
        if (ta > tb) { const x = ta; ta = tb; tb = x; }
        if (ta > t0) t0 = ta; if (tb < t1) t1 = tb;
        if (t0 > t1) ok = false;
      }
      if (!ok) continue;
      let tEnter = 0, tLeave = maxT, enterSide = -1;
      const pl = this.brushPlanes(i);
      for (let k = 0; k < pl.length; k++) {
        const [nx, ny, nz, d] = pl[k];
        const dist = nx * from[0] + ny * from[1] + nz * from[2] - d;     // > 0: outside this plane at the start
        const dn = nx * dir[0] + ny * dir[1] + nz * dir[2];
        if (Math.abs(dn) < 1e-9) { if (dist > 0) { tEnter = Infinity; break; } continue; }
        const t = -dist / dn;
        if (dn < 0) { if (t > tEnter) { tEnter = t; enterSide = k; } } else if (t < tLeave) tLeave = t;
        if (tEnter > tLeave) break;
      }
      if (tEnter > tLeave || tEnter === Infinity) continue;
      if (best === null || tEnter < best.t) {
        const side = enterSide >= 0 ? pl[enterSide][4] : this.brushes[i].shader;
        best = { t: tEnter, point: [from[0] + dir[0] * tEnter, from[1] + dir[1] * tEnter, from[2] + dir[2] * tEnter], brush: i, shader: side, sky: this.isSky(side) };
      }
    }
    return best;
  }

  // a ray straight up: the first hit with z (the height reached)
  upRay(from, maxZ = 100000) {
    const h = this.ray(from, [0, 0, 1], maxZ - from[2]);
    return h ? { ...h, z: h.point[2] } : null;
  }
}
