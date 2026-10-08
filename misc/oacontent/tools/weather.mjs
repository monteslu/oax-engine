// T13 weather: rain or snow over the open ground of outdoor maps, as looping
// func_oax_emitter entities (oax/rain, oax/snow). Particles do not collide, so
// an emitter is placed only over ground that sees the sky.
//
//   oacontent weather --kind rain|snow [--maps a,b] [--out dir] [--cell 900] [--max 14]
// Default maps: the outdoor class of the exposure table (run skyexposure first).
// Output: <out>/weather/<map>.oaxmap (not part of the default pack: weather is a
// choice, add it with `pack --parts weather`), report.json.
import fs from 'node:fs';
import path from 'node:path';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp } from '../lib/bsp.mjs';
import { loadShaders } from '../lib/shader.mjs';
import { entity, serializeEntities } from '../lib/entities.mjs';
import { floorSamples } from './skyexposure.mjs';
import { OUT } from '../lib/common.mjs';

// emitter sites: grid cells of exposed floor, biggest first
export function weatherSites(samples, exposedFn, { cell = 900, max = 14, height = 450 } = {}) {
  const cells = new Map();
  for (const s of samples) {
    if (!exposedFn(s.p)) continue;
    const k = `${Math.floor(s.p[0] / cell)},${Math.floor(s.p[1] / cell)}`;
    const c = cells.get(k) || { n: 0, x: 0, y: 0, zmax: -Infinity };
    c.n++; c.x += s.p[0]; c.y += s.p[1]; c.zmax = Math.max(c.zmax, s.p[2]);
    cells.set(k, c);
  }
  return [...cells.values()].filter((c) => c.n >= 5).sort((a, b) => b.n - a.n).slice(0, max)
    .map((c) => ({ origin: [Math.round(c.x / c.n), Math.round(c.y / c.n), Math.round(c.zmax + height)], samples: c.n }));
}

export async function run(args = {}) {
  const kind = args.kind === 'snow' ? 'snow' : 'rain';
  const out = path.join(args.out || OUT, 'weather');
  const tableFile = path.join(args.out || OUT, 'skyexposure', 'table.json');
  if (!fs.existsSync(tableFile)) { console.error('no exposure table: run `oacontent skyexposure` first'); return 2; }
  const table = JSON.parse(fs.readFileSync(tableFile, 'utf8'));
  const cs = new ContentSet([args.baseoa || DEFAULT_BASEOA]);
  const shaders = loadShaders(cs.shaderFiles());
  const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;
  fs.mkdirSync(out, { recursive: true });
  const rows = [];
  for (const m of cs.maps()) {
    const row = table.find((r) => r.name === m.name);
    if (!row || (only ? !only.has(m.name) : row.class !== 'outdoor')) continue;
    const bsp = new Bsp(cs.read(m.path), m.path);
    bsp.skyNames = new Set([...shaders.byName.values()].filter((d) => d.sky).map((d) => d.name.toLowerCase()));
    const top = bsp.models[0].maxs[2] + 64;
    const sites = weatherSites(floorSamples(bsp, shaders, 96, 3000), (p) => { const h = bsp.upRay(p, top + 1000); return !h || h.sky; }, { cell: Number(args.cell || 900), max: Number(args.max || 14) });
    if (!sites.length) continue;
    const ents = sites.map((s, i) => entity('func_oax_emitter', { origin: s.origin, particle: `oax/${kind}`, seed: 500 + i }));
    fs.writeFileSync(path.join(out, `${m.name}.oaxmap`), `// oacontent weather: ${m.name}, ${kind}, ${sites.length} emitters\n` + serializeEntities(ents));
    rows.push({ map: m.name, kind, emitters: sites.length });
  }
  fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(rows, null, 1));
  console.log(`weather ${kind}: ${rows.length} maps, ${rows.reduce((a, r) => a + r.emitters, 0)} emitters -> ${out}`);
  return 0;
}
