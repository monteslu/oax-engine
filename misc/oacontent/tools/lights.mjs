// T3 light recovery: lights for stock maps whose compiled BSPs carry none, as
// a maps/<name>.oaxmap sidecar (hybrid lighting: the map keeps its lightmaps and
// gains shadowed realtime lights, docs/lights.md).
//
// Sources, in priority order:
//   a. map sources (--mapsource <dir>): the original `light` entities of a
//      map's .map file, imported exactly as q3 profile lights;
//   b. surface lights: every surface whose shader has q3map_surfacelight
//      becomes point lights on a grid over the emitting area (colour from the
//      shader's light image), then each light's intensity is fitted by
//      non-negative least squares against the map's own baked lightmap, over
//      sampled texels with a line of sight to the light;
//   c. hot spots: bright baked texels the fitted lights leave unexplained get
//      a light each, placed by greedy search (--hotspots N caps it at N lights,
//      default 24; --no-hotspots turns it off): the stripped `light` entities
//      of the original map, as far as the lightmap shows them;
//   d. hand edits: data/lights/<map>.json (add / remove / scale), applied last.
//
//   oacontent lights [--maps a,b] [--baseoa dir] [--out dir] [--lmscale 0.5]
//                    [--mapsource dir] [--hotspots 24|--no-hotspots] [--max 64] [--texels 3000]
// Output: <out>/lights/<map>.oaxmap and <out>/lights/report.{json,txt}.
import fs from 'node:fs';
import path from 'node:path';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp, MST, SURF } from '../lib/bsp.mjs';
import { loadShaders } from '../lib/shader.mjs';
import { Entity, entity, parseEntities, serializeEntities } from '../lib/entities.mjs';
import { parseMapSource } from '../lib/map.mjs';
import { decodeRgba, meanColor } from '../lib/image.mjs';
import { OUT, REPO } from '../lib/common.mjs';

const Q3_POINTSCALE = 7500, Q3_MINDIST = 16;
const REF_LIGHT = 300;             // the reference intensity the fit scales
const lum = (r, g, b) => 0.2126 * r + 0.7152 * g + 0.0722 * b;

// ---- emitters ---------------------------------------------------------------

function shaderColor(cs, def, cache) {
  const key = def.name.toLowerCase();
  if (cache.has(key)) return cache.get(key);
  let color = [1, 1, 1];
  const tex = (def.lightImage ? [def.lightImage] : []).concat(def.textures);
  for (const t of tex) {
    const base = t.replace(/\.(tga|jpg|png)$/i, '');
    const f = ['tga', 'jpg', 'png'].map((e) => `${base}.${e}`).find((p) => cs.has(p));
    if (!f) continue;
    try { color = meanColor(decodeRgba(cs.read(f), f)); break; } catch { /* next */ }
  }
  cache.set(key, color);
  return color;
}

