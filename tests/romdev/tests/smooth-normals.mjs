// oax_smoothnormals <degrees> (renderer, R_OAXSmoothWorldNormals): a world's
// flat faces and triangle soups take angle-limited smooth vertex normals, so a
// low-poly curve shades smoothly under dynamic lights. oax_smooth (the key at
// 60 degrees) against oax_smooth_flat (no key): the same twelve-sided pillar
// and the same realtime light.
//   - across the pillar the lit side's brightness has far smaller steps from
//     one pixel to the next when smoothed than when flat (the facet edges);
//   - outside the pillar the two frames are the same;
//   - the cart renders the smoothed pillar like the native build.

import { shootCart, shootNative, readPng } from '../lib/ulight.mjs';
import { comparePng, halfSize } from '../lib/png.mjs';

export const name = 'smooth-normals';

const VIEW = '-300 0 128 0 0 0';
const PLAYER = '-400 0 32 0';
const PILLAR = { x0: 0.38, x1: 0.62, y0: 0.30, y1: 0.70 };

const luma = (img, i) => (img.data[i] * 0.299 + img.data[i + 1] * 0.587 + img.data[i + 2] * 0.114);

// the largest pixel to pixel brightness step along rows inside the box, in
// the 90th percentile of the rows (one number for the whole pillar's face)
function maxStep(img, box) {
  const x0 = Math.floor(box.x0 * img.width), x1 = Math.floor(box.x1 * img.width), y0 = Math.floor(box.y0 * img.height), y1 = Math.floor(box.y1 * img.height);
  const steps = [];
  for (let y = y0; y < y1; y++) {
    let m = 0;
    for (let x = x0 + 1; x < x1; x++) m = Math.max(m, Math.abs(luma(img, (y * img.width + x) * 4) - luma(img, (y * img.width + x - 1) * 4)));
    steps.push(m);
  }
  steps.sort((a, b) => a - b);
  return steps[Math.floor(steps.length * 0.9)];
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const smooth = await shootCart('oax_smooth', [VIEW], { name: 'smooth-on', out, player: PLAYER });
  const flat = await shootCart('oax_smooth_flat', [VIEW], { name: 'smooth-off', out, player: PLAYER });
  const nat = shootNative('oax_smooth', [VIEW], { name: 'smooth-native', out, player: PLAYER });
  const a = readPng(smooth.files[0]), b = readPng(flat.files[0]);
  const sa = maxStep(a, PILLAR), sb = maxStep(b, PILLAR);
  rows.push(`pillar: largest brightness step (p90 of rows) ${sa.toFixed(1)} smoothed, ${sb.toFixed(1)} flat; model ${smooth.values.r_ulight_model}`);
  if (smooth.values.r_ulight_model !== '2') failures.push(`map did not load as hybrid (model ${smooth.values.r_ulight_model})`);
  if (!(sb > 12)) failures.push(`control: the flat pillar shows no facet steps (${sb.toFixed(1)})`);
  if (!(sa < sb * 0.5)) failures.push(`smoothing did not remove the facet steps (${sa.toFixed(1)} vs ${sb.toFixed(1)})`);
  // outside the pillar the frames agree: compare the left and right fifths
  const side = { x0: 0, x1: 0.2, y0: 0, y1: 1 };
  const crop = (img, box) => { const x0 = Math.floor(box.x0 * img.width), x1 = Math.floor(box.x1 * img.width); const w = x1 - x0; const data = Buffer.alloc(w * img.height * 4); for (let y = 0; y < img.height; y++) img.data.copy(data, y * w * 4, (y * img.width + x0) * 4, (y * img.width + x1) * 4); return { width: w, height: img.height, data }; };
  const d = comparePng(crop(a, side), crop(b, side), { tolerance: 8 });
  rows.push(`outside the pillar (left fifth): ${(d.badFraction * 100).toFixed(2)}% of pixels differ by > 8`);
  if (d.badFraction > 0.02) failures.push('the smoothing changed pixels away from the pillar');
  const n = comparePng(halfSize(readPng(nat.files[0])), halfSize(a), { tolerance: 32 });
  rows.push(`cart vs native: mean ${n.meanDiff.toFixed(2)}, ${(n.badFraction * 100).toFixed(2)}% beyond 32`);
  if (n.meanDiff > 4 || n.badFraction > 0.03) failures.push('cart differs from native');
  return { ok: failures.length === 0, failures, rows };
}
