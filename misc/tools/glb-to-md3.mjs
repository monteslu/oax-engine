#!/usr/bin/env node
// glb-to-md3: convert a static glTF binary (.glb) model to an MD3 for the
// engine, with its base-colour textures written next to it.
//
//   node misc/tools/glb-to-md3.mjs in.glb <outRoot> <models/dir/name.md3> [--height 256] [--textures textures/dir]
//     [--texname <base>] [--scale <units per model unit> --offset "x y z"] [--only <regex>] [--exclude <regex>] [--rotz "<regex>=<degrees>"]
//
// --scale/--offset place the model exactly (engine units, after the axis
// change) instead of fitting it to --height; --only/--exclude keep the
// nodes whose name (or mesh name) matches; --rotz turns matching nodes
// about the model's z axis (e.g. a turret posed sideways in the source).
//
// - Node transforms are applied (the default scene, or every root node).
// - glTF is Y-up, -Z forward; the engine is Z-up, X forward:
//   (x, y, z)_gltf -> (-z, -x, y), a proper rotation (no mirroring).
// - The model is scaled to --height units tall (default 256) with its base at
//   z 0, centred on x/y 0. MD3 stores positions as int16 / 64, so a model is
//   limited to +-512 units; foliage renders scale it to the instance size.
// - One MD3 surface per primitive, split so no surface has more than 999
//   vertices (the renderer's per-surface limit).
// - A material's base colour texture is written as <textures>/<name>_<n>.<png|jpg>
//   and named as the surface shader (the renderer finds the file by name).
//   Untextured materials get a 4x4 image of their base colour factor.
//
// Animation, skins and morph targets are ignored.

import fs from 'node:fs';
import path from 'node:path';

const MAX_VERTS = 999;

function readGlb(file) {
  const b = fs.readFileSync(file);
  if (b.toString('latin1', 0, 4) !== 'glTF') throw new Error(`${file}: not a glb`);
  let o = 12, json = null, bin = null;
  while (o < b.length) {
    const len = b.readUInt32LE(o), type = b.readUInt32LE(o + 4);
    const data = b.subarray(o + 8, o + 8 + len);
    if (type === 0x4e4f534a) json = JSON.parse(data.toString('utf8'));
    else if (type === 0x004e4942) bin = data;
    o += 8 + len;
  }
  return { json, bin };
}

const COMP = { 5120: [Int8Array, 1], 5121: [Uint8Array, 1], 5122: [Int16Array, 2], 5123: [Uint16Array, 2], 5125: [Uint32Array, 4], 5126: [Float32Array, 4] };
const NCOMP = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };

function accessor(g, bin, i) {
  const a = g.accessors[i];
  const bv = g.bufferViews[a.bufferView];
  const [T, size] = COMP[a.componentType];
  const n = NCOMP[a.type];
  const stride = bv.byteStride || size * n;
  const base = (bv.byteOffset || 0) + (a.byteOffset || 0);
  const out = [];
  const view = new DataView(bin.buffer, bin.byteOffset, bin.byteLength);
  const rd = { 5120: 'getInt8', 5121: 'getUint8', 5122: 'getInt16', 5123: 'getUint16', 5125: 'getUint32', 5126: 'getFloat32' }[a.componentType];
  for (let k = 0; k < a.count; k++) {
    const v = [];
    for (let c = 0; c < n; c++) {
      let x = view[rd](base + k * stride + c * size, true);
      if (a.normalized) x = a.componentType === 5121 ? x / 255 : a.componentType === 5123 ? x / 65535 : x;
      v.push(x);
    }
    out.push(n === 1 ? v[0] : v);
  }
  void T;
  return out;
}

