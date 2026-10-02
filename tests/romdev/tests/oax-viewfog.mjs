// View fog, renderer side (design 2.2 fog): CG_OAX_R_SETVIEWFOG and the
// depth-based fog step in tr_postprocess, on both builds.
//
// The zone volumes that call the syscall belong to another feature; this
// drives it through the cgame test cvar cg_oaxViewFog (the same syscall),
// in oax_proc's long corridor.
//
// Checks, each next to a control that must fail:
// - the syscall reached the client (r_viewfog_density) and the renderer
//   advertises its oax_features tokens (skyportal lightstyle proc viewfog);
// - linear fog from 300 to 1200: the far end of the corridor moves toward
//   the fog colour, the floor next to the camera (closer than 300) does
//   not change; control: the near region of another camera differs;
// - the renderer test cvar r_oaxViewFog with the same parameters renders
//   the same frame as the syscall path;
// - density 0 turns it off again (matches the fog-free frame);
// - exponential fog is denser with distance;
// - cart goldens; native matches the cart; control: no-fog frame against
//   the fog golden fails.

import fs from 'node:fs';
import path from 'node:path';
import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { comparePng, readPng, halfSize, writePng } from '../lib/png.mjs';
import { diffFraction, meanDiff, meanRGB } from '../lib/imgstat.mjs';

export const name = 'oax-viewfog';

const MAP = 'oax_proc';
const SETUP = ['r_fixedShaderTime 3', 'r_autoExposure 0'];
const CAM = 'cl_overrideView "-740 0 128 0 0 0"';
const CAM2 = 'cl_overrideView "-740 0 128 0 30 0"';
const FOG = '0.5 0.6 0.7 0.9 300 1200';
const FOG_COLOR = [0.5, 0.6, 0.7];
const FAR = { x0: 0.45, y0: 0.42, x1: 0.55, y1: 0.55 };   // the corridor's end, ~1500 units away
const NEAR = { x0: 0.35, x1: 0.65, y0: 0.9 };            // floor 230-260 units away
const TOLERANCE = 24;

function shotList() {
  return [
    { cmd: `${CAM};cg_oaxViewFog ""`, name: 'nofog' },
    { cmd: `cg_oaxViewFog "${FOG}"`, name: 'fog', values: 'fog' },
    { cmd: CAM2, name: 'fog2' },
    { cmd: `${CAM};cg_oaxViewFog "";r_oaxViewFog "${FOG}"`, name: 'cvarfog' },
    { cmd: 'r_oaxViewFog ""', name: 'off', values: 'off' },
    { cmd: 'cg_oaxViewFog "0.8 0.4 0.2 0.002 0 0"', name: 'exp' },
    { cmd: 'cg_oaxViewFog ""', name: 'end', values: 'end' },
  ];
}

