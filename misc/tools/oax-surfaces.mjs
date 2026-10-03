#!/usr/bin/env node
// oax-surfaces.mjs: write and read the surface-world BSPX lumps
// OAX_SURFACES and OAX_COLLISION (specification: docs/map-format.md;
// engine side: code/qcommon/oax_surfaces.h).
//
// Used by tests/maps/build.mjs and by importers (an external converter):
//
//   import { SurfaceWorld, CollisionMeshes, OSF, uvMatrix } from '.../oax-surfaces.mjs';
//   const sw = new SurfaceWorld();
//   sw.polygon({ material: 'textures/base_floor/clang_floor', points: [[0,0,0],[64,0,0],[64,64,0],[0,64,0]],
//                uv: uvMatrix({ u: [1/64,0,0], v: [0,-1/64,0] }) });
//   sw.mesh({ material, positions, indexes, st, normals, flags: OSF.TWOSIDED });
//   // after q3map2 compiled the hull: bind every surface to the BSP leaves it passes through
//   const stats = sw.bindLeaves(fs.readFileSync('maps/x.bsp'));
//   addLump('maps/x.bsp', 'OAX_SURFACES', sw.toBuffer());
//
// CLI:
//   node misc/tools/oax-surfaces.mjs dump <map.bsp>     summary of both lumps

import fs from 'node:fs';
import { readBspx } from './bspx.mjs';

export const OSF = {
  TWOSIDED: 0x001,
  MASKED: 0x002,
  TRANSLUCENT: 0x004,
  ADDITIVE: 0x008,
  INVISIBLE: 0x010,
  UVMATRIX: 0x020,
  NORMALS: 0x040,
  TANGENTS: 0x080,
  COLORS: 0x100,
  DETAIL: 0x200,
  NOSHADOW: 0x400,
};
const OSF_KNOWN = 0x7ff;

export const SURFACES_VERSION = 1;
export const COLLISION_VERSION = 1;
const HEADER = 80, MATERIAL = 72, SURFACE = 112, VERT = 52, NAME = 64;
const C_HEADER = 48, C_MESH = 48, C_VERT = 12;

const f32 = Math.fround;
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const len = (a) => Math.sqrt(dot(a, a));
const norm = (a) => { const l = len(a); return l > 0 ? [a[0] / l, a[1] / l, a[2] / l] : [0, 0, 0]; };

// A texture matrix from axes: s = u . p + uOffset, t = v . p + vOffset, in
// texture repeats (divide world-unit axes by the texture size first).
export function uvMatrix({ u, v, uOffset = 0, vOffset = 0 }) {
  return [[u[0], u[1], u[2], uOffset], [v[0], v[1], v[2], vOffset]];
}

// Newell's normal of a polygon (any winding direction, robust to collinear points)
function newell(pts) {
  const n = [0, 0, 0];
  for (let i = 0; i < pts.length; i++) {
    const a = pts[i], b = pts[(i + 1) % pts.length];
    n[0] += (a[1] - b[1]) * (a[2] + b[2]);
    n[1] += (a[2] - b[2]) * (a[0] + b[0]);
    n[2] += (a[0] - b[0]) * (a[1] + b[1]);
  }
  return n;
}

function isConvex(pts, n) {
  for (let i = 0; i < pts.length; i++) {
    const a = pts[i], b = pts[(i + 1) % pts.length], c = pts[(i + 2) % pts.length];
    if (dot(cross(sub(b, a), sub(c, b)), n) < -1e-6) return false;
  }
  return true;
}

