// Ribbon trails (phase 6): the cgame keeps each trailed entity's trajectory
// history (16 ms steps of cg.time, re-evaluated when the trajectory
// changes) and adds the points every frame; the renderer builds a strip
// facing each view's camera, its width and colour running over its life.
//
// Checks, each with a control:
// - the map's bobbing brush model asks for a trail with the `oaxtrail` key
//   (CS_OAX_TRAILS: cg_map_trails 1) and gets one (r_trails, points);
// - rockets, grenades (also after bouncing off the wall: the history is
//   re-evaluated on the new trajectory) and plasma bolts in flight trail
//   (r_trails counts them beside the map trail); control: cg_oaxTrails 0
//   stops every trail;
// - a fixed test trail (cg_oaxTestTrail, no clock involved) draws, and
//   r_oaxTrails 0 removes exactly its pixels (the frame equals the
//   cg_oaxTrails 0 frame);
// - native draws the same test trail as the cart (control: the frame
//   without it).

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { diffFraction } from '../lib/imgstat.mjs';
import { MAP, SETUP, golden, nativeMatchesCart, isPicture, num } from '../lib/fxtest.mjs';

export const name = 'fx-trails';

const CAM_T = 'cl_overrideView "60 -150 170 8 105 0"';
const TEST = 'cg_oaxTestTrail "oaxfx/trailSmoke 200 430 150 -260 430 100 24 800"';
const TRAIL_BOX = { x0: 0.2, x1: 0.85, y0: 0.25, y1: 0.6 };

function shotList(k = 1) {
  return [
    // cosmetic physics (debris from the rocket still settling) would move
    // between the trails-off shots that must match exactly
    { cmd: 'cg_physics 0;setviewpos 300 0 40 0;give all;cl_overrideView "600 -420 140 12 90 0"', settle: 20, name: 'map', values: 'map' },
    { cmd: 'weapon 5', settle: 40 * k },
    { cmd: '+attack', settle: 2 * k },
    { cmd: '-attack', settle: 10 * k, name: 'rocket', values: 'rocket' },
    { cmd: 'weapon 4', settle: 90 * k },
    { cmd: '+attack', settle: 4 * k },
    { cmd: '-attack', settle: 10 * k, name: 'grenade', values: 'grenade' },
    { settle: 70 * k, name: 'bounced', values: 'bounced' },
    { cmd: 'weapon 8', settle: 60 * k },
    { cmd: '+attack', settle: 12 * k },
    { cmd: '-attack', settle: 2 * k, name: 'plasma', values: 'plasma' },
    { cmd: 'cg_oaxTrails 0', settle: 40 * k },
    { cmd: '+attack', settle: 12 * k },
    { cmd: '-attack', settle: 2 * k, name: 'plasmaoff', values: 'plasmaoff' },
    { cmd: `cg_oaxTrails 1;${CAM_T};${TEST}`, settle: 40 * k, name: 'test', values: 'test' },
    { cmd: 'cg_oaxTrails 0', name: 'testoff', values: 'testoff' },
    { cmd: 'cg_oaxTrails 1;r_oaxTrails 0', name: 'roff', values: 'roff' },
    { cmd: `r_oaxTrails 1;cg_oaxTestTrail ""` },
  ];
}

function checkBuild(build, r, ctx) {
  const { rows, failures } = ctx;
  const im = r.images, v = r.valuesAt;
  isPicture(ctx, `${build} test`, im.test);
  rows.push(`${build}: map trail entities ${v.map.cg_map_trails}, trails ${v.map.r_trails} (${v.map.r_trail_points} points); rocket in flight: ${v.rocket.r_trails}; plasma: ${v.plasma.r_trails}; cg_oaxTrails 0 with plasma in flight: ${v.plasmaoff.r_trails}`);
  if (num(v.map.cg_map_trails) !== 1 || num(v.map.r_trails) < 1 || num(v.map.r_trail_points) < 10) failures.push(`${build}: the map entity's trail did not draw`);
  if (num(v.rocket.r_trails) < 2) failures.push(`${build}: the rocket did not trail`);
  rows.push(`${build}: grenade in flight: ${v.grenade.r_trails} trails, after its bounce: ${v.bounced.r_trails} (${v.bounced.r_trail_points} points)`);
  if (num(v.grenade.r_trails) < 2 || num(v.bounced.r_trails) < 2) failures.push(`${build}: the grenade did not trail`);
  if (num(v.plasma.r_trails) < 2) failures.push(`${build}: the plasma bolts did not trail`);
  if (num(v.plasmaoff.r_trails) !== 0) failures.push(`${build}: control: cg_oaxTrails 0 still trails`);
  const flight = diffFraction(im.plasma, im.plasmaoff, 24);
  rows.push(`${build}: plasma frame with trails vs without ${(flight * 100).toFixed(2)}% differ`);
  if (flight < 0.002) failures.push(`${build}: the plasma trail does not show`);

  const t = diffFraction(im.test, im.testoff, 8, TRAIL_BOX);
  const same = diffFraction(im.testoff, im.roff, 0);
  rows.push(`${build}: test trail ${(t * 100).toFixed(2)}% of its box; r_oaxTrails 0 vs cg_oaxTrails 0 ${(same * 100).toFixed(4)}% differ (trails ${v.roff.r_trails})`);
  if (t < 0.01 || num(v.test.r_trails) < 1) failures.push(`${build}: the test trail does not draw`);
  if (same > 0 || num(v.roff.r_trails) !== 0) failures.push(`${build}: r_oaxTrails 0 does not remove the trails`);
}

export async function run({ goldens, out, update }) {
  const ctx = { goldens, out, update, rows: [], failures: [] };
  const cart = await cartShots('fx-trails', MAP, shotList(), { setup: SETUP, out });
  checkBuild('cart', cart, ctx);
  golden(ctx, 'fx_trail', cart.images.test);
  const native = nativeShots('fx-trails', MAP, shotList(2), { setup: SETUP });
  checkBuild('native', native, ctx);
  nativeMatchesCart(ctx, 'test trail', native.images.test, cart.images.test, cart.images.testoff, { box: TRAIL_BOX, limit: 0.01 });
  return { ok: ctx.failures.length === 0, failures: ctx.failures, rows: ctx.rows };
}
