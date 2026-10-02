// Instanced terrain foliage (phase 7) and terrain frame time, on the cart.
//
// - placement is deterministic and shared: a JS port of
//   OAXTerrain_CellFoliage over the map's own OAX_TERRAIN lump predicts the
//   renderer's instance total and the collision model's tree trunk count
//   exactly; control: the same port with another seed must not match;
// - instancing draws them: grass and trees counted per frame; a ground view
//   with foliage differs from r_oaxFoliage 0 (control: it would not if
//   nothing were drawn) and matches its golden;
// - distance fade: a far camera still draws every tree but skips the grass
//   of chunks past the grass fade end (within it, grass dithers out);
// - frame time: host wall time over a camera path (ulight-perf method),
//   published as rows; budget OA_TERRAIN_P95_MS (default 33).

import fs from 'node:fs';
import path from 'node:path';
import { cartShots } from '../lib/cartshot.mjs';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW, mapPath } from '../lib/scenes.mjs';
import { readValues } from '../lib/values.mjs';
import { comparePng, readPng, halfSize, writePng } from '../lib/png.mjs';
import { diffFraction } from '../lib/imgstat.mjs';
import { readTerrainLump, foliageCounts } from '../lib/oaxterrain.mjs';

export const name = 'terrain-foliage';

const MAP = 'oax_terrain';
const SETUP = ['r_fixedShaderTime 3', 'r_autoExposure 0'];
const GROUND = 'cl_overrideView "-900 -1100 120 8 45 0"';
const FAR = 'cl_overrideView "-1500 -1500 1150 35 45 0"';
const P95_MS = Number(process.env.OA_TERRAIN_P95_MS || 33);

async function frameTime(out) {
  const s = new Session('terrain-foliage-perf');
  try {
    await loadScene(s, MAP, { view: `${CLEAN_VIEW};r_autoExposure 0` });
    const shot = path.join(out, 'terrain-foliage-perf.png');
    const costs = [];
    for (let i = 0; i < 5; i++) { const t0 = performance.now(); await s.screenshot(shot); costs.push(performance.now() - t0); }
    const times = [];
    for (let b = 0; b < 12; b++) {
      const a = (b / 12) * Math.PI * 2;
      await s.command(`cl_overrideView "${(Math.cos(a) * 900).toFixed(0)} ${(Math.sin(a) * 900).toFixed(0)} 220 12 ${((a * 180 / Math.PI) + 180) % 360} 0"`);
      const t0 = performance.now();
      await s.step(30);
      await s.screenshot(shot);
      const t1 = performance.now();
      await s.screenshot(shot);
      const t2 = performance.now();
      times.push(Math.max(0, (t1 - t0) - (t2 - t1)) / 30);
    }
    const v = await readValues(s);
    times.sort((x, y) => x - y);
    return { p95: times[Math.min(times.length - 1, Math.floor(times.length * 0.95))], mean: times.reduce((x, y) => x + y, 0) / times.length, v };
  } finally {
    await s.shutdown();
  }
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const gdir = path.join(goldens, 'terrain');
  fs.mkdirSync(gdir, { recursive: true });

  const t = readTerrainLump(mapPath(MAP))[0];
  const counts = foliageCounts(t);
  const other = foliageCounts({ ...t, seed: t.seed + 1 });
  const r = await cartShots('terrain-foliage', MAP, [
    { cmd: GROUND, name: 'ground', values: 'ground' },
    { cmd: 'r_oaxFoliage 0', name: 'bare' },
    { cmd: `r_oaxFoliage 1;${FAR}`, name: 'far', values: 'far' },
  ], { setup: SETUP, out });
  const g = r.valuesAt.ground, f = r.valuesAt.far;
  const total = counts.reduce((a, b) => a + b, 0);
  rows.push(`placement: JS predicts ${counts.join(' + ')} = ${total} instances, renderer placed ${g.r_foliage_instances_total}; tree trunks predicted ${counts[1]}, collision has ${g.cm_terrain_trunks}; control seed+1 predicts ${other.join(' + ')}`);
  if (Number(g.r_foliage_instances_total) !== total) failures.push('the renderer placed a different number of instances than predicted');
  if (Number(g.cm_terrain_trunks) !== counts[1]) failures.push('collision has a different number of tree trunks than predicted');
  if (other.reduce((a, b) => a + b, 0) === total) failures.push('control did not fail: another seed predicts the same count');

  rows.push(`ground view: ${g.r_foliage_grass} grass + ${g.r_foliage_trees} trees drawn (instanced, per chunk); far view: ${f.r_foliage_grass} grass + ${f.r_foliage_trees} trees`);
  if (!(Number(g.r_foliage_grass) > 0 && Number(g.r_foliage_trees) > 0)) failures.push('the ground view draws no grass or no trees');
  if (!(Number(f.r_foliage_grass) < Number(g.r_foliage_grass) && Number(f.r_foliage_trees) > 0)) failures.push('distance fade: the far view should draw trees and less grass (chunks past the grass fade end are skipped)');
  const bare = diffFraction(r.images.ground, r.images.bare, 16);
  rows.push(`foliage covers ${(bare * 100).toFixed(1)}% of the ground view (against r_oaxFoliage 0)`);
  if (bare < 0.05) failures.push('foliage barely changes the ground view');

  const golden = path.join(gdir, 'foliage_ground.png');
  const half = halfSize(r.images.ground);
  if (update || !fs.existsSync(golden)) {
    writePng(golden, half);
    rows.push(`golden ground ${update ? 'updated' : 'created'}`);
  } else {
    const c = comparePng(readPng(golden), half, { tolerance: 24, diffPath: path.join(out, 'terrain-foliage_ground.diff.png') });
    const cc = comparePng(readPng(golden), halfSize(r.images.bare), { tolerance: 24 });
    rows.push(`golden ground: ${(c.badFraction * 100).toFixed(3)}% over tolerance; control (no foliage) ${(cc.badFraction * 100).toFixed(1)}%`);
    if (!c.sameSize || c.badFraction > 0.005) failures.push(`golden ground: ${(c.badFraction * 100).toFixed(2)}% differ`);
    if (cc.badFraction <= 0.005) failures.push('control did not fail: the foliage golden matches a bare frame');
  }

  const p = await frameTime(out);
  rows.push(`frame time on oax_terrain (circling at 220 units, foliage and sun shadows on): p95 ${p.p95.toFixed(2)} ms, mean ${p.mean.toFixed(2)} ms (host wall time, GPU-synced); last frame ${p.v.r_terrain_tris} terrain triangles, ${p.v.r_foliage_instances} foliage instances, ${p.v.r_terrain_chunks_occluded} chunks occluded`);
  if (!(p.p95 <= P95_MS)) failures.push(`frame time p95 ${p.p95.toFixed(2)} ms > ${P95_MS} ms`);
  return { ok: failures.length === 0, failures, rows };
}