// ear clipping for a simple (possibly concave) planar polygon wound
// counter-clockwise about n; returns triangle indexes
function triangulate(pts, n) {
  const idx = pts.map((_, i) => i);
  const out = [];
  const inside = (p, a, b, c) =>
    dot(cross(sub(b, a), sub(p, a)), n) >= 0 && dot(cross(sub(c, b), sub(p, b)), n) >= 0 && dot(cross(sub(a, c), sub(p, c)), n) >= 0;
  let guard = 0;
  while (idx.length > 3 && guard++ < 10000) {
    let cut = false;
    for (let i = 0; i < idx.length; i++) {
      const ia = idx[(i + idx.length - 1) % idx.length], ib = idx[i], ic = idx[(i + 1) % idx.length];
      const a = pts[ia], b = pts[ib], c = pts[ic];
      if (dot(cross(sub(b, a), sub(c, b)), n) <= 1e-9) continue;   // reflex or degenerate
      if (idx.some((k) => k !== ia && k !== ib && k !== ic && inside(pts[k], a, b, c))) continue;
      out.push(ia, ib, ic);
      idx.splice(i, 1);
      cut = true;
      break;
    }
    if (!cut) throw new Error('oax-surfaces: polygon could not be triangulated (self-intersecting?)');
  }
  out.push(idx[0], idx[1], idx[2]);
  return out;
}

export class SurfaceWorld {
  constructor() {
    this.materials = [];
    this.materialIndex = new Map();
    this.surfaces = [];
    this.bound = false;
  }

  material(name) {
    if (typeof name !== 'string' || !name || Buffer.byteLength(name, 'latin1') >= NAME) throw new Error(`oax-surfaces: bad material name "${name}"`);
    if (!this.materialIndex.has(name)) {
      this.materialIndex.set(name, this.materials.length);
      this.materials.push(name);
    }
    return this.materialIndex.get(name);
  }

