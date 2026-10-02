// Terrain occlusion queries (phase 7): chunks and their foliage hidden
// behind terrain are skipped, and nothing ever pops.
//
// On oax_occlusion a camera starts low south of a tall ridge (every chunk
// north of it hidden, trees included), rises and flies over it, revealing
// them (the reveal frame is reported).
// The same path runs three times on the cart:
//   r_oaxOcclusion 1 (conservative): must cull chunks at the start, and every
//     frame along the path must be byte-identical to r_oaxOcclusion 0;
//   r_oaxOcclusion 2 (control: first results frozen, no inflation, no camera
//     check) must differ from r_oaxOcclusion 0 somewhere along the path, or
//     the comparison could not catch a pop.
// Query results come back a frame late (the WebGL2 rule the cart follows),
// so this is the case a careless implementation gets wrong.

import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { readValues } from '../lib/values.mjs';
import { readPng } from '../lib/png.mjs';

export const name = 'terrain-occlusion';

const MAP = 'oax_occlusion';
// the camera rises 6 units a frame to 700, then flies north over the ridge
// at 8 units a frame, looking a little down
const FRAMES = 300;
const SHOT_EVERY = 5;
const view = (k) => `0 ${-1300 + 8 * Math.max(0, k - 110)} ${40 + 6 * Math.min(k, 110)} 15 90 0`;

async function runPath(mode, out) {
  const s = new Session(`terrain-occlusion-${mode}`);
  const shots = [];
  const counts = [];
  try {
    await loadScene(s, MAP, { view: `${CLEAN_VIEW};r_autoExposure 0;r_oaxOcclusion ${mode}` });
    await s.command(`cl_overrideView "${view(0)}"`);
    await s.step(30);
    for (let k = 0; k < FRAMES; k++) {
      await s.command(`cl_overrideView "${view(k)}"`);
      await s.step(1);
      if (k % SHOT_EVERY === 0) {
        const f = path.join(out, `terrain-occlusion-${mode}-${k}.png`);
        await s.screenshot(f);
        shots.push(readPng(f).data);
        const v = await readValues(s);
        counts.push({ k, occluded: Number(v.r_terrain_chunks_occluded), drawn: Number(v.r_terrain_chunks_drawn), trees: Number(v.r_foliage_trees), queries: Number(v.r_terrain_queries) });
      }
    }
  } finally {
    await s.shutdown();
  }
  return { shots, counts };
}

function firstDiff(a, b) {
  for (let i = 0; i < a.shots.length; i++) if (!a.shots[i].equals(b.shots[i])) return i * SHOT_EVERY;
  return -1;
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const on = await runPath(1, out);
  const off = await runPath(0, out);
  const frozen = await runPath(2, out);

  const start = on.counts[0], startOff = off.counts[0];
  const culled = on.counts.reduce((n, c) => n + c.occluded, 0);
  const lastCulled = on.counts.filter((c) => c.occluded > 0).map((c) => c.k).pop();
  rows.push(`start (behind the ridge): ${start.drawn} chunks drawn, ${start.occluded} occluded, ${start.queries} queries, ${start.trees} trees drawn (occlusion off: ${startOff.drawn} chunks, ${startOff.trees} trees)`);
  const end = on.counts[on.counts.length - 1];
  rows.push(`along the path: ${culled} chunk-frames culled over ${on.counts.length} samples, last culling at frame ${lastCulled ?? 'none'}; at the end ${end.drawn} chunks and ${end.trees} trees drawn (${end.occluded} occluded)`);
  if (!(end.trees > start.trees && end.occluded < start.occluded)) failures.push('the path never revealed the hidden chunks (nothing to pop)');
  if (!(start.occluded > 0)) failures.push('no chunk was occluded behind the ridge');
  if (!(start.trees < startOff.trees)) failures.push('the occluded chunks\' trees were still drawn');
  const d = firstDiff(on, off);
  rows.push(`never pops: occlusion on vs off ${d < 0 ? `identical in all ${on.shots.length} frames` : `DIFFER at frame ${d}`}`);
  if (d >= 0) failures.push(`occlusion changed the picture at frame ${d}`);
  const dc = firstDiff(frozen, off);
  rows.push(`control (frozen results): ${dc < 0 ? 'IDENTICAL' : `differs from occlusion off at frame ${dc}`}`);
  if (dc < 0) failures.push('control did not fail: frozen occlusion results matched every frame');
  return { ok: failures.length === 0, failures, rows };
}
