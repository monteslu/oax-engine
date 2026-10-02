// Hybrid lighting: a lightmapped map (oax_ulight_hybrid) keeps its baked
// base and adds its `rtlight` as a realtime shadowed light.
//   - the rtlight is visible (r_ulights_visible_ids names it);
//   - the frame differs from the same view with r_ulight 0 (the control,
//     which is the stock lightmap path) only where the rtlight reaches;
//   - the lightmapped base is there in both (not black);
//   - the cart matches the native build.

import path from 'node:path';
import { shootCart, shootNative, readPng } from '../lib/ulight.mjs';
import { comparePng, halfSize } from '../lib/png.mjs';
import { readEntities } from '../lib/bsp.mjs';
import { repoRoot } from '../lib/romdev.mjs';

export const name = 'ulight-hybrid';

const MAP = 'oax_ulight_hybrid';
const VIEW = '-380 -380 150 20 45 0';
const PLAYER = '-384 -384 32 45';

function mean(img) {
  let s = 0;
  for (let i = 0; i < img.data.length; i += 4) s += Math.max(img.data[i], img.data[i + 1], img.data[i + 2]);
  return s / (img.data.length / 4);
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const ents = readEntities(path.join(repoRoot, 'tests', 'maps', 'out', 'baseoa', 'maps', `${MAP}.bsp`));
  const rt = String(ents.findIndex((e) => e.classname === 'rtlight'));
  const on = await shootCart(MAP, [VIEW], { name: 'ulight-hybrid-on', out, player: PLAYER });
  const off = await shootCart(MAP, [VIEW], { name: 'ulight-hybrid-off', out, player: PLAYER, pre: 'r_ulight 0' });
  const nat = shootNative(MAP, [VIEW], { name: 'ulight-hybrid-native', out, player: PLAYER });
  const a = readPng(on.files[0]), b = readPng(off.files[0]);
  const ids = String(on.values.r_ulights_visible_ids || '').split(',');
  const d = comparePng(a, b, { tolerance: 16 });
  rows.push(`rtlight ${rt} visible: ${ids.includes(rt)} (ids ${on.values.r_ulights_visible_ids}); model ${on.values.r_ulight_model}`);
  rows.push(`r_ulight 1 vs 0: ${(d.badFraction * 100).toFixed(1)}% of pixels brighter by > 16; mean brightness ${mean(a).toFixed(1)} vs ${mean(b).toFixed(1)}`);
  if (on.values.r_ulight_model !== '2') failures.push(`map did not load as hybrid (model ${on.values.r_ulight_model})`);
  if (!ids.includes(rt)) failures.push('the rtlight was not visible');
  if (d.badFraction < 0.02) failures.push('control: r_ulight 0 looks the same, the rtlight added nothing');
  if (mean(b) < 10) failures.push('the lightmapped base is black');
  if (mean(a) <= mean(b)) failures.push('the realtime light did not brighten the frame');
  const n = comparePng(readPng(nat.files[0]) && halfSize(readPng(nat.files[0])), halfSize(a), { tolerance: 32 });
  rows.push(`cart vs native: mean ${n.meanDiff.toFixed(2)}, ${(n.badFraction * 100).toFixed(2)}% beyond 32`);
  if (n.meanDiff > 4 || n.badFraction > 0.03) failures.push('cart differs from native');
  return { ok: failures.length === 0, failures, rows };
}