// ---- matrices (column-major, as glTF) -----------------------------------------
const ident = () => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
function mul(a, b) {
  const r = new Array(16).fill(0);
  for (let c = 0; c < 4; c++) for (let rr = 0; rr < 4; rr++) for (let k = 0; k < 4; k++) r[c * 4 + rr] += a[k * 4 + rr] * b[c * 4 + k];
  return r;
}
function trs(n) {
  if (n.matrix) return n.matrix.slice();
  const [tx, ty, tz] = n.translation || [0, 0, 0];
  const [qx, qy, qz, qw] = n.rotation || [0, 0, 0, 1];
  const [sx, sy, sz] = n.scale || [1, 1, 1];
  const r = [
    1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw), 0,
    2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw), 0,
    2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy), 0,
    0, 0, 0, 1];
  for (let k = 0; k < 3; k++) { r[k] *= sx; r[4 + k] *= sy; r[8 + k] *= sz; }
  r[12] = tx; r[13] = ty; r[14] = tz;
  return r;
}
const xfP = (m, [x, y, z]) => [m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]];
const xfN = (m, [x, y, z]) => { const v = [m[0] * x + m[4] * y + m[8] * z, m[1] * x + m[5] * y + m[9] * z, m[2] * x + m[6] * y + m[10] * z]; const l = Math.hypot(...v) || 1; return v.map((c) => c / l); };
const toQuake = ([x, y, z]) => [-z, -x, y];

// ---- MD3 ------------------------------------------------------------------
function cstr(b, o, s, n) { b.fill(0, o, o + n); b.write(s.slice(0, n - 1), o, 'latin1'); }
function encodeNormal([x, y, z]) {
  if (x === 0 && y === 0) return z > 0 ? 0 : 128 << 8;
  let lng = Math.round(Math.atan2(y, x) * 255 / (2 * Math.PI)) & 0xff;
  let lat = Math.round(Math.acos(Math.max(-1, Math.min(1, z))) * 255 / (2 * Math.PI)) & 0xff;
  return (lat << 8) | lng;
}
function writeMD3(name, surfaces) {
  const surfs = surfaces.map((m, i) => {
    const nv = m.v.length, nt = m.tri.length;
    const size = 108 + 68 + nt * 12 + nv * 8 + nv * 8;
    const b = Buffer.alloc(size);
    b.write('IDP3', 0, 'latin1');
    cstr(b, 4, `s${i}`, 64);
    let o = 68;
    const ofsShaders = 108, ofsTris = ofsShaders + 68, ofsSt = ofsTris + nt * 12, ofsXyz = ofsSt + nv * 8;
    for (const v of [0, 1, 1, nv, nt, ofsTris, ofsShaders, ofsSt, ofsXyz, size]) { b.writeInt32LE(v, o); o += 4; }
    cstr(b, ofsShaders, m.shader, 64);
    for (let k = 0; k < nt; k++) for (let j = 0; j < 3; j++) b.writeInt32LE(m.tri[k][j], ofsTris + k * 12 + j * 4);
    for (let k = 0; k < nv; k++) { b.writeFloatLE(m.st[k][0], ofsSt + k * 8); b.writeFloatLE(m.st[k][1], ofsSt + k * 8 + 4); }
    for (let k = 0; k < nv; k++) {
      for (let j = 0; j < 3; j++) b.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(m.v[k][j] * 64))), ofsXyz + k * 8 + j * 2);
      b.writeUInt16LE(encodeNormal(m.n[k]), ofsXyz + k * 8 + 6);
    }
    return b;
  });
  const mins = [Infinity, Infinity, Infinity], maxs = [-Infinity, -Infinity, -Infinity];
  let radius = 0;
  for (const m of surfaces) for (const v of m.v) { for (let j = 0; j < 3; j++) { mins[j] = Math.min(mins[j], v[j]); maxs[j] = Math.max(maxs[j], v[j]); } radius = Math.max(radius, Math.hypot(...v)); }
  const head = Buffer.alloc(108), frame = Buffer.alloc(56);
  head.write('IDP3', 0, 'latin1');
  head.writeInt32LE(15, 4);
  cstr(head, 8, name, 64);
  const ofsFrames = 108, ofsSurfaces = ofsFrames + 56;
  const end = ofsSurfaces + surfs.reduce((a, s) => a + s.length, 0);
  let o = 72;
  for (const v of [0, 1, 0, surfs.length, 0, ofsFrames, ofsSurfaces, ofsSurfaces, end]) { head.writeInt32LE(v, o); o += 4; }
  mins.forEach((v, j) => frame.writeFloatLE(v, j * 4));
  maxs.forEach((v, j) => frame.writeFloatLE(v, 12 + j * 4));
  frame.writeFloatLE(radius, 36);
  cstr(frame, 40, 'frame0', 16);
  return Buffer.concat([head, frame, ...surfs]);
}