  // common fields of polygon() and mesh()
  #base(o, positions) {
    const flags = (o.flags || 0) | (o.uv ? OSF.UVMATRIX : 0) | (o.normals ? OSF.NORMALS : 0) | (o.tangents ? OSF.TANGENTS : 0) | (o.colors ? OSF.COLORS : 0);
    if (flags & ~OSF_KNOWN) throw new Error(`oax-surfaces: unknown flags ${(flags & ~OSF_KNOWN).toString(16)}`);
    if (!o.uv && !o.st) throw new Error('oax-surfaces: a surface needs uv (matrix) or st (per vertex)');
    for (const [k, per] of [['st', 2], ['normals', 3], ['tangents', 4], ['colors', 4]]) {
      if (o[k] && (o[k].length !== positions.length || o[k].some((x) => x.length !== per))) throw new Error(`oax-surfaces: ${k} must have ${per} values per vertex`);
    }
    for (const p of positions) if (p.length !== 3 || p.some((x) => !Number.isFinite(x))) throw new Error('oax-surfaces: bad vertex position');
    const tint = o.tint ? [...o.tint, ...[1, 1, 1, 1].slice(o.tint.length)] : [1, 1, 1, 1];
    return {
      material: this.material(o.material), flags, lightMask: (o.lightMask ?? 0) >>> 0, model: o.model || 0, tint,
      uv: o.uv || [[0, 0, 0, 0], [0, 0, 0, 0]], positions: positions.map((p) => p.map(f32)),
      st: o.st || positions.map((p) => o.uv.map((r) => r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3])),
      normals: o.normals, tangents: o.tangents, colors: o.colors, sourceId: o.sourceId ?? -1,
      leaves: null, area: -1,
    };
  }

  // A polygon (counter-clockwise seen from its front). A concave polygon is
  // triangulated into a mesh. Returns the surface index.
  polygon(o) {
    const pts = o.points;
    if (!pts || pts.length < 3) throw new Error('oax-surfaces: a polygon needs 3 or more points');
    const s = this.#base(o, pts);
    const nn = newell(s.positions);
    if (len(nn) < 1e-6) throw new Error('oax-surfaces: degenerate polygon');
    const n = norm(nn);
    s.plane = [n[0], n[1], n[2], dot(n, s.positions[0])];
    for (const p of s.positions) {
      if (Math.abs(dot(n, p) - s.plane[3]) > 0.1) throw new Error(`oax-surfaces: polygon is not planar (${(dot(n, p) - s.plane[3]).toFixed(3)} off)`);
    }
    s.indexes = isConvex(s.positions, n) ? null : triangulate(s.positions, n);
    this.surfaces.push(s);
    return this.surfaces.length - 1;
  }

  // An indexed triangle mesh (counter-clockwise seen from the front).
  mesh(o) {
    const s = this.#base(o, o.positions);
    if (!o.indexes || o.indexes.length < 3 || o.indexes.length % 3) throw new Error('oax-surfaces: a mesh needs triangles');
    if (o.indexes.some((i) => !(i >= 0 && i < o.positions.length))) throw new Error('oax-surfaces: mesh index out of range');
    s.indexes = [...o.indexes];
    s.plane = [0, 0, 0, 0];
    this.surfaces.push(s);
    return this.surfaces.length - 1;
  }

  // triangles of a surface as point triples (with their own normal)
  static triangles(s) {
    const out = [];
    if (s.indexes) {
      for (let i = 0; i < s.indexes.length; i += 3) out.push([s.positions[s.indexes[i]], s.positions[s.indexes[i + 1]], s.positions[s.indexes[i + 2]]]);
    } else {
      for (let i = 1; i + 1 < s.positions.length; i++) out.push([s.positions[0], s.positions[i], s.positions[i + 1]]);
    }
    return out;
  }

  // Assign every world surface the non-solid leaves of the compiled BSP it
  // passes through (see docs/map-format.md, "Leaf references"). Returns
  // { unbound: [surface indexes with no leaf], leafRefs, areas }.
  bindLeaves(bspBuf) {
    const bsp = readBspTree(bspBuf);
    const unbound = [];
    let refs = 0;
    this.surfaces.forEach((s, k) => {
      if (s.model) { s.leaves = []; s.area = -1; return; }
      const leaves = new Set();
      const twoSided = (s.flags & OSF.TWOSIDED) !== 0;
      if (!s.indexes) {
        const n = s.plane.slice(0, 3);
        polyLeaves(bsp, 0, s.positions, n, twoSided, leaves);
      } else {
        for (const tri of SurfaceWorld.triangles(s)) {
          const n = norm(cross(sub(tri[1], tri[0]), sub(tri[2], tri[0])));
          if (len(n) === 0) continue;
          polyLeaves(bsp, 0, tri, n, twoSided, leaves);
        }
      }
      s.leaves = [...leaves].sort((a, b) => a - b);
      const areas = new Set(s.leaves.map((l) => bsp.leafs[l].area));
      s.area = areas.size === 1 ? [...areas][0] : -1;
      refs += s.leaves.length;
      if (!s.leaves.length && !(s.flags & OSF.INVISIBLE)) unbound.push(k);
    });
    this.bound = true;
    return { unbound, leafRefs: refs, leafs: bsp.leafs.length };
  }

  toBuffer() {
    const S = this.surfaces;
    const numVerts = S.reduce((n, s) => n + s.positions.length, 0);
    const numIndexes = S.reduce((n, s) => n + (s.indexes ? s.indexes.length : 0), 0);
    const numRefs = S.reduce((n, s) => n + (s.leaves ? s.leaves.length : 0), 0);
    const ofsMat = HEADER;
    const ofsSurf = ofsMat + this.materials.length * MATERIAL;
    const ofsVert = ofsSurf + S.length * SURFACE;
    const ofsIdx = ofsVert + numVerts * VERT;
    const ofsRef = ofsIdx + numIndexes * 4;
    const b = Buffer.alloc(ofsRef + numRefs * 4);
    b.write('OSRF', 0, 'latin1');
    b.writeInt32LE(SURFACES_VERSION, 4);
    b.writeInt32LE(HEADER, 8);
    b.writeInt32LE(0, 12);
    const table = (at, n, ofs, size) => { b.writeInt32LE(n, at); b.writeInt32LE(ofs, at + 4); b.writeInt32LE(size, at + 8); };
    table(16, this.materials.length, ofsMat, MATERIAL);
    table(28, S.length, ofsSurf, SURFACE);
    table(40, numVerts, ofsVert, VERT);
    table(52, numIndexes, ofsIdx, 4);
    table(64, numRefs, ofsRef, 4);
    this.materials.forEach((m, i) => b.write(m, ofsMat + i * MATERIAL, 'latin1'));
    let v = 0, ix = 0, r = 0;
    S.forEach((s, i) => {
      const p = ofsSurf + i * SURFACE;
      b.writeInt32LE(s.material, p);
      b.writeUInt32LE(s.flags >>> 0, p + 4);
      b.writeUInt32LE(s.lightMask >>> 0, p + 8);
      b.writeInt32LE(s.model, p + 12);
      for (let k = 0; k < 4; k++) {
        b.writeFloatLE(s.tint[k], p + 16 + k * 4);
        b.writeFloatLE(s.uv[0][k], p + 32 + k * 4);
        b.writeFloatLE(s.uv[1][k], p + 48 + k * 4);
        b.writeFloatLE(s.plane[k], p + 64 + k * 4);
      }
      b.writeInt32LE(v, p + 80);
      b.writeInt32LE(s.positions.length, p + 84);
      b.writeInt32LE(ix, p + 88);
      b.writeInt32LE(s.indexes ? s.indexes.length : 0, p + 92);
      b.writeInt32LE(s.leaves ? r : -1, p + 96);
      b.writeInt32LE(s.leaves ? s.leaves.length : 0, p + 100);
      b.writeInt32LE(s.leaves ? s.area : -1, p + 104);
      b.writeInt32LE(s.sourceId, p + 108);
      s.positions.forEach((pos, k) => {
        const q = ofsVert + (v + k) * VERT;
        for (let c = 0; c < 3; c++) b.writeFloatLE(pos[c], q + c * 4);
        b.writeFloatLE(s.st[k][0], q + 12);
        b.writeFloatLE(s.st[k][1], q + 16);
        const n = s.normals ? s.normals[k] : (s.indexes ? [0, 0, 0] : s.plane);
        for (let c = 0; c < 3; c++) b.writeFloatLE(n[c], q + 20 + c * 4);
        const t = s.tangents ? s.tangents[k] : [0, 0, 0, 1];
        for (let c = 0; c < 4; c++) b.writeFloatLE(t[c], q + 32 + c * 4);
        const col = s.colors ? s.colors[k] : [255, 255, 255, 255];
        for (let c = 0; c < 4; c++) b.writeUInt8(Math.max(0, Math.min(255, Math.round(col[c]))), q + 48 + c);
      });
      if (s.indexes) s.indexes.forEach((x, k) => b.writeInt32LE(x, ofsIdx + (ix + k) * 4));
      if (s.leaves) s.leaves.forEach((l, k) => b.writeInt32LE(l, ofsRef + (r + k) * 4));
      v += s.positions.length;
      ix += s.indexes ? s.indexes.length : 0;
      r += s.leaves ? s.leaves.length : 0;
    });
    return b;
  }
}

