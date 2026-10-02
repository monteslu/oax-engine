// Soft particles (phase 6): particles fade by their depth in front of the
// opaque scene (a copy of the FBO's depth), so sprites crossing a wall or
// the floor lose their hard intersection line.
//
// oax_fx's smoke pile (softDistance 32) sits on the floor against a block.
// Frames: soft (default), hard (r_oaxSoftParticles 0), none (r_oaxParticles 0).
// Checks, each with a control:
// - soft takes a scene copy (r_scene_copies 1), hard takes none;
// - where the smoke crosses the block's face and the floor, soft is closer
//   to the smoke-free frame than hard is (the fade);
// - control: in open air, far from any surface, soft and hard are the same
//   frame (the fade is local, not a global dimming);
// - native matches the cart (control: the hard frame).

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { meanDiff } from '../lib/imgstat.mjs';
import { MAP, SETUP, golden, nativeMatchesCart, isPicture, num } from '../lib/fxtest.mjs';

export const name = 'fx-soft';

const CAM = 'cl_overrideView "-60 -250 50 8 70 0"';
const FACE = { x0: 0.655, x1: 0.74, y0: 0.4, y1: 0.6 };    // the smoke in front of the block's face
const FLOOR = { x0: 0.45, x1: 0.62, y0: 0.55, y1: 0.62 };  // the smoke on the floor
const AIR = { x0: 0.45, x1: 0.6, y0: 0.33, y1: 0.45 };     // the top of the cloud, in open air

function shotList() {
  return [
    { cmd: CAM, name: 'soft', values: 'soft' },
    { cmd: 'r_oaxSoftParticles 0', name: 'hard', values: 'hard' },
    { cmd: 'r_oaxParticles 0', name: 'none' },
    { cmd: 'r_oaxParticles 1;r_oaxSoftParticles 12' },
  ];
}

function checkBuild(build, r, ctx) {
  const { rows, failures } = ctx;
  const { soft, hard, none } = r.images;
  const v = r.valuesAt;
  isPicture(ctx, `${build} soft`, soft);
  rows.push(`${build}: scene copies soft ${v.soft.r_scene_copies}, hard ${v.hard.r_scene_copies}`);
  if (num(v.soft.r_scene_copies) < 1) failures.push(`${build}: soft particles took no scene copy`);
  if (num(v.hard.r_scene_copies) !== 0) failures.push(`${build}: hard particles took a scene copy`);
  for (const [what, box, min] of [['block face', FACE, 10], ['floor', FLOOR, 3]]) {
    const s = meanDiff(soft, none, box), h = meanDiff(hard, none, box);
    rows.push(`${build}: ${what}: soft ${s.toFixed(1)} from the smoke-free frame, hard ${h.toFixed(1)}`);
    if (h - s < min) failures.push(`${build}: soft particles do not fade at the ${what} (${s.toFixed(1)} vs ${h.toFixed(1)})`);
  }
  const air = meanDiff(soft, hard, AIR), airSmoke = meanDiff(soft, none, AIR);
  rows.push(`${build}: open air: soft vs hard ${air.toFixed(2)} (the smoke itself there: ${airSmoke.toFixed(1)})`);
  if (air > 1) failures.push(`${build}: soft particles change sprites far from any surface (${air.toFixed(2)})`);
  if (airSmoke < 20) failures.push(`${build}: control: no smoke in the open-air box`);
}

export async function run({ goldens, out, update }) {
  const ctx = { goldens, out, update, rows: [], failures: [] };
  const cart = await cartShots('fx-soft', MAP, shotList(), { setup: SETUP, out });
  checkBuild('cart', cart, ctx);
  golden(ctx, 'fx_soft', cart.images.soft);
  const native = nativeShots('fx-soft', MAP, shotList(), { setup: SETUP });
  checkBuild('native', native, ctx);
  nativeMatchesCart(ctx, 'soft smoke', native.images.soft, cart.images.soft, cart.images.hard, { box: FACE, limit: 0.02 });
  return { ok: ctx.failures.length === 0, failures: ctx.failures, rows: ctx.rows };
}