// point-light candidates over the emitting surfaces: one per grid cell of
// `cell` units per shader, at the area-weighted centroid, lifted off the surface
export function emitters(bsp, shaders, cs, { cell = 160 } = {}) {
  const cache = new Map();
  const cells = new Map();
  const N = bsp.verts.normal;
  for (const s of bsp.surfaces) {
    if (s.type !== MST.PLANAR && s.type !== MST.TRIANGLE_SOUP && s.type !== MST.PATCH) continue;
    const sh = bsp.shaders[s.shader];
    const def = shaders.byName.get(sh.name.toLowerCase());
    if (!def || !(def.surfaceLight > 0) || def.sky) continue;
    const n = [0, 0, 0];
    for (let v = 0; v < s.numVerts; v++) for (let a = 0; a < 3; a++) n[a] += N[(s.firstVert + v) * 3 + a];
    const nl = Math.hypot(...n) || 1; for (let a = 0; a < 3; a++) n[a] /= nl;
    for (const t of bsp.surfaceTriangles(s)) {
      const e1 = [t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]], e2 = [t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]];
      const area = Math.hypot(e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]) / 2;
      if (area < 1) continue;
      const c = [(t[0][0] + t[1][0] + t[2][0]) / 3, (t[0][1] + t[1][1] + t[2][1]) / 3, (t[0][2] + t[1][2] + t[2][2]) / 3];
      const key = `${def.name.toLowerCase()}|${Math.floor(c[0] / cell)}|${Math.floor(c[1] / cell)}|${Math.floor(c[2] / cell)}`;
      let e = cells.get(key);
      if (!e) { e = { shader: def.name, surfaceLight: def.surfaceLight, area: 0, sum: [0, 0, 0], n: [0, 0, 0], color: shaderColor(cs, def, cache) }; cells.set(key, e); }
      e.area += area;
      for (let a = 0; a < 3; a++) { e.sum[a] += c[a] * area; e.n[a] += n[a] * area; }
    }
  }
  const out = [];
  for (const e of cells.values()) {
    const nl = Math.hypot(...e.n) || 1;
    const normal = e.n.map((x) => x / nl);
    const centroid = e.sum.map((x) => x / e.area);
    out.push({ shader: e.shader, surfaceLight: e.surfaceLight, area: e.area, color: e.color, normal, centroid, origin: centroid.slice() });
  }
  return out;
}

// the origin lifted off the emitting surface along its normal while that stays
// in open space (a light in solid lights nothing)
function liftOrigins(bsp, ems) {
  for (const e of ems) {
    for (const lift of [16, 8, 24, 4, 0]) {
      const p = e.centroid.map((x, a) => x + e.normal[a] * lift);
      const leaf = bsp.leafs[bsp.pointLeaf(p)];
      if (leaf && leaf.cluster >= 0) { e.origin = p; e.lift = lift; break; }
    }
  }
  return ems.filter((e) => e.lift !== undefined);
}

// merge the weakest emitters into their nearest neighbour until at most `max` remain
function limit(ems, max) {
  const list = ems.slice();
  while (list.length > max) {
    list.sort((a, b) => a.area * a.surfaceLight - b.area * b.surfaceLight);
    const w = list.shift();
    let best = 0, bd = Infinity;
    for (let i = 0; i < list.length; i++) { const d = Math.hypot(...list[i].origin.map((x, a) => x - w.origin[a])); if (d < bd) { bd = d; best = i; } }
    const t = list[best], wa = w.area * w.surfaceLight, ta = t.area * t.surfaceLight;
    t.origin = t.origin.map((x, a) => (x * ta + w.origin[a] * wa) / (ta + wa));
    t.centroid = t.origin;
    t.area += w.area;
  }
  return list;
}

// ---- the bake: lightmap texels with world positions ---------------------------