// ---- reading -----------------------------------------------------------

export function readSurfaces(buf) {
  if (buf.toString('latin1', 0, 4) !== 'OSRF') throw new Error('not an OSRF lump');
  const version = buf.readInt32LE(4);
  if (version !== SURFACES_VERSION) throw new Error(`OAX_SURFACES version ${version}`);
  const t = (at) => ({ n: buf.readInt32LE(at), ofs: buf.readInt32LE(at + 4), size: buf.readInt32LE(at + 8) });
  const M = t(16), S = t(28), V = t(40), I = t(52), R = t(64);
  const materials = [];
  for (let i = 0; i < M.n; i++) materials.push(buf.toString('latin1', M.ofs + i * M.size, M.ofs + i * M.size + NAME).replace(/\0.*$/s, ''));
  const verts = [];
  for (let i = 0; i < V.n; i++) {
    const q = V.ofs + i * V.size;
    const fl = (k) => buf.readFloatLE(q + k * 4);
    verts.push({ xyz: [fl(0), fl(1), fl(2)], st: [fl(3), fl(4)], normal: [fl(5), fl(6), fl(7)], tangent: [fl(8), fl(9), fl(10), fl(11)], color: [...buf.subarray(q + 48, q + 52)] });
  }
  const indexes = [];
  for (let i = 0; i < I.n; i++) indexes.push(buf.readInt32LE(I.ofs + i * 4));
  const leafRefs = [];
  for (let i = 0; i < R.n; i++) leafRefs.push(buf.readInt32LE(R.ofs + i * 4));
  const surfaces = [];
  for (let i = 0; i < S.n; i++) {
    const p = S.ofs + i * S.size;
    const fl = (k) => buf.readFloatLE(p + k);
    const s = {
      material: buf.readInt32LE(p), flags: buf.readUInt32LE(p + 4), lightMask: buf.readUInt32LE(p + 8), model: buf.readInt32LE(p + 12),
      tint: [fl(16), fl(20), fl(24), fl(28)], uv: [[fl(32), fl(36), fl(40), fl(44)], [fl(48), fl(52), fl(56), fl(60)]],
      plane: [fl(64), fl(68), fl(72), fl(76)],
      firstVert: buf.readInt32LE(p + 80), numVerts: buf.readInt32LE(p + 84), firstIndex: buf.readInt32LE(p + 88), numIndexes: buf.readInt32LE(p + 92),
      firstLeaf: buf.readInt32LE(p + 96), numLeafs: buf.readInt32LE(p + 100), area: buf.readInt32LE(p + 104), sourceId: buf.readInt32LE(p + 108),
    };
    s.materialName = materials[s.material];
    s.points = verts.slice(s.firstVert, s.firstVert + s.numVerts).map((x) => x.xyz);
    s.triangles = [];
    if (s.numIndexes) for (let k = 0; k < s.numIndexes; k += 3) s.triangles.push([0, 1, 2].map((c) => s.points[indexes[s.firstIndex + k + c]]));
    else for (let k = 1; k + 1 < s.numVerts; k++) s.triangles.push([s.points[0], s.points[k], s.points[k + 1]]);
    s.leaves = s.firstLeaf >= 0 ? leafRefs.slice(s.firstLeaf, s.firstLeaf + s.numLeafs) : null;
    surfaces.push(s);
  }
  return { version, materials, surfaces, verts, indexes, leafRefs };
}

