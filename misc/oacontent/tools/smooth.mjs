// T7 smoothing and tessellation: for each map, how faceted its curves are and
// the sidecar keys to fix it (oax_smoothnormals, oax_subdivisions; docs/map-format.md).
//
//   oacontent smooth [--maps a,b] [--baseoa dir] [--out dir] [--angle 50]
// Output: <out>/smooth/<map>.oaxmap (worldspawn keys), report.{json,txt}.
//
// The faceted-curve detector joins world faces that share an edge with a crease
// of between 6 and 45 degrees (a polygonal arch, a twelve-sided pillar) into
// clusters; a cluster of four or more faces is a curve built from flats.
// The smoothing angle is the sidecar's cap on how big a crease still shades
// smooth (45 degrees by default plus margin: the 90 degree corners of walls
// and floors stay hard). Tessellation compares the vertex counts of the map's
// patches at the stock r_subdivisions (4) and at 2.
import fs from 'node:fs';
import path from 'node:path';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp, MST, SURF } from '../lib/bsp.mjs';
import { loadShaders } from '../lib/shader.mjs';
import { OUT } from '../lib/common.mjs';

const MIN_CREASE = 6, MAX_CREASE = 45;

function faceNormal(bsp, s) {
  const N = bsp.verts.normal;
  const n = [0, 0, 0];
  for (let v = 0; v < s.numVerts; v++) for (let a = 0; a < 3; a++) n[a] += N[(s.firstVert + v) * 3 + a];
  const l = Math.hypot(...n) || 1;
  return n.map((x) => x / l);
}

export function analyze(bsp, shaders) {
  const faces = [];
  for (let i = 0; i < bsp.surfaces.length; i++) {
    const s = bsp.surfaces[i];
    if (s.type !== MST.PLANAR) continue;
    const sh = bsp.shaders[s.shader];
    const def = shaders.byName.get(sh.name.toLowerCase());
    if ((sh.surfaceFlags & (SURF.SKY | SURF.NODRAW)) || def?.sky || def?.nodraw) continue;
    faces.push({ i, s, n: faceNormal(bsp, s) });
  }
  // shared positions -> the faces meeting there
  const q = (x) => Math.round(x * 8);
  const at = new Map();
  const V = bsp.verts.xyz;
  faces.forEach((f, fi) => {
    for (let v = 0; v < f.s.numVerts; v++) {
      const k = `${q(V[(f.s.firstVert + v) * 3])},${q(V[(f.s.firstVert + v) * 3 + 1])},${q(V[(f.s.firstVert + v) * 3 + 2])}`;
      let l = at.get(k); if (!l) at.set(k, l = new Set()); l.add(fi);
    }
  });
  // union-find over faces that meet with a smoothable crease
  const parent = faces.map((_, i) => i);
  const find = (x) => { while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
  const hist = new Array(10).fill(0);       // creases by 10 degrees
  let smoothable = 0, positions = 0;
  for (const set of at.values()) {
    if (set.size < 2) continue;
    positions++;
    const l = [...set];
    let any = false;
    for (let a = 0; a < l.length; a++) for (let b = a + 1; b < l.length; b++) {
      const cos = Math.max(-1, Math.min(1, faces[l[a]].n[0] * faces[l[b]].n[0] + faces[l[a]].n[1] * faces[l[b]].n[1] + faces[l[a]].n[2] * faces[l[b]].n[2]));
      const deg = (Math.acos(cos) * 180) / Math.PI;
      hist[Math.min(9, Math.floor(deg / 10))]++;
      if (deg >= MIN_CREASE && deg <= MAX_CREASE) { any = true; parent[find(l[a])] = find(l[b]); }
    }
    if (any) smoothable++;
  }
  const clusters = new Map();
  faces.forEach((f, fi) => { const r = find(fi); (clusters.get(r) || clusters.set(r, []).get(r)).push(fi); });
  const curves = [];
  for (const members of clusters.values()) {
    if (members.length < 4) continue;
    const mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9];
    for (const fi of members) { const f = faces[fi]; for (let v = 0; v < f.s.numVerts; v++) for (let a = 0; a < 3; a++) { const x = V[(f.s.firstVert + v) * 3 + a]; if (x < mn[a]) mn[a] = x; if (x > mx[a]) mx[a] = x; } }
    curves.push({ faces: members.length, mins: mn.map(Math.round), maxs: mx.map(Math.round), shader: bsp.shaders[faces[members[0]].s.shader].name });
  }
  curves.sort((a, b) => b.faces - a.faces);
  // patches: control polygon lengths per direction -> tessellated vertex estimate
  let patches = 0, vAt4 = 0, vAt2 = 0;
  const P = (i) => [V[i * 3], V[i * 3 + 1], V[i * 3 + 2]];
  for (const s of bsp.surfaces) {
    if (s.type !== MST.PATCH) continue;
    patches++;
    const w = s.patchWidth, h = s.patchHeight;
    let lu = 0, lv = 0;
    for (let y = 0; y < h; y++) for (let x = 0; x + 1 < w; x++) { const a = P(s.firstVert + y * w + x), b = P(s.firstVert + y * w + x + 1); lu = Math.max(lu, 0); lu += Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]) / h; }
    for (let x = 0; x < w; x++) for (let y = 0; y + 1 < h; y++) { const a = P(s.firstVert + y * w + x), b = P(s.firstVert + (y + 1) * w + x); lv += Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]) / w; }
    const verts = (sub) => (2 + Math.ceil(lu / sub)) * (2 + Math.ceil(lv / sub));
    vAt4 += verts(4); vAt2 += verts(2);
  }
  return { faces: faces.length, sharedPositions: positions, smoothablePositions: smoothable, smoothableFraction: positions ? +(smoothable / positions).toFixed(3) : 0, creaseHistogram: hist, curves: curves.slice(0, 8), curveClusters: curves.length, patches, patchVertsAt4: vAt4, patchVertsAt2: vAt2 };
}

