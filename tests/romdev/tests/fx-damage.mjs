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
// - native and cart agree on the corner colour (control: the plain frame);
//   no pixel golden.

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { diffFraction, meanRGB } from '../lib/imgstat.mjs';
import { MAP, SETUP, isPicture } from '../lib/fxtest.mjs';

export const name = 'fx-damage';

const CAM = 'cl_overrideView "0 -300 140 0 -90 0"';
const CORNERS = [{ x0: 0, x1: 0.12, y0: 0, y1: 0.16 }, { x0: 0.88, x1: 1, y0: 0, y1: 0.16 }, { x0: 0, x1: 0.12, y0: 0.84, y1: 1 }, { x0: 0.88, x1: 1, y0: 0.84, y1: 1 }];
const CENTRE = { x0: 0.4, x1: 0.6, y0: 0.4, y1: 0.6 };

function shotList() {
  return [
    { cmd: `${CAM};cg_oaxTestDamage 0`, name: 'off', settle: 120 },
    { cmd: 'cg_oaxTestDamage 0.3', name: 'mid', settle: 120 },
    { cmd: 'cg_oaxTestDamage 1', name: 'full', settle: 120 },
    { cmd: 'cg_oaxTestDamage 0;cg_oaxDamageFx 0', name: 'switchoff', settle: 120 },
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
  const sw = diffFraction(im.off, im.switchoff, 0);
  rows.push(`${build}: switched off vs plain ${(sw * 100).toFixed(4)}% differ`);
  if (sw > 0) failures.push(`${build}: cg_oaxDamageFx 0 does not give the plain frame (${(sw * 100).toFixed(4)}%)`);
  if (!(cornerLum(im.full) < cornerLum(im.off) + 40)) failures.push(`${build}: the corners got far brighter, not tinted`);
}

export async function run({ goldens, out, update }) {
  const ctx = { goldens, out, update, rows: [], failures: [] };
  const cart = await cartShots('fx-damage', MAP, shotList(), { setup: SETUP, out });
  check('cart', cart, ctx);
  const native = nativeShots('fx-damage', MAP, shotList(), { setup: SETUP });
  check('native', native, ctx);
  // No pixel golden: the regression guard is the corner colour. Cart and native agree on the
  // corner red share within 0.02 at every strength, and the control (the plain frame) is far from it.
  // (Until 2026-10-08 this allowed 0.2: scripted 2D shaders depth tested against the unclearable
  // window depth buffer after a direct post-process, which stippled the overlay at random.)
  for (const k of ['mid', 'full']) {
    const dr = Math.abs(cornerRed(native.images[k]) - cornerRed(cart.images[k]));
    const plain = Math.abs(cornerRed(cart.images.off) - cornerRed(cart.images[k]));
    ctx.rows.push(`native vs cart corner red share at ${k}: differ by ${dr.toFixed(3)} (control, plain frame: ${plain.toFixed(3)})`);
    if (dr > 0.02) ctx.failures.push(`native and cart disagree on the vignette at ${k} (${dr.toFixed(3)})`);
    if (plain < 0.1) ctx.failures.push(`control did not fail: the plain frame is as red as the ${k} frame`);
  }
  if (!(cornerRed(cart.images.mid) > 0.5 && cornerRed(cart.images.mid) < 0.65)) ctx.failures.push(`the cart's mid vignette is off its recorded colour (${cornerRed(cart.images.mid).toFixed(3)}, expected 0.5 to 0.65)`);
  return { ok: ctx.failures.length === 0, failures: ctx.failures, rows: ctx.rows };
}