// both lumps of a compiled map (null when absent)
export function surfacesFromBsp(bspBuf) {
  const { lumps } = readBspx(bspBuf);
  const s = lumps.find((l) => l.name === 'OAX_SURFACES');
  const c = lumps.find((l) => l.name === 'OAX_COLLISION');
  return { surfaces: s ? readSurfaces(Buffer.from(s.data)) : null, collision: c ? readCollision(Buffer.from(c.data)) : null };
}

// ---- the compiled BSP tree (planes, nodes, leafs) ---------------------------

export function readBspTree(buf) {
  if (buf.toString('latin1', 0, 4) !== 'IBSP') throw new Error('not a Q3 BSP');
  const lump = (i) => ({ ofs: buf.readInt32LE(8 + i * 8), len: buf.readInt32LE(12 + i * 8) });
  const P = lump(2), N = lump(3), L = lump(4);
  const planes = [];
  for (let i = 0; i < P.len / 16; i++) {
    const o = P.ofs + i * 16;
    planes.push({ n: [buf.readFloatLE(o), buf.readFloatLE(o + 4), buf.readFloatLE(o + 8)], d: buf.readFloatLE(o + 12) });
  }
  const nodes = [];
  for (let i = 0; i < N.len / 36; i++) {
    const o = N.ofs + i * 36;
    nodes.push({ plane: buf.readInt32LE(o), children: [buf.readInt32LE(o + 4), buf.readInt32LE(o + 8)] });
  }
  const leafs = [];
  for (let i = 0; i < L.len / 48; i++) {
    const o = L.ofs + i * 48;
    leafs.push({ cluster: buf.readInt32LE(o), area: buf.readInt32LE(o + 4) });
  }
  return { planes, nodes, leafs };
}