export function lightmapTexels(bsp, target = 4000) {
  const V = bsp.verts, idx = bsp.indexes;
  // first pass: how many texels in all, for the stride
  let total = 0;
  const tris = [];
  for (const s of bsp.surfaces) {
    if ((s.type !== MST.PLANAR && s.type !== MST.TRIANGLE_SOUP) || s.lightmap < 0 || s.lightmap >= bsp.numLightmaps) continue;
    for (let t = 0; t + 2 < s.numIndexes; t += 3) {
      const i = [0, 1, 2].map((k) => idx[s.firstIndex + t + k] + s.firstVert);
      const uv = i.map((v) => [V.lm[v * 2] * 128, V.lm[v * 2 + 1] * 128]);
      const a2 = Math.abs((uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1]) - (uv[2][0] - uv[0][0]) * (uv[1][1] - uv[0][1]));
      total += a2 / 2;
      tris.push({ page: s.lightmap, i, uv });
    }
  }
  const stride = Math.max(1, Math.round(total / target));
  const out = [];
  let counter = 0;
  const page = new Map();
  for (const tr of tris) {
    const [a, b, c] = tr.uv;
    const minx = Math.floor(Math.min(a[0], b[0], c[0])), maxx = Math.ceil(Math.max(a[0], b[0], c[0]));
    const miny = Math.floor(Math.min(a[1], b[1], c[1])), maxy = Math.ceil(Math.max(a[1], b[1], c[1]));
    const den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
    if (Math.abs(den) < 1e-9) continue;
    for (let py = Math.max(0, miny); py < Math.min(128, maxy); py++) for (let px = Math.max(0, minx); px < Math.min(128, maxx); px++) {
      const x = px + 0.5, y = py + 0.5;
      const l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / den;
      const l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / den;
      const l3 = 1 - l1 - l2;
      if (l1 < -0.05 || l2 < -0.05 || l3 < -0.05) continue;
      if (counter++ % stride) continue;
      let pg = page.get(tr.page); if (!pg) { pg = bsp.lightmapPage(tr.page); page.set(tr.page, pg); }
      const o = (py * 128 + px) * 3;
      const pos = [0, 1, 2].map((k) => l1 * V.xyz[tr.i[0] * 3 + k] + l2 * V.xyz[tr.i[1] * 3 + k] + l3 * V.xyz[tr.i[2] * 3 + k]);
      const nrm = [0, 1, 2].map((k) => l1 * V.normal[tr.i[0] * 3 + k] + l2 * V.normal[tr.i[1] * 3 + k] + l3 * V.normal[tr.i[2] * 3 + k]);
      const nl = Math.hypot(...nrm) || 1;
      out.push({ pos, normal: nrm.map((v) => v / nl), value: lum(pg[o], pg[o + 1], pg[o + 2]) / 255, rgb: [pg[o] / 255, pg[o + 1] / 255, pg[o + 2] / 255] });
    }
  }
  return out;
}

// one emitter's contribution at a texel under the q3 point light (inverse
// square, Lambert) at reference intensity, in lightmap units (1 = texture at 1x)
function reference(em, tx) {
  const d = [em.origin[0] - tx.pos[0], em.origin[1] - tx.pos[1], em.origin[2] - tx.pos[2]];
  const dist = Math.hypot(...d);
  const photons = REF_LIGHT * Q3_POINTSCALE;
  if (dist > Math.sqrt(photons)) return 0;
  const nl = Math.max(0, (d[0] * tx.normal[0] + d[1] * tx.normal[1] + d[2] * tx.normal[2]) / (dist || 1));
  return (photons * nl) / (Math.max(dist, Q3_MINDIST) ** 2) / 255;
}

// A least squares fit of realtime lights to the baked lightmap: columns of
// reference contributions (the q3 point light at REF_LIGHT, with a line of
// sight test), non-negative scales per light and a constant ambient term:
//   target ~ a + sum k_i g_i
export class Fit {
  constructor(bsp, texels, targetScale) {
    this.bsp = bsp; this.texels = texels;
    this.n = texels.length;
    this.y = Float64Array.from(texels, (t) => t.value * targetScale);
    this.cols = [];                 // Float32Array(n) per light
    this.k = []; this.ambient = 0;
  }

  // the column of a light at origin: reference contribution where the texel
  // sees it (a ray through the world brushes), 0 elsewhere
  column(origin) {
    const g = new Float32Array(this.n);
    const R2 = REF_LIGHT * Q3_POINTSCALE;
    for (let i = 0; i < this.n; i++) {
      const tx = this.texels[i];
      const ref = reference({ origin }, tx);
      if (ref < 0.002) continue;
      const o = [tx.pos[0] + tx.normal[0] * 2, tx.pos[1] + tx.normal[1] * 2, tx.pos[2] + tx.normal[2] * 2];
      const d = [origin[0] - o[0], origin[1] - o[1], origin[2] - o[2]];
      const dist = Math.hypot(...d);
      if (dist * dist > R2) continue;
      const hit = this.bsp.ray(o, d.map((x) => x / dist), dist - 3);
      if (!hit || hit.sky) g[i] = ref;
    }
    return g;
  }

