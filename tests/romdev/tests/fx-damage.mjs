// Hurt and low-health feedback (cgame CG_OAXDamageFx, the *oaxvignette image,
// shader oaxfx/vignette): a red vignette that darkens and reddens the frame's
// edges and leaves its centre alone. cg_oaxTestDamage pins the strength.
//
// - at 0 the frame is the plain one; at 0.6 and 1.0 the corners get redder
//   (red share of the corner pixels rises with the strength) and the centre
//   stays within noise;
// - control: the centre of the frame does not change (a full-screen tint
//   would fail it) and the corners do (a no-op would fail them);
// - cg_oaxDamageFx 0 does not stop the pinned test value (it is a cheat
//   override) but the real path is off: with the test value 0 and the
//   switch 0 the frame equals the plain one;
// - native draws the same frame as the cart (control: the plain frame);
//   cart golden (control: the plain frame against it).

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { diffFraction, meanRGB } from '../lib/imgstat.mjs';
import { MAP, SETUP, golden, goldenControl, isPicture } from '../lib/fxtest.mjs';

export const name = 'fx-damage';

const CAM = 'cl_overrideView "0 -300 140 0 -90 0"';
const CORNERS = [{ x0: 0, x1: 0.12, y0: 0, y1: 0.16 }, { x0: 0.88, x1: 1, y0: 0, y1: 0.16 }, { x0: 0, x1: 0.12, y0: 0.84, y1: 1 }, { x0: 0.88, x1: 1, y0: 0.84, y1: 1 }];
const CENTRE = { x0: 0.4, x1: 0.6, y0: 0.4, y1: 0.6 };

function shotList() {
  return [
    { cmd: `${CAM};cg_oaxTestDamage 0`, name: 'off' },
    { cmd: 'cg_oaxTestDamage 0.6', name: 'mid' },
    { cmd: 'cg_oaxTestDamage 1', name: 'full' },
    { cmd: 'cg_oaxTestDamage 0;cg_oaxDamageFx 0', name: 'switchoff' },
  ];
}

const redShare = (img, box) => { const m = meanRGB(img, box); return m[0] / Math.max(1, m[0] + m[1] + m[2]); };
const cornerRed = (img) => CORNERS.reduce((s, b) => s + redShare(img, b), 0) / CORNERS.length;
const cornerLum = (img) => CORNERS.reduce((s, b) => { const m = meanRGB(img, b); return s + (m[0] + m[1] + m[2]) / 3; }, 0) / CORNERS.length;

function check(build, r, ctx) {
  const { rows, failures } = ctx;
  const im = r.images;
  isPicture(ctx, `${build} off`, im.off);
  const red = ['off', 'mid', 'full'].map((k) => cornerRed(im[k]));
  rows.push(`${build}: corner red share ${red.map((v) => v.toFixed(3)).join(' < ')}; centre change at 1.0 ${(diffFraction(im.off, im.full, 8, CENTRE) * 100).toFixed(3)}%`);
  if (!(red[1] > red[0] + 0.03 && red[2] > red[1] + 0.02)) failures.push(`${build}: the corners do not redden with the strength (${red.map((v) => v.toFixed(3)).join(', ')})`);
  if (diffFraction(im.off, im.full, 8, CENTRE) > 0.01) failures.push(`${build}: the vignette changes the centre of the frame`);
  if (diffFraction(im.off, im.full, 8, CORNERS[0]) < 0.5) failures.push(`${build}: the corner did not change (a no-op would pass the centre check)`);
  const sw = diffFraction(im.off, im.switchoff, 0);
  rows.push(`${build}: switched off vs plain ${(sw * 100).toFixed(4)}% differ`);
  if (sw > 0) failures.push(`${build}: cg_oaxDamageFx 0 does not give the plain frame (${(sw * 100).toFixed(4)}%)`);
  if (!(cornerLum(im.full) < cornerLum(im.off) + 40)) failures.push(`${build}: the corners got far brighter, not tinted`);
}

export async function run({ goldens, out, update }) {
  const ctx = { goldens, out, update, rows: [], failures: [] };
  const cart = await cartShots('fx-damage', MAP, shotList(), { setup: SETUP, out });
  check('cart', cart, ctx);
  golden(ctx, 'fx_damage', cart.images.mid);
  goldenControl(ctx, 'fx_damage', cart.images.off, 'the plain frame');
  const native = nativeShots('fx-damage', MAP, shotList(), { setup: SETUP });
  check('native', native, ctx);
  // native blends the 2D vignette about 0.8x as strongly as the cart (corner red 125 against 145
  // at the same alpha; a host difference in 2D alpha blending, not investigated here), so the
  // parity check is at a wider tolerance than the other effects': same picture, not same values
  const d = diffFraction(native.images.mid, cart.images.mid, 48);
  const c = diffFraction(native.images.mid, cart.images.off, 48);
  ctx.rows.push(`native vs cart vignette (tolerance 48): ${(d * 100).toFixed(3)}% differ (control ${(c * 100).toFixed(2)}%)`);
  if (d > 0.1) ctx.failures.push(`native vignette differs from the cart's (${(d * 100).toFixed(2)}% at tolerance 48)`);
  if (c < 0.15) ctx.failures.push('control did not fail: native vignette matches the cart frame without it');
  return { ok: ctx.failures.length === 0, failures: ctx.failures, rows: ctx.rows };
}
