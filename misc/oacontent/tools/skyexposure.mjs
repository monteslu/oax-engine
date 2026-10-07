// T10 sky exposure: how outdoors is each map, measured. From a grid of floor
// points (upward facing surfaces, sampled by area) a ray goes straight up
// through the world brushes: it reaches sky (exposed), meets another solid
// (covered) or leaves the world box without a hit (open). A second ray goes
// toward the sun (the sky shader's q3map_sun direction) for sun exposure.
// Output: <out>/skyexposure/{table.json,table.txt,maps/<name>.json}.
//   oacontent skyexposure [--baseoa dir] [--maps a,b] [--spacing 96] [--max 3000]
import fs from 'node:fs';
import path from 'node:path';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp, SURF, MST } from '../lib/bsp.mjs';
import { loadShaders } from '../lib/shader.mjs';
import { OUT } from '../lib/common.mjs';

const GRID = 6;   // the per-map coarse grid of exposure (GRID x GRID cells over the floor bounds)

export function sunDirection(sun) {
  // q3map_sun r g b intensity degrees elevation: degrees = compass angle about z
  if (!sun || sun.length < 6) return null;
  const az = (sun[4] * Math.PI) / 180, el = (sun[5] * Math.PI) / 180;
  return [Math.cos(az) * Math.cos(el), Math.sin(az) * Math.cos(el), Math.sin(el)];
}

// floor samples: [{ p: [x, y, z], area }] on upward facing planar surfaces and
// patch cells, spaced by `spacing`, capped at `max` by thinning
export function floorSamples(bsp, shaders, spacing = 96, max = 3000) {
  const pts = [];
  for (const s of bsp.surfaces) {
    if (s.type !== MST.PLANAR && s.type !== MST.PATCH) continue;
    const sh = bsp.shaders[s.shader];
    const def = shaders.byName.get(sh.name.toLowerCase());
    if ((sh.surfaceFlags & (SURF.SKY | SURF.NODRAW)) || def?.sky || def?.nodraw || def?.fog) continue;
    // facing from the vertices' own normals (the winding of the triangles in a Q3
    // BSP is not a reliable up or down): a planar face has one normal, a patch
    // an average over its control points
    const N = bsp.verts.normal;
    let nz = 0;
    if (s.type === MST.PLANAR) nz = N[s.firstVert * 3 + 2];
    else { for (let v = 0; v < s.numVerts; v++) nz += N[(s.firstVert + v) * 3 + 2]; nz /= Math.max(1, s.numVerts); }
    if (nz < 0.7) continue;
    for (const t of bsp.surfaceTriangles(s)) {
      const e1 = [t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]], e2 = [t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]];
      const c = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]];
      const a = Math.hypot(...c) / 2;
      if (a < 64) continue;
      const n = Math.max(1, Math.round(a / (spacing * spacing)));
      for (let k = 0; k < n; k++) {
        // deterministic low-discrepancy points inside the triangle
        const u = ((k + 0.5) / n), v = ((k * 0.618034) % 1);
        const su = Math.sqrt(u), b0 = 1 - su, b1 = su * (1 - v), b2 = su * v;
        pts.push({ p: [t[0][0] * b0 + t[1][0] * b1 + t[2][0] * b2, t[0][1] * b0 + t[1][1] * b1 + t[2][1] * b2, t[0][2] * b0 + t[1][2] * b1 + t[2][2] * b2 + 24], area: a / n });
      }
    }
  }
  if (pts.length > max) { const step = pts.length / max; return Array.from({ length: max }, (_, i) => pts[Math.floor(i * step)]); }
  return pts;
}

