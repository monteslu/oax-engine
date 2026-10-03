// Players collide with vehicles (vehicle fixes after phase 8). A vehicle is
// solid to pmove as an oriented box (the type's chassis and wheels:
// BG_VehOBB), on the server (the engine's "ent_obb": G_OAX_ENT_SET_OBB,
// SV_ClipHandleForEntity) and in the client's own prediction
// (CG_OAX_CM_TEMP_OBB in CG_ClipMoveToEntities) through the same engine
// brush trace. A player standing on the deck is carried with it tick by
// tick; a moving vehicle pushes players aside (or runs them over: the
// roadkill rule, unchanged).
//
// On oax_vehicle_test (vehicle rule on) the spawn faces the parked buggy's
// tail 100 units away. Native and cart:
//   1. walk: the left stick forward for a second; the player stops at the
//      buggy's tail (its box, minus the player's half width), on both
//      builds at the same x;
//   2. push: the player stands in front of the parked hover craft and it
//      drives into them (vehdrive): the player is pushed ahead of its
//      nose, never inside its box;
//   3. deck: the player stands on the deck (vehplace, standing still) and
//      stands there on the vehicle (ground entity = the buggy);
//   4. ride: the empty buggy drives (vehdrive) 1.5 s; the player is still
//      on its deck, carried as far as the buggy went;
// Control (g_oaxVehSolid 0, a cheat cvar, must fail every check): the walk
// passes through the buggy and the player falls through its deck.
// Prediction: during the blocked walk the client's own prediction stops
// where the server does (no new prediction errors, cg_mispredicts);
// control: with the client ignoring vehicles (cg_oaxVehClip 0, the server
// still solid) it walks into the buggy and is corrected.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { runNative } from '../lib/native.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';

export const name = 'vehicle-collide';

const MAP = 'oax_vehicle_test';
// the parked buggy (tests/maps/src/oax_vehicle_test.mjs, settled on its
// suspension) and its collision box (bg_oax_vehicle.c: half 72 46 30,
// offset 0 0 -16): the deck at +14
const BUGGY = { x: -1600, y: 0, z: 47.81, hx: 72, deck: 14 };
const DECK = [BUGGY.x - 30, BUGGY.y, BUGGY.z + BUGGY.deck + 24.25];
const STOP_X = BUGGY.x - BUGGY.hx - 15;		// the player's front against the tail
const DRIVE = 'vehdrive 0.5 0 1500';
// the hover craft (vehicle 1, half length 64) and a player 10 units in front of its nose
const HOVER_HX = 64;
const PUSH_AT = [-1600 + HOVER_HX + 15 + 10, 266, 24.25];
const PUSH = 'vehdrive 0.3 0 600 1';

const vec = (s) => String(s || '').split(' ').map(Number);
const p0 = (v) => { const [x, y, z, ground] = vec(v.g_veh_p0_pos); return { x, y, z, ground }; };
const veh0 = (v) => { const [num, x, y, z] = vec(v.g_veh0); const e = vec(v.g_veh0_exact); return { num, x, y, z, quat: e.slice(6, 10) }; };
// a world point in the vehicle's body space (x forward, y left, z up)
function bodySpace(veh, p) {
  const [qx, qy, qz, qw] = veh.quat;
  const axis = [
    [1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw)],
    [2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw)],
    [2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy)],
  ];
  const d = [p.x - veh.x, p.y - veh.y, p.z - veh.z];
  return axis.map((a) => a[0] * d[0] + a[1] * d[1] + a[2] * d[2]);
}

async function cartRun(tag, solid, clip = 1) {
  const s = new Session(tag);
  const at = {};
  try {
    await loadScene(s, MAP, { view: `${CLEAN_VIEW};set g_oaxVehicles 1` });
    await s.command(`exec padscripts/padbinds.cfg;g_oaxVehSolid ${solid};cg_oaxVehClip ${clip}`);
    await s.step(120);
    at.start = await readValues(s);
    await s.command('padscript padscripts/seats_throttle.pad');
    await s.step(100);
    at.walk = await readValues(s);
    await s.command(`vehplace 0 ${PUSH_AT.join(' ')}`);
    await s.step(30);
    await s.command(PUSH);
    await s.step(120);
    at.push = await readValues(s);
    await s.command(`vehplace 0 ${DECK.join(' ')}`);
    await s.step(40);
    at.deck = await readValues(s);
    await s.command(DRIVE);
    await s.step(160);
    at.ride = await readValues(s);
    return at;
  } finally {
    await s.shutdown();
  }
}

function nativeRun(tag, solid, clip = 1) {
  const home = runNative(tag, MAP, [
    'exec padscripts/padbinds.cfg', 'bot_enable 0', `g_oaxVehSolid ${solid}`, `cg_oaxVehClip ${clip}`, 'wait 200',
    'debugvalues start.txt',
    // the walk, then the rest from the end command so nothing queued
    // behind a wait holds the pad's +forward back
    `padscript padscripts/seats_throttle.pad "wait 60; debugvalues walk.txt; vehplace 0 ${PUSH_AT.join(' ')}; wait 60; ${PUSH}; wait 240; debugvalues push.txt; vehplace 0 ${DECK.join(' ')}; wait 80; debugvalues deck.txt; ${DRIVE}; wait 320; debugvalues ride.txt; quit"`,
  ], { quit: false, set: { g_oaxVehicles: 1 } });
  const read = (f) => { const p = path.join(home, 'baseoa', f); return fs.existsSync(p) ? parseDebugValues(fs.readFileSync(p, 'utf8')) : {}; };
  return { start: read('start.txt'), walk: read('walk.txt'), push: read('push.txt'), deck: read('deck.txt'), ride: read('ride.txt') };
}