// clip a polygon down the tree; leaves it reaches that are not solid
const ON_EPS = 0.01;
function polyLeaves(bsp, node, pts, n, twoSided, out) {
  while (node >= 0) {
    const nd = bsp.nodes[node], pl = bsp.planes[nd.plane];
    const d = pts.map((p) => dot(pl.n, p) - pl.d);
    const front = d.some((x) => x > ON_EPS), back = d.some((x) => x < -ON_EPS);
    if (!front && !back) {
      // on the plane: the side the surface faces (both when two-sided)
      const facing = dot(pl.n, n) >= 0 ? 0 : 1;
      if (twoSided) polyLeaves(bsp, nd.children[1 - facing], pts, n, twoSided, out);
      node = nd.children[facing];
      continue;
    }
    if (front && back) {
      const [f, b] = splitPoly(pts, d);
      if (b.length >= 3) polyLeaves(bsp, nd.children[1], b, n, twoSided, out);
      pts = f;
      node = nd.children[0];
      continue;
    }
    node = nd.children[front ? 0 : 1];
  }
  const leaf = -1 - node;
  if (bsp.leafs[leaf].cluster !== -1) out.add(leaf);
}

function splitPoly(pts, d) {
  const f = [], b = [];
  for (let i = 0; i < pts.length; i++) {
    const p = pts[i], q = pts[(i + 1) % pts.length], dp = d[i], dq = d[(i + 1) % pts.length];
    if (dp >= -ON_EPS) f.push(p);
    if (dp <= ON_EPS) b.push(p);
    if ((dp > ON_EPS && dq < -ON_EPS) || (dp < -ON_EPS && dq > ON_EPS)) {
      const t = dp / (dp - dq);
      const m = [p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t, p[2] + (q[2] - p[2]) * t];
      f.push(m);
      b.push(m);
    }
  }
  return [f, b];
}

// ---- OAX_COLLISION ---------------------------------------------------------

export class CollisionMeshes {
  constructor() {
    this.meshes = [];
  }

  // positions [[x,y,z]], indexes (counter-clockwise from outside), contents
  // (default 1, solid), surfaceFlags, thickness (default 4)
  mesh({ positions, indexes, contents = 1, surfaceFlags = 0, thickness = 4, sourceId = -1 }) {
    if (!positions || positions.length < 3) throw new Error('oax-surfaces: a collision mesh needs vertices');
    if (!indexes || indexes.length < 3 || indexes.length % 3 || indexes.some((i) => !(i >= 0 && i < positions.length))) throw new Error('oax-surfaces: bad collision mesh indexes');
    if (!(thickness > 0 && thickness <= 4096)) throw new Error('oax-surfaces: collision thickness must be in (0, 4096]');
    this.meshes.push({ positions: positions.map((p) => p.map(f32)), indexes: [...indexes], contents, surfaceFlags, thickness, sourceId });
    return this.meshes.length - 1;
  }