function checkBuild(build, r, failures, rows) {
  const v = r.valuesAt;
  const feats = (v.end.r_oax_features || '').split(/\s+/);
  rows.push(`${build}: renderer features "${v.end.r_oax_features}"; r_viewfog_density ${v.fog.r_viewfog_density} with fog, ${v.end.r_viewfog_density} after`);
  for (const t of ['skyportal', 'lightstyle', 'proc', 'viewfog']) if (!feats.includes(t)) failures.push(`${build}: the renderer does not advertise ${t}`);
  if (Math.abs(Number(v.fog.r_viewfog_density) - 0.9) > 1e-6) failures.push(`${build}: the fog syscall did not reach the client`);
  if (Number(v.end.r_viewfog_density) !== 0) failures.push(`${build}: clearing the fog did not reach the client`);

  const im = r.images;
  const farNo = meanRGB(im.nofog, FAR), farFog = meanRGB(im.fog, FAR);
  const dist = (c) => Math.hypot(c[0] - FOG_COLOR[0] * 255, c[1] - FOG_COLOR[1] * 255, c[2] - FOG_COLOR[2] * 255);
  const nearChange = diffFraction(im.nofog, im.fog, 8, NEAR);
  const nearCtl = diffFraction(im.fog, im.fog2, 8, NEAR);
  rows.push(`${build}: far end ${farNo.map((x) => x.toFixed(0)).join(',')} -> ${farFog.map((x) => x.toFixed(0)).join(',')} (distance to fog colour ${dist(farNo).toFixed(0)} -> ${dist(farFog).toFixed(0)}); near floor ${(nearChange * 100).toFixed(2)}% changed (control other camera ${(nearCtl * 100).toFixed(1)}%)`);
  if (dist(farFog) > dist(farNo) * 0.6) failures.push(`${build}: the far end did not move toward the fog colour`);
  if (nearChange > 0.01) failures.push(`${build}: fog changed the floor closer than its start (${(nearChange * 100).toFixed(2)}%)`);
  if (nearCtl < 0.05) failures.push(`${build}: control did not fail: another camera's near region matches`);

  const same = diffFraction(im.fog, im.cvarfog, 2);
  const offSame = diffFraction(im.nofog, im.off, 2);
  rows.push(`${build}: r_oaxViewFog vs syscall fog ${(same * 100).toFixed(3)}% differ; fog off vs no fog ${(offSame * 100).toFixed(3)}% differ`);
  if (same > 0.001) failures.push(`${build}: r_oaxViewFog renders differently from the syscall fog`);
  if (offSame > 0.001) failures.push(`${build}: density 0 did not turn the fog off`);

  // exponential: fog fraction grows with distance (centre vs lower frame)
  const expFar = meanRGB(im.exp, FAR), expNear = meanRGB(im.exp, NEAR), noNear = meanRGB(im.nofog, NEAR);
  const tint = (c, base) => (c[0] - base[0]) - (c[2] - base[2]);   // orange fog raises red over blue
  rows.push(`${build}: exponential fog tint near ${tint(expNear, noNear).toFixed(1)}, far ${tint(expFar, farNo).toFixed(1)}`);
  if (!(tint(expFar, farNo) > tint(expNear, noNear) + 20)) failures.push(`${build}: exponential fog is not denser with distance`);
}

// the native half alone (also usable without a romdev server)
export function nativeChecks(failures, rows) {
  const native = nativeShots('oax-viewfog', MAP, shotList(), { setup: SETUP });
  checkBuild('native', native, failures, rows);
  return native;
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const gdir = path.join(goldens, 'oax');
  fs.mkdirSync(gdir, { recursive: true });

  const cart = await cartShots('oax-viewfog', MAP, shotList(), { setup: SETUP, out });
  checkBuild('cart', cart, failures, rows);
  for (const n of ['fog', 'exp']) {
    const golden = path.join(gdir, `viewfog_${n}.png`);
    const half = halfSize(cart.images[n]);
    if (update || !fs.existsSync(golden)) {
      writePng(golden, half);
      rows.push(`cart golden ${n} ${update ? 'updated' : 'created'}`);
      continue;
    }
    const r = comparePng(readPng(golden), half, { tolerance: TOLERANCE, diffPath: path.join(out, `oax-viewfog_${n}.diff.png`) });
    rows.push(`cart golden ${n}: ${(r.badFraction * 100).toFixed(3)}% over tolerance`);
    if (!r.sameSize || r.badFraction > 0.005) failures.push(`cart golden ${n}: ${(r.badFraction * 100).toFixed(2)}% differ`);
  }
  const ctl = comparePng(readPng(path.join(gdir, 'viewfog_fog.png')), halfSize(cart.images.nofog), { tolerance: TOLERANCE });
  rows.push(`cart control: fog-free frame against the fog golden ${(ctl.badFraction * 100).toFixed(1)}% differ`);
  if (ctl.badFraction <= 0.005) failures.push('cart control did not fail: the fog golden matches a fog-free frame');

  const native = nativeChecks(failures, rows);
  const m = meanDiff(native.images.fog, cart.images.fog), mctl = meanDiff(native.images.fog, cart.images.nofog);
  rows.push(`native vs cart fog frame: mean difference ${m.toFixed(2)} (control against the fog-free frame ${mctl.toFixed(1)})`);
  if (m > 4) failures.push(`native fog frame differs from the cart's (mean ${m.toFixed(2)})`);
  if (mctl <= 4) failures.push('native control did not fail');
  return { ok: failures.length === 0, failures, rows };
}