  add(origin) { this.cols.push(this.column(origin)); this.k.push(0); return this.cols.length - 1; }

  solve() {
    const m = this.cols.length, n = this.n;
    const A = Array.from({ length: m + 1 }, () => new Float64Array(m + 1)), bv = new Float64Array(m + 1);
    const col = (j, i) => (j === m ? 1 : this.cols[j][i]);
    for (let i = 0; i < n; i++) {
      const nz = [];
      for (let j = 0; j <= m; j++) if (col(j, i)) nz.push(j);
      for (const j of nz) { bv[j] += col(j, i) * this.y[i]; for (const k of nz) A[j][k] += col(j, i) * col(k, i); }
    }
    const x = new Float64Array(m + 1);
    for (let j = 0; j < m; j++) x[j] = this.k[j] || 0;
    x[m] = this.ambient;
    for (let it = 0; it < 300; it++) {
      for (let j = 0; j <= m; j++) {
        if (A[j][j] < 1e-12) continue;
        let r = bv[j]; for (let k = 0; k <= m; k++) if (k !== j) r -= A[j][k] * x[k];
        x[j] = Math.max(0, r / A[j][j]);
      }
    }
    this.k = Array.from(x.slice(0, m)); this.ambient = x[m];
    return this.stats();
  }

  residual() {
    const r = new Float64Array(this.n);
    for (let i = 0; i < this.n; i++) { let p = this.ambient; for (let j = 0; j < this.cols.length; j++) p += this.cols[j][i] * this.k[j]; r[i] = this.y[i] - p; }
    return r;
  }

  stats() {
    const r = this.residual();
    let ssRes = 0, ssTot = 0; const mean = this.y.reduce((a, b) => a + b, 0) / Math.max(1, this.n);
    for (let i = 0; i < this.n; i++) { ssRes += r[i] * r[i]; ssTot += (this.y[i] - mean) ** 2; }
    return { r2: ssTot > 0 ? 1 - ssRes / ssTot : 0, ambient: this.ambient, meanTarget: mean, sse: ssRes };
  }

  // Greedy light placement for what the lights so far leave unexplained: up
  // to maxAdd lights, each the best of `pool` candidates (positions above the
  // brightest unexplained texels, lifted along the surface normal to a height
  // the room allows), kept while it removes at least minGain of the error.
  // Returns [{ origin, light, color }].
  greedy({ maxAdd = 24, pool = 36, minGain = 0.004, spacing = 96 } = {}) {
    const added = [];
    for (let round = 0; round < maxAdd; round++) {
      const res = this.residual();
      const order = Array.from(res.keys()).filter((i) => res[i] > 0.03).sort((a, b) => res[b] - res[a]);
      if (!order.length) break;
      const cands = [];
      for (const i of order) {
        const tx = this.texels[i];
        if (cands.some((c) => Math.hypot(c.tx.pos[0] - tx.pos[0], c.tx.pos[1] - tx.pos[1], c.tx.pos[2] - tx.pos[2]) < spacing)) continue;
        cands.push({ tx, i });
        if (cands.length >= pool) break;
      }
      let best = null;
      for (const c of cands) {
        const tx = c.tx;
        const up = this.bsp.ray([tx.pos[0] + tx.normal[0] * 4, tx.pos[1] + tx.normal[1] * 4, tx.pos[2] + tx.normal[2] * 4], tx.normal, 160);
        const h = Math.max(24, Math.min(96, (up ? up.t : 160) - 16));
        const origin = tx.pos.map((x, a) => x + tx.normal[a] * h);
        const leaf = this.bsp.leafs[this.bsp.pointLeaf(origin)];
        if (!leaf || leaf.cluster < 0) continue;
        const g = this.column(origin);
        let num = 0, den = 0;
        for (let i = 0; i < this.n; i++) if (g[i]) { num += res[i] * g[i]; den += g[i] * g[i]; }
        if (den <= 0 || num <= 0) continue;
        const gain = (num * num) / den;
        if (!best || gain > best.gain) best = { gain, origin, g, num, den };
      }
      const sse = this.stats().sse;
      if (!best || best.gain < minGain * sse) break;
      const idx = this.cols.length;
      this.cols.push(best.g); this.k.push(best.num / best.den);
      this.solve();
      // the light's colour: the baked colour of the texels it lights, weighted by what it explains
      const c = [0, 0, 0]; let w = 0;
      for (let i = 0; i < this.n; i++) { const v = best.g[i] * this.k[idx]; if (v > 0) { for (let a = 0; a < 3; a++) c[a] += this.texels[i].rgb[a] * v; w += v; } }
      const mx = Math.max(...c, 1e-6);
      added.push({ idx, origin: best.origin, color: c.map((x) => x / mx) });
    }
    return added.map((a) => ({ origin: a.origin, light: REF_LIGHT * this.k[a.idx], color: a.color, note: 'lightmap fit' }));
  }
}