// split a triangle list so no piece has more than MAX_VERTS vertices
function split(prim) {
  const out = [];
  let cur = null, map = null;
  const start = () => { cur = { v: [], n: [], st: [], tri: [], shader: prim.shader }; map = new Map(); out.push(cur); };
  start();
  for (const t of prim.tri) {
    const need = t.filter((i) => !map.has(i)).length;
    if (cur.v.length + need > MAX_VERTS) start();
    cur.tri.push(t.map((i) => {
      if (!map.has(i)) { map.set(i, cur.v.length); cur.v.push(prim.v[i]); cur.n.push(prim.n[i]); cur.st.push(prim.st[i]); }
      return map.get(i);
    }));
  }
  return out.filter((s) => s.tri.length);
}

export function convert(inFile, outRoot, modelPath, { height = 256, texDir, texBase, scale, offset = [0, 0, 0], only, exclude, rotz } = {}) {
  const { json: g, bin } = readGlb(inFile);
  const base = path.basename(modelPath, '.md3');
  const tbase = texBase || base;
  texDir = texDir || path.dirname(modelPath);
  const shaders = [];
  const byImage = new Map();   // materials sharing an image share one texture
  // materials -> textures
  (g.materials || []).forEach((mat, mi) => {
    const pbr = mat.pbrMetallicRoughness || {};
    if (pbr.baseColorTexture) {
      const src = g.textures[pbr.baseColorTexture.index].source;
      if (byImage.has(src)) { shaders[mi] = byImage.get(src); return; }
      byImage.set(src, `${texDir}/${tbase}_${mi}`);
    }
    const shader = `${texDir}/${tbase}_${mi}`;
    fs.mkdirSync(path.join(outRoot, texDir), { recursive: true });
    if (pbr.baseColorTexture) {
      const img = g.images[g.textures[pbr.baseColorTexture.index].source];
      let data, ext;
      if (img.bufferView !== undefined) {
        const bv = g.bufferViews[img.bufferView];
        data = bin.subarray(bv.byteOffset || 0, (bv.byteOffset || 0) + bv.byteLength);
        ext = img.mimeType === 'image/jpeg' ? 'jpg' : 'png';
      } else {
        const src = path.join(path.dirname(inFile), decodeURIComponent(img.uri));
        data = fs.readFileSync(src);
        ext = path.extname(src).slice(1).toLowerCase() === 'jpg' || path.extname(src).slice(1).toLowerCase() === 'jpeg' ? 'jpg' : 'png';
      }
      fs.writeFileSync(path.join(outRoot, `${shader}.${ext}`), data);
    } else {
      // a flat colour: a tiny TGA of the base colour factor
      // glTF colour factors are linear; images are sRGB
      const srgb = (c) => (c <= 0.0031308 ? 12.92 * c : 1.055 * c ** (1 / 2.4) - 0.055);
      const [r, gg, bb, a] = (pbr.baseColorFactor || [0.8, 0.8, 0.8, 1]).map((c, k) => Math.round(Math.max(0, Math.min(1, k < 3 ? srgb(c) : c)) * 255));
      const t = Buffer.alloc(18 + 16 * 4);
      t[2] = 2; t.writeUInt16LE(4, 12); t.writeUInt16LE(4, 14); t[16] = 32; t[17] = 8;
      for (let k = 0; k < 16; k++) { t[18 + k * 4] = bb; t[19 + k * 4] = gg; t[20 + k * 4] = r; t[21 + k * 4] = a; }
      fs.writeFileSync(path.join(outRoot, `${shader}.tga`), t);
    }
    shaders[mi] = shader;
  });
  // walk the scene
  const prims = [];
  const named = (n) => [n.name || '', n.mesh !== undefined ? g.meshes[n.mesh].name || '' : ''];
  const visit = (ni, parent, inherited = null) => {
    const n = g.nodes[ni];
    const m = mul(parent, trs(n));
    const names = named(n);
    const match = (re) => re && names.some((x) => re.test(x));
    const turn = rotz && match(rotz.re) ? rotz.deg : inherited;
    const keep = (!only || match(only)) && !match(exclude);
    if (n.mesh !== undefined && keep) {
      for (const p of g.meshes[n.mesh].primitives) {
        if ((p.mode ?? 4) !== 4) continue;
        const rz = (v) => { if (turn == null) return v; const a = turn * Math.PI / 180, c = Math.cos(a), sn = Math.sin(a); return [v[0] * c - v[1] * sn, v[0] * sn + v[1] * c, v[2]]; };
        const pos = accessor(g, bin, p.attributes.POSITION).map((v) => rz(toQuake(xfP(m, v))));
        const nrm = p.attributes.NORMAL !== undefined ? accessor(g, bin, p.attributes.NORMAL).map((v) => rz(toQuake(xfN(m, v)))) : pos.map(() => [0, 0, 1]);
        const st = p.attributes.TEXCOORD_0 !== undefined ? accessor(g, bin, p.attributes.TEXCOORD_0) : pos.map(() => [0, 0]);
        const ind = p.indices !== undefined ? accessor(g, bin, p.indices) : pos.map((_, k) => k);
        const tri = [];
        // MD3 winding is the reverse of glTF's counter-clockwise front faces
        for (let k = 0; k + 2 < ind.length; k += 3) tri.push([ind[k], ind[k + 2], ind[k + 1]]);
        prims.push({ v: pos, n: nrm, st, tri, shader: shaders[p.material] ?? `${texDir}/${base}_none` });
      }
    }
    for (const c of n.children || []) visit(c, m, turn);
  };
  const scene = g.scenes?.[g.scene ?? 0];
  const roots = scene ? scene.nodes : g.nodes.map((_, i) => i).filter((i) => !g.nodes.some((n) => (n.children || []).includes(i)));
  for (const r of roots) visit(r, ident());
  // fit: base at z 0, centred, height units tall
  const mins = [Infinity, Infinity, Infinity], maxs = [-Infinity, -Infinity, -Infinity];
  for (const p of prims) for (const v of p.v) for (let j = 0; j < 3; j++) { mins[j] = Math.min(mins[j], v[j]); maxs[j] = Math.max(maxs[j], v[j]); }
  if (!prims.length) throw new Error('no meshes selected');
  let s, place;
  if (scale) {
    s = scale;
    place = ([x, y, z]) => [x * s + offset[0], y * s + offset[1], z * s + offset[2]];
  } else {
    s = height / (maxs[2] - mins[2]);
    const cx = (mins[0] + maxs[0]) / 2, cy = (mins[1] + maxs[1]) / 2;
    place = ([x, y, z]) => [(x - cx) * s, (y - cy) * s, (z - mins[2]) * s];
  }
  for (const p of prims) p.v = p.v.map(place);
  const surfaces = prims.flatMap(split);
  const md3 = writeMD3(modelPath, surfaces);
  fs.mkdirSync(path.join(outRoot, path.dirname(modelPath)), { recursive: true });
  fs.writeFileSync(path.join(outRoot, modelPath), md3);
  const tris = surfaces.reduce((a, x) => a + x.tri.length, 0);
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (const x of surfaces) for (const v of x.v) for (let j = 0; j < 3; j++) { lo[j] = Math.min(lo[j], v[j]); hi[j] = Math.max(hi[j], v[j]); }
  return { surfaces: surfaces.length, tris, bounds: [lo, hi], size: hi.map((h, j) => h - lo[j]), shaders: [...new Set(surfaces.map((x) => x.shader))] };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const [inFile, outRoot, modelPath, ...rest] = process.argv.slice(2);
  if (!modelPath) { console.error('usage: glb-to-md3.mjs in.glb <outRoot> <models/dir/name.md3> [--height 256] [--textures textures/dir]'); process.exit(2); }
  const opt = (k) => { const i = rest.indexOf(`--${k}`); return i >= 0 ? rest[i + 1] : undefined; };
  const rz = opt('rotz');
  const r = convert(inFile, outRoot, modelPath, {
    height: Number(opt('height') || 256), texDir: opt('textures'), texBase: opt('texname'),
    scale: opt('scale') ? Number(opt('scale')) : undefined,
    offset: opt('offset') ? opt('offset').trim().split(/\s+/).map(Number) : undefined,
    only: opt('only') ? new RegExp(opt('only')) : undefined,
    exclude: opt('exclude') ? new RegExp(opt('exclude')) : undefined,
    rotz: rz ? { re: new RegExp(rz.split('=')[0]), deg: Number(rz.split('=')[1]) } : undefined,
  });
  console.log(`${modelPath}: ${r.surfaces} surfaces, ${r.tris} triangles, bounds ${r.bounds.map((b) => b.map((v) => v.toFixed(0)).join(' ')).join(' .. ')}; textures ${r.shaders.join(', ')}`);
}
