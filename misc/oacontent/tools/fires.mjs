// T11 fires: flame sprites in the stock maps (shaders named flame, bigflame,
// xflame, ...) get a real flame behind them: a looping particle emitter
// (oax/flame, oax/flame_small) and a flickering light (oax_effect oax_fire),
// as sidecar entities.
//
//   oacontent fires [--maps a,b] [--baseoa dir] [--out dir] [--radius 260] [--shadows 3] [--max-lights 12]
// Output: <out>/fires/<map>.oaxmap and report.json. Sprites within 40 units of
// each other are one fire (a bigflame is 16 animation frames). The first
// --shadows lights of a map cast shadows; at most --max-lights fires glow (an
// even spread), the rest are emitter only.
import fs from 'node:fs';
import path from 'node:path';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp } from '../lib/bsp.mjs';
import { entity, serializeEntities } from '../lib/entities.mjs';
import { OUT } from '../lib/common.mjs';

export const FLAME = /(^|\/)(x?flame|bigflame|[rb]_flame|flameanim|flame\d|flame1side|flame1km|flame2)[^/]*$/i;
const JOIN = 40;

export function flameClusters(bsp) {
  const sh = bsp.shaders, V = bsp.verts.xyz;
  const items = [];
  for (const s of bsp.surfaces) {
    const name = sh[s.shader]?.name || '';
    if (!FLAME.test(name) || /torch|flare/i.test(name) || s.numVerts < 3) continue;
    const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
    for (let i = s.firstVert; i < s.firstVert + s.numVerts; i++) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], V[i * 3 + k]); hi[k] = Math.max(hi[k], V[i * 3 + k]); }
    items.push({ lo, hi, name });
  }
  const clusters = [];
  for (const it of items) {
    const c = [(it.lo[0] + it.hi[0]) / 2, (it.lo[1] + it.hi[1]) / 2, (it.lo[2] + it.hi[2]) / 2];
    const hit = clusters.find((k) => Math.hypot(k.c[0] - c[0], k.c[1] - c[1], k.c[2] - c[2]) < JOIN + Math.max(k.size, it.hi[2] - it.lo[2]) / 2);
    if (hit) {
      for (let k = 0; k < 3; k++) { hit.lo[k] = Math.min(hit.lo[k], it.lo[k]); hit.hi[k] = Math.max(hit.hi[k], it.hi[k]); }
      hit.c = [(hit.lo[0] + hit.hi[0]) / 2, (hit.lo[1] + hit.hi[1]) / 2, (hit.lo[2] + hit.hi[2]) / 2];
      hit.size = hit.hi[2] - hit.lo[2]; hit.names.add(it.name);
    } else clusters.push({ lo: [...it.lo], hi: [...it.hi], c, size: it.hi[2] - it.lo[2], names: new Set([it.name]) });
  }
  return clusters;
}

export function fireEntities(clusters, { radius = 260, shadows = 3, maxLights = 12 } = {}) {
  const ents = [];
  // lights cost a lit pass each: with many fires only an even spread of them glow
  const lit = new Set(clusters.length <= maxLights ? clusters.map((_, i) => i) : Array.from({ length: maxLights }, (_, j) => Math.floor((j * clusters.length) / maxLights)));
  let nLit = 0;
  clusters.forEach((k, i) => {
    const small = k.size < 36;
    ents.push(entity('func_oax_emitter', { origin: [k.c[0], k.c[1], k.lo[2] + 2].map((v) => Math.round(v)).join(' '), particle: small ? 'oax/flame_small' : 'oax/flame', seed: 100 + i }));
    if (!lit.has(i)) return;
    const l = entity('rtlight', { origin: [k.c[0], k.c[1], k.lo[2] + Math.max(8, k.size * 0.6)].map((v) => Math.round(v)).join(' '), oax_profile: 'physical', oax_radius: small ? radius * 0.7 : radius, oax_intensity: small ? 0.7 : 1, oax_color: '1 0.55 0.22', oax_effect: `oax_fire ${(1.8 + (i % 5) * 0.37).toFixed(2)} ${((i * 0.29) % 1).toFixed(2)} 0.6 0.4`, _note: `fire ${[...k.names][0]}` });
    if (nLit++ >= shadows) l.set('noshadows', '1');
    ents.push(l);
  });
  return ents;
}

export async function run(args = {}) {
  const baseoa = args.baseoa || DEFAULT_BASEOA;
  const out = path.join(args.out || OUT, 'fires');
  fs.mkdirSync(out, { recursive: true });
  const cs = new ContentSet([baseoa]);
  const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;
  const rows = [];
  for (const m of cs.maps()) {
    if (only && !only.has(m.name)) continue;
    const bsp = new Bsp(cs.read(m.path), m.path);
    const cl = flameClusters(bsp);
    if (!cl.length) continue;
    const ents = fireEntities(cl, { radius: Number(args.radius || 260), shadows: Number(args.shadows ?? 3), maxLights: Number(args['max-lights'] || 12) });
    fs.writeFileSync(path.join(out, `${m.name}.oaxmap`), `// oacontent fires: ${m.name}, ${cl.length} fires\n` + serializeEntities(ents));
    rows.push({ map: m.name, fires: cl.length, small: cl.filter((k) => k.size < 36).length });
  }
  fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(rows, null, 1));
  console.log('map              fires small\n' + rows.map((r) => `${r.map.padEnd(16)} ${String(r.fires).padStart(5)} ${String(r.small).padStart(5)}`).join('\n'));
  console.log(`${rows.reduce((a, r) => a + r.fires, 0)} fires in ${rows.length} maps -> ${out}`);
  return 0;
}
