// Sky portals (misc_oax_skyportal, design 2.1) on both builds.
//
// oax_skyportal's arena sky shows a sealed sky room through a portal: a
// nebula skybox, a flat green "moon" block 50 degrees up toward yaw 45 and
// movers, turning 6 degrees a second. cg_oaxSkyPortalTime pins that turn.
//
// Checks, each next to a control that must fail:
// - the oax QVMs spawned and drew the portal (g_skyportal, cg_skyportal);
// - the moon is centred on screen when the camera faces its turned yaw
//   (45 + 6 t) at t = 0, 2.5 s and 5 s; control: at 5 s the yaw-45 camera
//   sees it well off centre (the sky did turn);
// - portal sky is the dark nebula, r_oaxSkyPortal 0 shows the moon1
//   fallback skybox (blue) and no moon;
// - stock QVMs (OA_STOCK_QVM_DIR, native) run the map without error and
//   show the fallback skybox;
// - cart goldens from three cameras at fixed shader and sky time; control:
//   a view against another view's golden, and the portal-off view against
//   the portal-on golden, both fail.

import fs from 'node:fs';
import path from 'node:path';
import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { comparePng, readPng, halfSize, writePng } from '../lib/png.mjs';
import { centroid, fraction } from '../lib/imgstat.mjs';
import { MOON_DIR, SKY_ROTATE_YAW } from '../../maps/src/oax_skyportal.mjs';

export const name = 'oax-skyportal';

const MAP = 'oax_skyportal';
const SETUP = ['r_fixedShaderTime 100', 'r_autoExposure 0', 'cg_oaxSkyPortalTime 0'];
const GOLDEN_CAMERAS = ['-320 -320 58 -50 45 0', '320 320 58 -50 225 0', '0 -400 58 -60 90 0'];
const MOON_EYE = '-320 -320 58';
const TOLERANCE = 24;
const MAX_BAD = 0.005;

const isMoon = (r, g, b) => g > 110 && g > 1.6 * r && g > 1.6 * b;
const isFallbackBlue = (r, g, b) => b > 90 && b > 3 * r && b > 3 * g;
const SKY = { y1: 0.45 };    // the upper part of the frame: sky in every moon view

function moonView(t) {
  return `cl_overrideView "${MOON_EYE} ${-MOON_DIR.pitch} ${MOON_DIR.yaw + SKY_ROTATE_YAW * t} 0"`;
}

function shots() {
  const list = [];
  GOLDEN_CAMERAS.forEach((c, i) => list.push({ cmd: `cl_overrideView "${c}"`, name: `golden${i}` }));
  for (const t of [0, 2.5, 5]) list.push({ cmd: `cg_oaxSkyPortalTime ${t * 1000};${moonView(t)}`, name: `moon${t * 10}` });
  list.push({ cmd: `cg_oaxSkyPortalTime 5000;${moonView(0)}`, name: 'moonControl' });
  list.push({ cmd: `cg_oaxSkyPortalTime 0;r_oaxSkyPortal 0;cl_overrideView "${GOLDEN_CAMERAS[0]}"`, name: 'off0' });
  list.push({ cmd: moonView(0), name: 'offMoon', values: 'end' });
  return list;
}

// property checks shared by both builds
function checkBuild(build, r, failures, rows) {
  const v = r.valuesAt?.end || r.values;
  if (v.g_skyportal !== '1') failures.push(`${build}: game published no g_skyportal (${v.g_skyportal})`);
  if (v.cg_skyportal !== '1') failures.push(`${build}: cgame drew no sky portal (cg_skyportal ${v.cg_skyportal})`);

  for (const t of [0, 2.5, 5]) {
    const img = r.images[`moon${t * 10}`];
    if (!img) { failures.push(`${build}: no moon frame at t=${t}`); continue; }
    const c = centroid(img, isMoon);
    rows.push(`${build}: t=${t}s camera yaw ${MOON_DIR.yaw + SKY_ROTATE_YAW * t}: moon ${c.n} px at ${c.x.toFixed(3)}, ${c.y.toFixed(3)}`);
    if (c.n < 300 || Math.abs(c.x - 0.5) > 0.03 || Math.abs(c.y - 0.5) > 0.05) failures.push(`${build}: moon not centred at t=${t} (${c.n} px at ${c.x.toFixed(3)}, ${c.y.toFixed(3)})`);
  }
  const ctl = centroid(r.images.moonControl, isMoon);
  rows.push(`${build}: control t=5s camera yaw 45: moon at ${ctl.x.toFixed(3)} (must be off centre)`);
  if (ctl.n > 0 && Math.abs(ctl.x - 0.5) < 0.1) failures.push(`${build}: control did not fail: the sky did not turn (moon at ${ctl.x.toFixed(3)})`);

  const on = r.images.moon0, off = r.images.offMoon;
  const blueOn = fraction(on, isFallbackBlue, SKY), blueOff = fraction(off, isFallbackBlue, SKY);
  const moonOff = centroid(off, isMoon).n;
  rows.push(`${build}: fallback-blue sky ${(blueOn * 100).toFixed(1)}% with the portal, ${(blueOff * 100).toFixed(1)}% with r_oaxSkyPortal 0 (moon ${moonOff} px)`);
  if (blueOn > 0.05) failures.push(`${build}: portal view shows the fallback sky (${(blueOn * 100).toFixed(1)}%)`);
  if (blueOff < 0.4 || moonOff > 50) failures.push(`${build}: r_oaxSkyPortal 0 did not fall back to the skybox (${(blueOff * 100).toFixed(1)}% blue, moon ${moonOff} px)`);
}