// the checks; returns the failures (the control must have them all)
function check(build, at, rows) {
  const f = [];
  const w = p0(at.walk), d = p0(at.deck), r = p0(at.ride);
  const v0 = veh0(at.deck), v1 = veh0(at.ride);
  rows.push(`${build}: walk from x ${p0(at.start).x} stopped at x ${w.x} (the buggy's tail box at ${STOP_X})`);
  if (!(Math.abs(w.x - STOP_X) < 0.5)) f.push(`${build}: the walk ended at x ${w.x}, not against the buggy (${STOP_X})`);
  {
    const p = p0(at.push), [, hx] = vec(at.push.g_veh1);
    const gap = p.x - hx;
    rows.push(`${build}: push: the hover craft drove to x ${hx}, the player ${p.x.toFixed(2)} (${gap.toFixed(2)} ahead of its center, its nose box ends at ${HOVER_HX + 15}); pushed ${at.push.g_veh_pushed} ticks`);
    if (!(Number(at.push.g_veh_pushed) > 0) || !(gap > HOVER_HX + 15 - 0.5)) f.push(`${build}: the hover craft did not push the player (gap ${gap.toFixed(2)})`);
  }
  rows.push(`${build}: on the deck: z ${d.z} (deck stance ${(BUGGY.z + BUGGY.deck + 24).toFixed(2)}), ground entity ${d.ground} (buggy ${v0.num})`);
  if (d.ground !== v0.num || !(Math.abs(d.z - (BUGGY.z + BUGGY.deck + 24)) < 0.5)) f.push(`${build}: not standing on the deck (z ${d.z}, ground ${d.ground})`);
  const moved = Math.hypot(v1.x - v0.x, v1.y - v0.y), carried = Math.hypot(r.x - d.x, r.y - d.y);
  const before = bodySpace(v0, d), after = bodySpace(v1, r);
  rows.push(`${build}: ride: the buggy went ${moved.toFixed(1)} units, the player ${carried.toFixed(1)}; on the deck at body ${before.map((x) => x.toFixed(1)).join(' ')} -> ${after.map((x) => x.toFixed(1)).join(' ')}, ` +
    `ground ${r.ground}; carried ${at.ride.g_veh_carried} ticks, pushed ${at.ride.g_veh_pushed}`);
  if (!(moved > 300)) f.push(`${build}: the buggy drove only ${moved.toFixed(1)} units`);
  // still on the deck (its top at +14, the player's feet 24 under its origin;
  // a tilted deck lifts an axis-aligned box by up to its half width times the slope)
  if (r.ground !== v1.num || !(Math.abs(after[0]) < BUGGY.hx) || !(Math.abs(after[2] - (BUGGY.deck + 24)) < 6)) f.push(`${build}: the player did not ride the deck`);
  return f;
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const res = {
    cart: await cartRun('vcollide-cart', 1),
    cartOff: await cartRun('vcollide-cart-off', 0),
    native: nativeRun('vcollide-native', 1),
    nativeOff: nativeRun('vcollide-native-off', 0),
    cartNoClip: await cartRun('vcollide-cart-noclip', 1, 0),
    nativeNoClip: nativeRun('vcollide-native-noclip', 1, 0),
  };
  fs.writeFileSync(path.join(out, 'vehicle-collide.json'), JSON.stringify(res, null, 1));
  for (const b of ['cart', 'native']) {
    if (!res[b].ride.g_veh_p0_pos) { failures.push(`${b}: no values`); continue; }
    rows.push(`${b}: vehicle collision on (g_veh_solid ${res[b].start.g_veh_solid})`);
    failures.push(...check(b, res[b], rows));
  }
  for (const b of ['cartOff', 'nativeOff']) {
    if (!res[b].ride.g_veh_p0_pos) { failures.push(`${b}: no values`); continue; }
    const cf = [];
    const ctl = check(`control ${b} (g_oaxVehSolid 0)`, res[b], cf);
    rows.push(...cf);
    rows.push(`control ${b}: ${ctl.length}/4 checks fail${ctl.length === 4 ? ', as they must' : ''}`);
    if (ctl.length !== 4) failures.push(`control ${b}: with vehicle collision off only ${ctl.length} of 4 checks fail`);
  }
  // the client's prediction against the buggy
  const miss = (at) => Number(at.walk.cg_mispredicts) - Number(at.start.cg_mispredicts);
  for (const b of ['cart', 'native']) {
    const on = miss(res[b]), off = miss(res[`${b}NoClip`]);
    rows.push(`${b}: prediction errors during the blocked walk ${on}; control cg_oaxVehClip 0: ${off}, walk ended at ${p0(res[`${b}NoClip`].walk).x}`);
    if (on !== 0) failures.push(`${b}: ${on} prediction errors walking into the buggy`);
    if (!(off > 0)) failures.push(`${b}: control: a client that ignores vehicles was never corrected`);
  }
  // the walk is the same pad script on both builds
  if (res.native.walk.g_veh_p0_pos && res.cart.walk.g_veh_p0_pos) {
    const same = res.native.walk.g_veh_p0_pos === res.cart.walk.g_veh_p0_pos;
    rows.push(`walk end native vs cart: ${res.native.walk.g_veh_p0_pos} / ${res.cart.walk.g_veh_p0_pos} (${same ? 'identical' : 'DIFFER'})`);
    if (!same) failures.push('the blocked walk ends differently on native and the cart');
  }
  return { ok: failures.length === 0, failures, rows };
}