// ---- the sidecar --------------------------------------------------------------

const fmt = (v) => String(+v.toFixed(3));
export function sidecar({ lmScale, lights, comment }) {
  const ws = entity('worldspawn', { oax_lighting: 'hybrid', oax_lightmapscale: lmScale, oax_shadowmode: 'maps' });
  const ents = [ws];
  for (const l of lights) {
    const e = entity('rtlight', { origin: l.origin.map(Math.round).join(' '), oax_profile: 'q3', light: Math.round(l.light), _color: l.color.map(fmt).join(' ') });
    if (l.note) e.set('_note', l.note);
    ents.push(e);
  }
  return `// ${comment}\n` + serializeEntities(ents);
}

function importMapSource(text) {
  const out = [];
  for (const e of parseMapSource(text)) {
    if (e.classname !== 'light' || !e.origin) continue;
    if (e.get('target') || e.get('radius')) continue;                       // spots: not imported yet
    const color = (e.vec('_color') || [1, 1, 1]);
    out.push({ origin: e.origin, light: Number(e.get('light') || e.get('_light') || 300) * Number(e.get('_scale') || 1), color, note: 'map source', spawnflags: Number(e.get('spawnflags') || 0) });
  }
  return out;
}

function handEdits(map, lights) {
  const f = path.join(REPO, 'misc', 'oacontent', 'data', 'lights', `${map}.json`);
  if (!fs.existsSync(f)) return { lights, edits: null };
  const j = JSON.parse(fs.readFileSync(f, 'utf8'));
  let out = lights.slice();
  for (const r of j.remove || []) out = out.filter((l) => Math.hypot(l.origin[0] - r.origin[0], l.origin[1] - r.origin[1], l.origin[2] - r.origin[2]) > (r.radius ?? 64));
  for (const s of j.scale || []) for (const l of out) if (Math.hypot(l.origin[0] - s.origin[0], l.origin[1] - s.origin[1], l.origin[2] - s.origin[2]) <= (s.radius ?? 128)) l.light *= s.factor;
  for (const a of j.add || []) out.push({ origin: a.origin, light: a.light, color: a.color || [1, 1, 1], note: 'hand edit' });
  return { lights: out, edits: j };
}

