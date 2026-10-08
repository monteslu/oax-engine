// T12 sky and outdoor mode: the per-map environment keys the enhanced look
// asks for, from the T10 exposure table (run `skyexposure` first).
//
//   oacontent sky [--maps a,b] [--out dir]
// Every map: oax_bloom 1 (bloom on while r_oaxBloom is 2). A map whose sky has
// a sun and where at least 15% of the floor sees the sky: oax_sundisc (the sun
// drawn in the sky). An outdoor map (50% or more): also a light haze
// (oax_atmosphere), cloud shadows (oax_clouds) and a mild grade (oax_grade).
// Output: <out>/sky/<map>.oaxmap and report.json.
import fs from 'node:fs';
import path from 'node:path';
import { entity, serializeEntities } from '../lib/entities.mjs';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp } from '../lib/bsp.mjs';
import { OUT } from '../lib/common.mjs';

export function skyKeys(row, floorZ) {
  const keys = { oax_bloom: 1 };
  const hasSun = !!row.sun;
  if (hasSun && row.exposed >= 0.15) keys.oax_sundisc = 0.07;
  if (row.exposed >= 0.5) {
    keys.oax_atmosphere = `0.62 0.72 0.86 0.00005 0.0008 ${Math.round(floorZ)} ${hasSun ? 0.35 : 0}`;
    if (hasSun) keys.oax_clouds = '1600 12 5 0.45 0.3';
    keys.oax_grade = '1.05 1.04 1 1 1 0.1';
  }
  return keys;
}

export async function run(args = {}) {
  const out = path.join(args.out || OUT, 'sky');
  const tableFile = path.join(args.out || OUT, 'skyexposure', 'table.json');
  if (!fs.existsSync(tableFile)) { console.error('no exposure table: run `oacontent skyexposure` first'); return 2; }
  const table = JSON.parse(fs.readFileSync(tableFile, 'utf8'));
  const cs = new ContentSet([args.baseoa || DEFAULT_BASEOA]);
  const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;
  fs.mkdirSync(out, { recursive: true });
  const rows = [];
  for (const m of cs.maps()) {
    if (only && !only.has(m.name)) continue;
    const row = table.find((r) => r.name === m.name);
    if (!row) continue;
    const bsp = new Bsp(cs.read(m.path), m.path);
    const keys = skyKeys(row, bsp.models[0].mins[2] + 64);
    fs.writeFileSync(path.join(out, `${m.name}.oaxmap`), `// oacontent sky: ${m.name}, ${row.class}\n` + serializeEntities([entity('worldspawn', keys)]));
    rows.push({ map: m.name, class: row.class, keys: Object.keys(keys) });
  }
  fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(rows, null, 1));
  const by = (c) => rows.filter((r) => r.class === c).length;
  console.log(`sky: ${rows.length} maps (${by('outdoor')} outdoor, ${by('mixed')} mixed, ${by('indoor')} indoor), ${rows.filter((r) => r.keys.includes('oax_sundisc')).length} with a sun disc -> ${out}`);
  return 0;
}