export function suggest(a, { angle = 50 } = {}) {
  const keys = {};
  if (a.smoothableFraction >= 0.01 || a.curveClusters > 0) keys.oax_smoothnormals = angle;
  if (a.patches > 0 && a.patchVertsAt2 <= 120000 && a.patchVertsAt2 > a.patchVertsAt4 * 1.2) keys.oax_subdivisions = 2;
  return keys;
}

export async function run(args) {
  const baseoa = args.baseoa || DEFAULT_BASEOA;
  const out = path.join(args.out || OUT, 'smooth');
  fs.mkdirSync(out, { recursive: true });
  const cs = new ContentSet([baseoa]);
  const shaders = loadShaders(cs.shaderFiles());
  const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;
  const rows = [];
  for (const m of cs.maps()) {
    if (only && !only.has(m.name)) continue;
    try {
      const bsp = new Bsp(cs.read(m.path), m.path);
      const a = analyze(bsp, shaders);
      const keys = suggest(a, { angle: Number(args.angle) || 50 });
      if (Object.keys(keys).length) fs.writeFileSync(path.join(out, `${m.name}.oaxmap`), `// oacontent smooth: ${m.name}\n{\n"classname" "worldspawn"\n${Object.entries(keys).map(([k, v]) => `"${k}" "${v}"`).join('\n')}\n}\n`);
      rows.push({ name: m.name, ...a, keys });
    } catch (e) { rows.push({ name: m.name, error: e.message }); }
  }
  fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(rows, null, 1) + '\n');
  const lines = ['map              faces  creases(6-45deg) curves(>=4 faces)  patches  verts@4  verts@2  keys'];
  for (const r of rows) lines.push(r.error ? `${r.name.padEnd(16)} ERROR ${r.error}` : `${r.name.padEnd(16)} ${String(r.faces).padStart(5)}  ${String(r.smoothableFraction).padStart(8)}  ${String(r.curveClusters).padStart(14)}  ${String(r.patches).padStart(8)} ${String(r.patchVertsAt4).padStart(8)} ${String(r.patchVertsAt2).padStart(8)}  ${Object.entries(r.keys).map(([k, v]) => `${k.replace('oax_', '')}=${v}`).join(' ')}`);
  fs.writeFileSync(path.join(out, 'report.txt'), lines.join('\n') + '\n');
  console.log(lines.join('\n'));
  return 0;
}
