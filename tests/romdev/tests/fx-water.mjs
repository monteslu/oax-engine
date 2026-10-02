// Water (phase 6): shaders with the oaxWater keyword draw with the water
// program: a planar reflection (the view mirrored in the water plane,
// rendered first), refraction of the opaque scene, two scrolling normal-map
// wave layers by shader time, and a depth tint.
//
// oax_fx's pool, seen from its edge: a red panel and a pillar above it.
// Checks, each with a control:
// - the reflection view runs and the surface draws (r_water_reflections,
//   r_water_surfs); the red panel shows in the water; control:
//   r_oaxWater 2 (refraction only) has no reflection view and no red there;
// - the waves move with shader time; frozen time renders identically;
// - r_oaxWater 0 draws the shader's own stages (control: changes the pool);
// - native matches the cart (control: the fallback frame);
// - a cart golden (control: the fallback frame).

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { diffFraction, fraction, meanRGB } from '../lib/imgstat.mjs';
import { MAP, SETUP, golden, goldenControl, nativeMatchesCart, isPicture, num } from '../lib/fxtest.mjs';

export const name = 'fx-water';

const CAM = 'cl_overrideView "-250 0 120 25 180 0"';
const POOL = { x0: 0.02, x1: 0.98, y0: 0.33, y1: 0.64 };
const REFLECT = { x0: 0.36, x1: 0.66, y0: 0.33, y1: 0.58 };
const OPEN = { x0: 0.05, x1: 0.3, y0: 0.45, y1: 0.62 };
const red = (r, g, b) => r > 150 && g < 90 && b < 90;

function shotList() {
  return [
    { cmd: CAM, name: 'water', values: 'water' },
    { settle: 30, name: 'water2' },
    { cmd: 'r_fixedShaderTime 6', name: 'later' },
    { cmd: 'r_fixedShaderTime 5;r_oaxWater 2', name: 'refract', values: 'refract' },
    { cmd: 'r_oaxWater 0', name: 'fallback', values: 'fallback' },
    { cmd: 'r_oaxWater 1' },
  ];
}

function checkBuild(build, r, ctx) {
  const { rows, failures } = ctx;
  const im = r.images, v = r.valuesAt;
  isPicture(ctx, `${build} water`, im.water);
  rows.push(`${build}: reflections ${v.water.r_water_reflections}, water surfaces ${v.water.r_water_surfs}; refraction only: ${v.refract.r_water_reflections}, ${v.refract.r_water_surfs}; fallback: ${v.fallback.r_water_surfs}`);
  if (num(v.water.r_water_reflections) !== 1 || num(v.water.r_water_surfs) < 1) failures.push(`${build}: the water did not draw with a reflection`);
  if (num(v.refract.r_water_reflections) !== 0 || num(v.refract.r_water_surfs) < 1) failures.push(`${build}: r_oaxWater 2 should draw without a reflection view`);
  if (num(v.fallback.r_water_surfs) !== 0) failures.push(`${build}: r_oaxWater 0 still draws the water program`);

  const redW = fraction(im.water, red, REFLECT), redR = fraction(im.refract, red, REFLECT);
  rows.push(`${build}: red panel reflected: ${(redW * 100).toFixed(1)}% of the reflection box; refraction only ${(redR * 100).toFixed(2)}%`);
  if (redW < 0.03) failures.push(`${build}: the red panel is not reflected`);
  if (redR > 0.003) failures.push(`${build}: control: red in the water without the reflection`);

  const frozen = diffFraction(im.water, im.water2, 0);
  const waves = diffFraction(im.water, im.later, 8, POOL);
  rows.push(`${build}: frozen time ${(frozen * 100).toFixed(4)}% differ; a second later the pool ${(waves * 100).toFixed(1)}% differs`);
  if (frozen > 0) failures.push(`${build}: frozen time does not render identically`);
  if (waves < 0.1) failures.push(`${build}: the waves do not move with shader time`);

  const fb = diffFraction(im.water, im.fallback, 8, POOL);
  const tint = meanRGB(im.refract, OPEN);
  rows.push(`${build}: fallback stages change ${(fb * 100).toFixed(1)}% of the pool; open water (refraction) rgb ${tint.map((x) => x.toFixed(0)).join(',')}`);
  if (fb < 0.3) failures.push(`${build}: r_oaxWater 0 does not change the pool`);
  if (!((tint[1] + tint[2]) / 2 > tint[0] + 5)) failures.push(`${build}: the open water is not tinted toward the water colour`);
}

export async function run({ goldens, out, update }) {
  const ctx = { goldens, out, update, rows: [], failures: [] };
  const cart = await cartShots('fx-water', MAP, shotList(), { setup: SETUP, out });
  checkBuild('cart', cart, ctx);
  golden(ctx, 'fx_water', cart.images.water);
  goldenControl(ctx, 'fx_water', cart.images.fallback, 'the fallback stages');
  const native = nativeShots('fx-water', MAP, shotList(), { setup: SETUP });
  checkBuild('native', native, ctx);
  nativeMatchesCart(ctx, 'water', native.images.water, cart.images.water, cart.images.fallback, { box: POOL, limit: 0.02 });
  return { ok: ctx.failures.length === 0, failures: ctx.failures, rows: ctx.rows };
}
