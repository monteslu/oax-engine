// Landing from a small height comes to rest (gamecode bg_pmove.c
// PM_OAXIntoGround), on the cart AND native.
//
// The stock bug: a fall that ends within 0.25 units of a floor with no
// collision lands by the ground trace alone, with its whole fall speed
// still pointing into the floor. PM_WalkMove clipped that to the overclip
// residual and then rescaled it to the full speed ("don't decrease
// velocity on slopes"), straight up: the player bounced forever between
// Land and kickoff. Which heights hit it depends on the gravity step phase
// (on oa_dm1 at this spot: 6 and 16 units; a converted reference map's
// spawns: 19 of 60).
//
// Checks: exact placements (`setviewpos x y z yaw pitch`) 6, 8, 12 and 16
// units above the floor, and the stock teleport at the same heights, are
// standing on the world with no vertical speed after 60 frames, and stay
// there (three samples). Control: g_oaxLandFix 0 (the stock walk move)
// keeps bouncing at 6 units (some sample is airborne).

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';

export const name = 'oax-land';

const MAP = 'oa_dm1';
// a spawn point's spot on oa_dm1: floor top at z -144
const X = 280, Y = 1416, FLOOR = -144, MINS = 24;
const HEIGHTS = [6, 8, 12, 16];
const WORLD = '1022';
const SETUP = ['cl_overrideView ""'];

function shots() {
  const list = [];
  for (const h of HEIGHTS) {
    list.push({ cmd: `setviewpos ${X} ${Y} ${FLOOR + MINS + h} 180 0`, settle: 60, values: `exact${h}a` });
    list.push({ settle: 3, values: `exact${h}b` }, { settle: 3, values: `exact${h}c` });
  }
  for (const h of HEIGHTS) {
    list.push({ cmd: `setviewpos ${X} ${Y} ${FLOOR + MINS + h} 180`, settle: 60, values: `stock${h}a` });
    list.push({ settle: 3, values: `stock${h}b` }, { settle: 3, values: `stock${h}c` });
  }
  list.push({ cmd: `g_oaxLandFix 0;setviewpos ${X} ${Y} ${FLOOR + MINS + 6} 180 0`, settle: 60, values: 'ctla' });
  for (const k of 'bcdef') list.push({ settle: 3, values: `ctl${k}` });
  list.push({ cmd: 'g_oaxLandFix 1', settle: 1 });
  return list;
}

const place = (v) => {
  const p = String(v?.g_place || '').split(' ');
  return { z: +p[3], ground: p[7], vz: +p[8] };
};

function check(build, r, failures, rows) {
  const v = r.valuesAt;
  for (const kind of ['exact', 'stock']) {
    for (const h of HEIGHTS) {
      const s = ['a', 'b', 'c'].map((k) => place(v[`${kind}${h}${k}`]));
      rows.push(`${build}: ${kind} placement ${h} above the floor: ${s.map((x) => `z ${x.z.toFixed(3)} ground ${x.ground} vz ${x.vz.toFixed(3)}`).join(' | ')}`);
      if (s.some((x) => x.ground !== WORLD || Math.abs(x.vz) > 1)) failures.push(`${build}: ${kind} placement ${h} above the floor does not come to rest`);
    }
  }
  const c = ['a', 'b', 'c', 'd', 'e', 'f'].map((k) => place(v[`ctl${k}`]));
  rows.push(`${build}: control g_oaxLandFix 0, 6 above: grounds ${c.map((x) => x.ground).join(' ')}, vz ${c.map((x) => x.vz.toFixed(1)).join(' ')}`);
  if (c.every((x) => x.ground === WORLD && Math.abs(x.vz) <= 1)) failures.push(`${build}: control did not fail: the stock walk move came to rest too`);
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const cart = await cartShots('oax-land', MAP, shots(), { setup: SETUP, out });
  check('cart', cart, failures, rows);
  const native = nativeShots('oax-land', MAP, shots(), { setup: SETUP });
  check('native', native, failures, rows);
  for (const h of HEIGHTS) {
    const a = cart.valuesAt[`exact${h}a`]?.g_place, b = native.valuesAt[`exact${h}a`]?.g_place;
    if (a !== b) failures.push(`exact ${h}: native ${b} != cart ${a}`);
  }
  rows.push('native vs cart exact placements: ' + (HEIGHTS.every((h) => cart.valuesAt[`exact${h}a`]?.g_place === native.valuesAt[`exact${h}a`]?.g_place) ? 'identical' : 'DIFFER'));
  return { ok: failures.length === 0, failures, rows };
}