  toBuffer() {
    const nv = this.meshes.reduce((n, m) => n + m.positions.length, 0);
    const ni = this.meshes.reduce((n, m) => n + m.indexes.length, 0);
    const ofsMesh = C_HEADER, ofsVert = ofsMesh + this.meshes.length * C_MESH, ofsIdx = ofsVert + nv * C_VERT;
    const b = Buffer.alloc(ofsIdx + ni * 4);
    b.write('OCOL', 0, 'latin1');
    b.writeInt32LE(COLLISION_VERSION, 4);
    b.writeInt32LE(C_HEADER, 8);
    b.writeInt32LE(0, 12);
    b.writeInt32LE(this.meshes.length, 16); b.writeInt32LE(ofsMesh, 20); b.writeInt32LE(C_MESH, 24);
    b.writeInt32LE(nv, 28); b.writeInt32LE(ofsVert, 32); b.writeInt32LE(C_VERT, 36);
    b.writeInt32LE(ni, 40); b.writeInt32LE(ofsIdx, 44);
    let v = 0, ix = 0;
    this.meshes.forEach((m, i) => {
      const p = ofsMesh + i * C_MESH;
      b.writeInt32LE(m.contents, p);
      b.writeInt32LE(m.surfaceFlags, p + 4);
      b.writeFloatLE(m.thickness, p + 8);
      b.writeInt32LE(0, p + 12);
      b.writeInt32LE(v, p + 16); b.writeInt32LE(m.positions.length, p + 20);
      b.writeInt32LE(ix, p + 24); b.writeInt32LE(m.indexes.length, p + 28);
      b.writeInt32LE(m.sourceId, p + 32);
      m.positions.forEach((pos, k) => pos.forEach((c, j) => b.writeFloatLE(c, ofsVert + (v + k) * C_VERT + j * 4)));
      m.indexes.forEach((x, k) => b.writeInt32LE(x, ofsIdx + (ix + k) * 4));
      v += m.positions.length;
      ix += m.indexes.length;
    });
    return b;
  }
}

export function readCollision(buf) {
  if (buf.toString('latin1', 0, 4) !== 'OCOL') throw new Error('not an OCOL lump');
  const version = buf.readInt32LE(4);
  if (version !== COLLISION_VERSION) throw new Error(`OAX_COLLISION version ${version}`);
  const nm = buf.readInt32LE(16), om = buf.readInt32LE(20), sm = buf.readInt32LE(24);
  const ov = buf.readInt32LE(32), sv = buf.readInt32LE(36), oi = buf.readInt32LE(44);
  const meshes = [];
  for (let i = 0; i < nm; i++) {
    const p = om + i * sm;
    const m = {
      contents: buf.readInt32LE(p), surfaceFlags: buf.readInt32LE(p + 4), thickness: buf.readFloatLE(p + 8),
      firstVert: buf.readInt32LE(p + 16), numVerts: buf.readInt32LE(p + 20), firstIndex: buf.readInt32LE(p + 24), numIndexes: buf.readInt32LE(p + 28),
      sourceId: buf.readInt32LE(p + 32),
    };
    m.positions = [];
    for (let k = 0; k < m.numVerts; k++) { const q = ov + (m.firstVert + k) * sv; m.positions.push([buf.readFloatLE(q), buf.readFloatLE(q + 4), buf.readFloatLE(q + 8)]); }
    m.indexes = [];
    for (let k = 0; k < m.numIndexes; k++) m.indexes.push(buf.readInt32LE(oi + (m.firstIndex + k) * 4));
    meshes.push(m);
  }
  return { version, meshes };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const [cmd, file] = process.argv.slice(2);
  if (cmd !== 'dump' || !file) {
    console.error('usage: oax-surfaces.mjs dump <map.bsp>');
    process.exit(2);
  }
  const { surfaces, collision } = surfacesFromBsp(fs.readFileSync(file));
  if (!surfaces) console.log('OAX_SURFACES: none');
  else {
    const flags = {};
    for (const s of surfaces.surfaces) for (const [k, b] of Object.entries(OSF)) if (s.flags & b) flags[k] = (flags[k] || 0) + 1;
    const tris = surfaces.surfaces.reduce((n, s) => n + s.triangles.length, 0);
    console.log(`OAX_SURFACES v${surfaces.version}: ${surfaces.surfaces.length} surfaces, ${tris} triangles, ${surfaces.verts.length} vertices, ${surfaces.materials.length} materials, ${surfaces.leafRefs.length} leaf refs`);
    console.log(`  flags: ${Object.entries(flags).map(([k, n]) => `${k} ${n}`).join(', ') || 'none'}`);
    console.log(`  materials: ${surfaces.materials.join(', ')}`);
  }
  if (!collision) console.log('OAX_COLLISION: none');
  else console.log(`OAX_COLLISION v${collision.version}: ${collision.meshes.length} meshes, ${collision.meshes.reduce((n, m) => n + m.indexes.length / 3, 0)} triangles`);
}
