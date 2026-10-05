#!/usr/bin/env node
// obj-to-md3: convert a static Wavefront OBJ model to an MD3 for the engine,
// one MD3 surface per OBJ material.
//
//   node misc/tools/obj-to-md3.mjs in.obj out.md3 --name models/dir/name.md3
//     --shader <base> [--height 96 | --scale <units per model unit>]
//     [--rotz <degrees>] [--smooth]
//
// - OBJ (as Blender writes it) is Y-up, -Z forward; the engine is Z-up, X
//   forward: (x, y, z)_obj -> (-z, -x, y), a proper rotation (no mirroring),
//   then --rotz turns the model about z.
// - The model is scaled to --height units tall (or by --scale), its base at
//   z 0 and centred on x/y 0.
// - Each material's surface is named <base>_<material, lower case> for the
//   map's shaders to define; polygons are fanned into triangles, wound as
//   MD3 wants (the reverse of OBJ's counter-clockwise front faces).
// - Normals come from the file's vn, else the face's flat normal; --smooth
//   instead averages the face normals (area weighted) at each position,
//   across materials, so a faceted model shades and reflects smoothly.
//   Texture coordinates from vt, else 0.

import fs from 'node:fs';
import { writeMD3 } from '../../tests/maps/lib/vehicles.mjs';

function opt(name, def) {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}

const [inFile, outFile] = process.argv.slice(2);
const name = opt('name');
const base = opt('shader');
if (!inFile || !outFile || !name || !base) {
  console.error('usage: obj-to-md3 in.obj out.md3 --name models/dir/name.md3 --shader <base> [--height 96 | --scale s] [--rotz deg]');
  process.exit(2);
}

const toQuake = ([x, y, z]) => [-z, -x, y];
const rot = Number(opt('rotz', 0)) * Math.PI / 180;
const rz = ([x, y, z]) => [x * Math.cos(rot) - y * Math.sin(rot), x * Math.sin(rot) + y * Math.cos(rot), z];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a) => { const l = Math.hypot(...a) || 1; return a.map((v) => v / l); };

const v = [], vt = [], vn = [];
const surfaces = new Map();	// material -> { v, n, st, tri }
let mtl = 'default';
for (const line of fs.readFileSync(inFile, 'utf8').split('\n')) {
  const p = line.trim().split(/\s+/);
  if (p[0] === 'v') v.push(rz(toQuake(p.slice(1, 4).map(Number))));
  else if (p[0] === 'vt') vt.push([Number(p[1]), 1 - Number(p[2])]);
  else if (p[0] === 'vn') vn.push(rz(toQuake(p.slice(1, 4).map(Number))));
  else if (p[0] === 'usemtl') mtl = p[1];
  else if (p[0] === 'f') {
    const s = surfaces.get(mtl) ?? surfaces.set(mtl, { v: [], n: [], st: [], tri: [] }).get(mtl);
    const corners = p.slice(1).map((c) => c.split('/').map((x) => (x ? Number(x) : 0)));
    const idx = (k, n) => (k < 0 ? n + k : k - 1);
    const pts = corners.map(([a]) => v[idx(a, v.length)]);
    const flat = norm(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0])));
    const first = s.v.length;
    corners.forEach(([a, t, n], k) => {
      s.v.push(pts[k]);
      s.st.push(t ? vt[idx(t, vt.length)] : [0, 0]);
      s.n.push(n ? vn[idx(n, vn.length)] : flat);
    });
    // a fan, wound clockwise from outside (MD3)
    for (let k = 1; k + 1 < corners.length; k++) s.tri.push([first, first + k + 1, first + k]);
  }
}

if (process.argv.includes('--smooth')) {
  const key = (p) => p.map((x) => x.toFixed(4)).join(',');
  const acc = new Map();
  for (const s of surfaces.values()) {
    for (const [a, b, c] of s.tri) {
      // the triangle's normal, area weighted (wound clockwise from outside)
      const n = cross(sub(s.v[c], s.v[a]), sub(s.v[b], s.v[a]));
      for (const k of [a, b, c]) {
        const t = acc.get(key(s.v[k])) ?? [0, 0, 0];
        acc.set(key(s.v[k]), [t[0] + n[0], t[1] + n[1], t[2] + n[2]]);
      }
    }
  }
  for (const s of surfaces.values()) s.n = s.v.map((p) => norm(acc.get(key(p))));
}

// placement: base at z 0, centred, to the height (or scale)
const all = [...surfaces.values()].flatMap((s) => s.v);
const mins = [0, 1, 2].map((j) => Math.min(...all.map((p) => p[j])));
const maxs = [0, 1, 2].map((j) => Math.max(...all.map((p) => p[j])));
const scale = opt('scale') ? Number(opt('scale')) : Number(opt('height', 96)) / (maxs[2] - mins[2]);
const cx = (mins[0] + maxs[0]) / 2, cy = (mins[1] + maxs[1]) / 2;
for (const s of surfaces.values()) s.v = s.v.map(([x, y, z]) => [(x - cx) * scale, (y - cy) * scale, (z - mins[2]) * scale]);

const meshes = [...surfaces.entries()].map(([m, s]) => ({ ...s, shader: `${base}_${m.toLowerCase()}` }));
fs.writeFileSync(outFile, writeMD3(name, meshes));
const size = [0, 1, 2].map((j) => ((maxs[j] - mins[j]) * scale).toFixed(0)).join(' x ');
console.log(`${name}: ${meshes.length} surfaces (${meshes.map((m) => m.shader).join(', ')}), ${meshes.reduce((a, m) => a + m.tri.length, 0)} triangles, ${size} units`);