export function recoverMap(cs, shaders, mapPath, opts = {}) {
  const bsp = new Bsp(cs.read(mapPath), mapPath);
  bsp.skyNames = new Set([...shaders.byName.values()].filter((d) => d.sky).map((d) => d.name.toLowerCase()));
  const name = path.basename(mapPath, '.bsp');
  const lmScale = Number(opts.lmscale ?? 0.5);
  const report = { name, source: null, lmScale };
  let lights = [];
  const existing = parseEntities(bsp.entityText).filter((e) => e.classname === 'light').length;
  report.existingLightEntities = existing;

  const srcFile = opts.mapsource ? path.join(opts.mapsource, `${name}.map`) : null;
  if (srcFile && fs.existsSync(srcFile)) {
    lights = importMapSource(fs.readFileSync(srcFile, 'latin1'));
    report.source = 'map source';
    report.imported = lights.length;
  } else {
    let ems = liftOrigins(bsp, emitters(bsp, shaders, cs));
    report.emitterCandidates = ems.length;
    ems = limit(ems, Number(opts.max) || 64);
    const texels = lightmapTexels(bsp, Number(opts.texels) || 3000);
    report.texels = texels.length;
    if (texels.length < 200) {
      report.source = 'none';
      report.note = 'too few lightmap texels to fit (a vertex-lit or lightmap-less map)';
    } else {
      const fit = new Fit(bsp, texels, 1 - lmScale);
      for (const e of ems) fit.add(e.origin);
      const before = ems.length ? fit.solve() : fit.stats();
      lights = ems.map((e, i) => ({ origin: e.origin, light: REF_LIGHT * fit.k[i], color: e.color.map((c) => c / Math.max(...e.color, 1e-6)), note: `surfacelight ${e.surfaceLight} ${e.shader}`, area: e.area, idx: i })).filter((l) => l.light >= 5);
      report.source = ems.length ? 'surface lights + lightmap fit' : 'lightmap fit';
      report.surfaceLights = lights.length;
      report.fitSurfaceOnly = { r2: +before.r2.toFixed(3), ambient: +before.ambient.toFixed(4), meanBaked: +before.meanTarget.toFixed(4) };
      if (opts['no-hotspots'] === undefined) {
        const extra = fit.greedy({ maxAdd: Number(opts.hotspots) || 24 });
        lights = lights.map((l) => ({ ...l, light: REF_LIGHT * fit.k[l.idx] })).filter((l) => l.light >= 5).concat(extra.filter((l) => l.light >= 5));
        report.hotspotLights = extra.length;
      }
      const fin = fit.stats();
      report.fit = { r2: +fin.r2.toFixed(3), ambient: +fin.ambient.toFixed(4), meanBaked: +fin.meanTarget.toFixed(4) };
    }
  }
  const edited = handEdits(name, lights);
  lights = edited.lights;
  report.handEdits = edited.edits ? { add: (edited.edits.add || []).length, remove: (edited.edits.remove || []).length, scale: (edited.edits.scale || []).length } : null;
  report.lights = lights.length;
  report.totalIntensity = Math.round(lights.reduce((a, l) => a + l.light, 0));
  return { report, lights, text: lights.length ? sidecar({ lmScale, lights, comment: `oacontent lights: ${name}, ${report.source}, ${lights.length} lights` }) : null };
}

export async function run(args) {
  const baseoa = args.baseoa || DEFAULT_BASEOA;
  const out = path.join(args.out || OUT, 'lights');
  fs.mkdirSync(out, { recursive: true });
  const cs = new ContentSet([baseoa]);
  const shaders = loadShaders(cs.shaderFiles());
  const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;
  const rows = [];
  for (const m of cs.maps()) {
    if (only && !only.has(m.name)) continue;
    try {
      const r = recoverMap(cs, shaders, m.path, args);
      if (r.text) fs.writeFileSync(path.join(out, `${m.name}.oaxmap`), r.text);
      rows.push(r.report);
    } catch (e) { rows.push({ name: m.name, error: e.message }); }
  }
  fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(rows, null, 1) + '\n');
  const lines = ['map              source                         lights  r2    texels  total light  note'];
  for (const r of rows) lines.push(`${r.name.padEnd(16)} ${String(r.error ? 'ERROR ' + r.error : r.source).padEnd(30)} ${String(r.lights ?? '').padStart(6)}  ${String(r.fit?.r2 ?? '').padEnd(5)} ${String(r.texels ?? '').padStart(6)}  ${String(r.totalIntensity ?? '').padStart(11)}  ${r.note || ''}`);
  fs.writeFileSync(path.join(out, 'report.txt'), lines.join('\n') + '\n');
  console.log(lines.join('\n'));
  console.log(`sidecars in ${out}`);
  return 0;
}