function stockQvmRun(failures, rows) {
  const dir = process.env.OA_STOCK_QVM_DIR;
  if (!dir) {
    rows.push('stock QVMs: skipped (set OA_STOCK_QVM_DIR to a stock OpenArena gamecode build)');
    return;
  }
  const r = nativeShots('skyportal-stock', MAP, [{ cmd: moonView(0), name: 'stock' }], { qvmDir: dir, setup: SETUP });
  const img = r.images.stock;
  const log = fs.readFileSync(path.join(r.home, 'native.err'), 'utf8');
  if (!img) { failures.push('stock QVMs: no frame (map did not load?)'); return; }
  const blue = fraction(img, isFallbackBlue, SKY), moon = centroid(img, isMoon).n;
  rows.push(`stock QVMs (native): fallback-blue sky ${(blue * 100).toFixed(1)}%, moon ${moon} px, g_skyportal ${r.values.g_skyportal ?? 'absent'}`);
  if (/ERROR|Bad game system trap|Bad cgame system trap/.test(log)) failures.push('stock QVMs: the log reports an error');
  if (blue < 0.4 || moon > 50) failures.push(`stock QVMs: no fallback skybox (${(blue * 100).toFixed(1)}% blue, moon ${moon} px)`);
  if (r.values.g_skyportal) failures.push('stock QVMs: control: a stock game published g_skyportal');
}

// the native half alone (also usable without a romdev server)
export function nativeChecks(failures, rows) {
  const native = nativeShots('oax-skyportal', MAP, shots(), { setup: SETUP });
  checkBuild('native', native, failures, rows);
  stockQvmRun(failures, rows);
  return native;
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const gdir = path.join(goldens, 'oax');
  fs.mkdirSync(gdir, { recursive: true });

  const cart = await cartShots('oax-skyportal', MAP, shots(), { setup: SETUP, out });
  checkBuild('cart', cart, failures, rows);

  // cart goldens
  GOLDEN_CAMERAS.forEach((c, i) => {
    const golden = path.join(gdir, `skyportal_${i}.png`);
    const half = halfSize(cart.images[`golden${i}`]);
    if (update || !fs.existsSync(golden)) {
      writePng(golden, half);
      rows.push(`cart golden ${i} ${update ? 'updated' : 'created'} (${c})`);
      return;
    }
    const r = comparePng(readPng(golden), half, { tolerance: TOLERANCE, diffPath: path.join(out, `oax-skyportal_${i}.diff.png`) });
    rows.push(`cart golden ${i}: ${(r.badFraction * 100).toFixed(3)}% over tolerance`);
    if (!r.sameSize || r.badFraction > MAX_BAD) failures.push(`cart golden ${i}: ${(r.badFraction * 100).toFixed(2)}% differ`);
  });
  const g0 = readPng(path.join(gdir, 'skyportal_0.png'));
  const ctlView = comparePng(readPng(path.join(gdir, 'skyportal_1.png')), halfSize(cart.images.golden0), { tolerance: TOLERANCE });
  const ctlOff = comparePng(g0, halfSize(cart.images.off0), { tolerance: TOLERANCE });
  rows.push(`cart controls: view 0 vs golden 1 ${(ctlView.badFraction * 100).toFixed(1)}%, portal off vs golden 0 ${(ctlOff.badFraction * 100).toFixed(1)}% differ`);
  if (ctlView.badFraction <= MAX_BAD) failures.push('cart control did not fail: view 0 matches golden 1');
  if (ctlOff.badFraction <= MAX_BAD) failures.push('cart control did not fail: r_oaxSkyPortal 0 matches the portal golden');

  nativeChecks(failures, rows);
  return { ok: failures.length === 0, failures, rows };
}