export function exposure(bsp, shaders, { spacing = 96, max = 3000 } = {}) {
  bsp.skyNames = new Set([...shaders.byName.values()].filter((d) => d.sky).map((d) => d.name.toLowerCase()));
  const samples = floorSamples(bsp, shaders, spacing, max);
  const def = new Map();
  let sun = null;
  for (const sh of bsp.shaders) { const d = shaders.byName.get(sh.name.toLowerCase()); if (d?.sky && d.sun) { sun = d.sun; def.set('sky', d.name); } }
  const dir = sunDirection(sun);
  const leafs = bsp.leafs;
  let n = 0, sky = 0, open = 0, covered = 0, sunLit = 0, sunN = 0, w = 0;
  const bb = { lo: [Infinity, Infinity], hi: [-Infinity, -Infinity] };
  for (const s of samples) { bb.lo[0] = Math.min(bb.lo[0], s.p[0]); bb.lo[1] = Math.min(bb.lo[1], s.p[1]); bb.hi[0] = Math.max(bb.hi[0], s.p[0]); bb.hi[1] = Math.max(bb.hi[1], s.p[1]); }
  const cells = Array.from({ length: GRID * GRID }, () => ({ n: 0, sky: 0 }));
  const top = bsp.models[0].maxs[2] + 64;
  for (const s of samples) {
    const leaf = leafs[bsp.pointLeaf(s.p)];
    if (!leaf || leaf.cluster < 0) continue;                      // inside solid or the void
    n++; w += s.area;
    const h = bsp.upRay(s.p, top + 1000);
    const exposed = !h || h.sky;
    if (h && h.sky) sky++; else if (!h) open++; else covered++;
    const cx = Math.min(GRID - 1, Math.floor(((s.p[0] - bb.lo[0]) / Math.max(1, bb.hi[0] - bb.lo[0])) * GRID)), cy = Math.min(GRID - 1, Math.floor(((s.p[1] - bb.lo[1]) / Math.max(1, bb.hi[1] - bb.lo[1])) * GRID));
    const cell = cells[cy * GRID + cx]; cell.n++; if (exposed) cell.sky++;
    if (dir && exposed) { sunN++; const sh = bsp.ray(s.p, dir, 20000); if (!sh || sh.sky) sunLit++; }
  }
  const frac = (x) => (n ? +(x / n).toFixed(3) : 0);
  const exposedFrac = frac(sky + open);
  return {
    samples: n, skyFraction: frac(sky), openFraction: frac(open), coveredFraction: frac(covered), exposed: exposedFrac,
    sun: sun ? { rgb: sun.slice(0, 3), intensity: sun[3], degrees: sun[4], elevation: sun[5], direction: dir.map((x) => +x.toFixed(3)) } : null,
    sunLitOfExposed: sunN ? +(sunLit / sunN).toFixed(3) : null,
    grid: { size: GRID, bounds: bb, cells: cells.map((c) => (c.n ? +(c.sky / c.n).toFixed(2) : null)) },
    class: exposedFrac >= 0.5 ? 'outdoor' : exposedFrac >= 0.15 ? 'mixed' : 'indoor',
    skyShader: def.get('sky') || null,
  };
}

export async function run(args) {
  const baseoa = args.baseoa || DEFAULT_BASEOA;
  const out = path.join(args.out || OUT, 'skyexposure');
  fs.mkdirSync(path.join(out, 'maps'), { recursive: true });
  const cs = new ContentSet([baseoa]);
  const shaders = loadShaders(cs.shaderFiles());
  const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;
  const rows = [];
  const t0 = Date.now();
  for (const m of cs.maps()) {
    if (only && !only.has(m.name)) continue;
    try {
      const bsp = new Bsp(cs.read(m.path), m.path);
      const r = { name: m.name, size: bsp.models[0].maxs.map((v, i) => Math.round(v - bsp.models[0].mins[i])), ...exposure(bsp, shaders, { spacing: Number(args.spacing) || 96, max: Number(args.max) || 3000 }) };
      fs.writeFileSync(path.join(out, 'maps', `${m.name}.json`), JSON.stringify(r, null, 1) + '\n');
      rows.push(r);
    } catch (e) { rows.push({ name: m.name, error: e.message }); }
  }
  rows.sort((a, b) => (b.exposed ?? -1) - (a.exposed ?? -1));
  fs.writeFileSync(path.join(out, 'table.json'), JSON.stringify(rows.map(({ grid, ...r }) => r), null, 1) + '\n');
  const lines = ['map              class    exposed  sky   open  covered  sunlit  samples  sky shader'];
  for (const r of rows) {
    if (r.error) { lines.push(`${r.name.padEnd(16)} ERROR ${r.error}`); continue; }
    lines.push(`${r.name.padEnd(16)} ${r.class.padEnd(8)} ${r.exposed.toFixed(2).padStart(7)} ${r.skyFraction.toFixed(2).padStart(5)} ${r.openFraction.toFixed(2).padStart(5)} ${r.coveredFraction.toFixed(2).padStart(8)} ${(r.sunLitOfExposed ?? NaN).toFixed(2).padStart(7)} ${String(r.samples).padStart(8)}  ${r.skyShader || ''}`);
  }
  fs.writeFileSync(path.join(out, 'table.txt'), lines.join('\n') + '\n');
  console.log(lines.join('\n'));
  console.log(`${rows.length} maps in ${((Date.now() - t0) / 1000).toFixed(0)} s; written to ${out}`);
  return 0;
}
