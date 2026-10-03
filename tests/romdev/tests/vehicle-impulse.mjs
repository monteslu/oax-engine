// Explosions push vehicles (vehicle fixes after phase 8). Splash damage
// (rocket, grenade, BFG, a vehicle blowing up) and direct hits reach a
// vehicle through G_Damage, which queues an impulse along the knockback
// direction at the hit point (G_OAXVehicleImpulse); the vehicle clock
// applies it at the first tick that starts at or after the hit's level
// time, so it is part of the deterministic authoritative world.
//
// On oax_vehicle_test (vehicle rule on) a real rocket (vehrocket: fire_rocket
// from a point) hits the ground beside the parked buggy, 44 units from its
// flank. Native and cart, the rocket fired at the end of an idle pad script
// (the same command time on both builds):
//   - the buggy takes splash damage and is shoved sideways and turned;
//   - the same run twice on the cart gives the same bits; native gives the
//     same bits as the cart;
// Control: the same run without the rocket leaves the parked buggy exactly
// where it was (the bits do not change).

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { runNative } from '../lib/native.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';

export const name = 'vehicle-impulse';

const MAP = 'oax_vehicle_test';
// the parked buggy (settled), the rocket from its right flank down into the
// ground 90 units out (its box is 46 wide)
const B = [-1600, 0, 47.81];
const ROCKET = `vehrocket ${B[0]} ${B[1] - 250} ${B[2] + 30} ${B[0]} ${B[1] - 90} ${(B[2] - 80).toFixed(2)}`;
const SETTLE = 300;

const vec = (s) => String(s || '').split(' ').map(Number);
// the state without the world time at the end
const bits = (v) => String(v.g_veh0_exact || '').split(' ').slice(0, 10).join(' ');

async function cartRun(tag, fire) {
  const s = new Session(tag);
  try {
    await loadScene(s, MAP, { view: `${CLEAN_VIEW};set g_oaxVehicles 1` });
    await s.command('exec padscripts/padbinds.cfg');
    await s.step(120);
    const before = await readValues(s);
    await s.command(fire ? `padscript padscripts/idle.pad "${ROCKET}"` : 'padscript padscripts/idle.pad');
    await s.step(30 + SETTLE);
    return { before, after: await readValues(s) };
  } finally {
    await s.shutdown();
  }
}

function nativeRun(tag, fire) {
  const home = runNative(tag, MAP, [
    'exec padscripts/padbinds.cfg', 'bot_enable 0', 'wait 200', 'debugvalues before.txt',
    `padscript padscripts/idle.pad "${fire ? `${ROCKET}; ` : ''}wait ${SETTLE * 2}; debugvalues after.txt; quit"`,
  ], { quit: false, set: { g_oaxVehicles: 1 } });
  const read = (f) => { const p = path.join(home, 'baseoa', f); return fs.existsSync(p) ? parseDebugValues(fs.readFileSync(p, 'utf8')) : {}; };
  return { before: read('before.txt'), after: read('after.txt') };
}

function yawOf(q) {
  const [x, y, z, w] = q;
  return Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)) * 180 / Math.PI;
}

function effect(r) {
  const a = vec(r.before.g_veh0_exact), b = vec(r.after.g_veh0_exact);
  return {
    moved: Math.hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]),
    side: b[1] - a[1],
    turned: yawOf(b.slice(6, 10)) - yawOf(a.slice(6, 10)),
    health: [vec(r.before.g_veh0)[7], vec(r.after.g_veh0)[7]],
    impulses: Number(r.after.g_veh_impulses),
  };
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const res = {
    cart: await cartRun('vimpulse-cart', true),
    cart2: await cartRun('vimpulse-cart2', true),
    cartNone: await cartRun('vimpulse-cart-none', false),
    native: nativeRun('vimpulse-native', true),
    nativeNone: nativeRun('vimpulse-native-none', false),
  };
  fs.writeFileSync(path.join(out, 'vehicle-impulse.json'), JSON.stringify(res, null, 1));
  for (const [k, r] of Object.entries(res)) if (!r.after.g_veh0_exact) failures.push(`${k}: no values`);
  if (failures.length) return { ok: false, failures, rows };

  for (const k of ['cart', 'native']) {
    const e = effect(res[k]);
    rows.push(`${k}: the rocket: impulses ${e.impulses}, health ${e.health[0]} -> ${e.health[1]}, the buggy moved ${e.moved.toFixed(2)} units (${e.side.toFixed(2)} sideways, away from the blast), turned ${e.turned.toFixed(2)} degrees`);
    if (e.impulses !== 1) failures.push(`${k}: ${e.impulses} impulses`);
    if (!(e.health[1] < e.health[0])) failures.push(`${k}: no splash damage`);
    if (!(e.side > 5) || !(Math.abs(e.turned) > 1)) failures.push(`${k}: the blast did not push the buggy (side ${e.side.toFixed(2)}, turned ${e.turned.toFixed(2)})`);
  }
  for (const k of ['cartNone', 'nativeNone']) {
    const r = res[k];
    rows.push(`control ${k} (no rocket): impulses ${r.after.g_veh_impulses}, the buggy ${bits(r.before) === bits(r.after) ? 'did not move (same bits)' : 'MOVED'}`);
    if (bits(r.before) !== bits(r.after) || Number(r.after.g_veh_impulses) !== 0) failures.push(`control ${k}: the buggy moved without a rocket`);
  }
  rows.push(`determinism: cart run 1 vs run 2 ${bits(res.cart.after) === bits(res.cart2.after) ? 'identical' : 'DIFFER'}; native vs cart ${bits(res.native.after) === bits(res.cart.after) ? 'identical' : 'DIFFER'} (${bits(res.cart.after)})`);
  if (bits(res.cart.after) !== bits(res.cart2.after)) failures.push('the same rocket gave different results on two cart runs');
  if (bits(res.native.after) !== bits(res.cart.after)) failures.push(`native ${bits(res.native.after)} vs cart ${bits(res.cart.after)}`);
  return { ok: failures.length === 0, failures, rows };
}
