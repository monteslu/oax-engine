// Bloom (phase 6): in the HDR post-process chain, before the tonemap: a
// soft-knee threshold at half size, a pyramid of 13-tap downsamples, tent
// upsamples added back up, the result added onto the scene. r_oaxBloom is
// 0 by default, so stock frames are unchanged (stock-maps proves that).
//
// oax_fx's bright panel (twice white in scene light) on a dark wall.
// Checks, each with a control:
// - off by default: no bloom passes, and turning it on runs them
//   (r_bloom_passes);
// - the halo: the wall just outside the panel brightens, the same on all
//   four sides (the pyramid is centred: within 25% left/right, top/bottom);
// - the frame's corners, far from the panel, do not change;
// - control: a threshold above the panel's light (50) gives the bloom-free
//   frame back;
// - native matches the cart (control: the bloom-free frame); cart golden.

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { meanLuma, meanDiff } from '../lib/imgstat.mjs';
import { MAP, SETUP, golden, goldenControl, nativeMatchesCart, isPicture, num } from '../lib/fxtest.mjs';

export const name = 'fx-bloom';

const CAM = 'cl_overrideView "0 -300 140 0 -90 0"';
// the panel covers x 0.427-0.573, y 0.36-0.62 of the frame: boxes just outside each side
const SIDES = {
  left: { x0: 0.39, x1: 0.42, y0: 0.42, y1: 0.56 },
  right: { x0: 0.58, x1: 0.61, y0: 0.42, y1: 0.56 },
  top: { x0: 0.46, x1: 0.54, y0: 0.31, y1: 0.35 },
  bottom: { x0: 0.46, x1: 0.54, y0: 0.63, y1: 0.67 },
};
const CORNERS = [{ x0: 0, x1: 0.15, y0: 0, y1: 0.2 }, { x0: 0.85, x1: 1, y0: 0, y1: 0.2 }, { x0: 0, x1: 0.15, y0: 0.8, y1: 1 }, { x0: 0.85, x1: 1, y0: 0.8, y1: 1 }];

function shotList() {
  return [
    { cmd: CAM, name: 'off', values: 'off' },
    { cmd: 'r_oaxBloom 1', name: 'on', values: 'on' },
    { cmd: 'r_oaxBloomThreshold 50', name: 'high' },
    { cmd: 'r_oaxBloom 0;r_oaxBloomThreshold 1' },
  ];
}

function checkBuild(build, r, ctx) {
  const { rows, failures } = ctx;
  const im = r.images, v = r.valuesAt;
  isPicture(ctx, `${build} on`, im.on);
  rows.push(`${build}: bloom passes off ${v.off.r_bloom_passes}, on ${v.on.r_bloom_passes}`);
  if (num(v.off.r_bloom_passes) !== 0) failures.push(`${build}: bloom runs by default`);
  if (num(v.on.r_bloom_passes) < 3) failures.push(`${build}: r_oaxBloom 1 runs no bloom passes`);

  const gain = Object.fromEntries(Object.entries(SIDES).map(([k, b]) => [k, meanLuma(im.on, b) - meanLuma(im.off, b)]));
  rows.push(`${build}: halo (luma gain) ${Object.entries(gain).map(([k, g]) => `${k} ${g.toFixed(1)}`).join(', ')}`);
  for (const [k, g] of Object.entries(gain)) if (g < 15) failures.push(`${build}: no halo on the ${k} of the panel`);
  const ratio = (a, b) => Math.min(a, b) / Math.max(a, b);
  if (ratio(gain.left, gain.right) < 0.75 || ratio(gain.top, gain.bottom) < 0.75) failures.push(`${build}: the halo is lopsided`);

  const corner = Math.max(...CORNERS.map((b) => meanDiff(im.on, im.off, b)));
  const high = meanDiff(im.high, im.off);
  rows.push(`${build}: frame corners change ${corner.toFixed(2)}; threshold 50 vs bloom off ${high.toFixed(3)}`);
  if (corner > 1) failures.push(`${build}: bloom changes the frame far from the panel`);
  if (high > 0.05) failures.push(`${build}: control: a threshold over the panel's light still blooms`);
}

export async function run({ goldens, out, update }) {
  const ctx = { goldens, out, update, rows: [], failures: [] };
  const cart = await cartShots('fx-bloom', MAP, shotList(), { setup: SETUP, out });
  checkBuild('cart', cart, ctx);
  golden(ctx, 'fx_bloom', cart.images.on);
  goldenControl(ctx, 'fx_bloom', cart.images.off, 'the bloom-free frame');
  const native = nativeShots('fx-bloom', MAP, shotList(), { setup: SETUP });
  checkBuild('native', native, ctx);
  nativeMatchesCart(ctx, 'bloom', native.images.on, cart.images.on, cart.images.off, { box: { x0: 0.35, x1: 0.65, y0: 0.25, y1: 0.75 }, limit: 0.01 });
  return { ok: ctx.failures.length === 0, failures: ctx.failures, rows: ctx.rows };
}
