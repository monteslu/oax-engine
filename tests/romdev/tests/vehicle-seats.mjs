// Vehicle seats (phase 8): the driver drives and cannot shoot, the gunner
// shoots and cannot drive, getting out puts the player back on its feet.
// On oax_vehicle_test (vehicle rule on), on the cart:
//   1. on foot, firing spends ammo (the control that firing works at all);
//   2. in the buggy's driver seat (use button), firing spends nothing and
//      the stick drives the buggy;
//   3. moved to the gunner seat (server command vehseat), firing spends
//      ammo (counted as gunner shots) while the stick moves nothing;
//   4. the use button gets the gunner out, standing beside the buggy.
// The rider's player state carries PMF_OAX_VEHICLE and STAT_OAX_VEHICLE,
// read back through the g_veh_p0 debug value (vehicle, seat, weapon, ammo).

import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { readValues } from '../lib/values.mjs';

export const name = 'vehicle-seats';

const MAP = 'oax_vehicle_test';
// pad scripts (tests/romdev/data/padscripts/seats_*.pad): fire, use, throttle
const PAD_FRAMES = { fire: 46, use: 9, throttle: 66 };

function p0(v) {
  const [veh, seat, weapon, ammo] = String(v.g_veh_p0 || '-1 -1 0 0').split(' ').map(Number);
  return { veh, seat, weapon, ammo };
}

async function hold(s, pad) {
  await s.command(`padscript padscripts/seats_${pad}.pad`);
  await s.step(PAD_FRAMES[pad] + 30);
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const s = new Session('vehicle-seats');
  try {
    await loadScene(s, MAP, { view: `${CLEAN_VIEW};set g_oaxVehicles 1` });
    await s.command('exec padscripts/padbinds.cfg');
    await s.step(120);
    const fire = 'fire', use = 'use', throttle = 'throttle';

    // 1. on foot
    let a = p0(await readValues(s));
    await hold(s, fire);
    let b = p0(await readValues(s));
    rows.push(`on foot: vehicle ${b.veh}, weapon ${b.weapon}, ammo ${a.ammo} -> ${b.ammo}`);
    if (!(b.veh < 0)) failures.push('the player started in a vehicle');
    if (!(b.ammo < a.ammo)) failures.push('control: firing on foot spent no ammo');

    // 2. driver
    await hold(s, use);
    await s.step(20);
    a = p0(await readValues(s));
    const d0 = Number((await readValues(s)).g_veh_distance);
    await hold(s, fire);
    b = p0(await readValues(s));
    rows.push(`driver: vehicle ${b.veh} seat ${b.seat}, ammo ${a.ammo} -> ${b.ammo} while firing`);
    if (!(b.veh >= 0 && b.seat === 0)) failures.push(`use did not seat the player as driver (vehicle ${b.veh} seat ${b.seat})`);
    if (b.ammo !== a.ammo) failures.push('the driver fired a weapon');
    await hold(s, throttle);
    const d1 = Number((await readValues(s)).g_veh_distance);
    rows.push(`driver: the stick drove the buggy ${(d1 - d0).toFixed(1)} units`);
    if (!(d1 - d0 > 100)) failures.push(`the driver's stick moved the buggy only ${(d1 - d0).toFixed(1)} units`);

    // 3. gunner (the buggy's second seat)
    await s.command('vehseat 0 1');
    await s.step(30);
    const g0 = await readValues(s);
    a = p0(g0);
    await hold(s, fire);
    const g1 = await readValues(s);
    b = p0(g1);
    rows.push(`gunner: vehicle ${b.veh} seat ${b.seat}, ammo ${a.ammo} -> ${b.ammo}, gunner shots ${g0.g_veh_gunner_shots} -> ${g1.g_veh_gunner_shots}`);
    if (!(b.veh >= 0 && b.seat === 1)) failures.push(`vehseat did not seat the player as gunner (vehicle ${b.veh} seat ${b.seat})`);
    if (!(b.ammo < a.ammo)) failures.push('the gunner could not shoot');
    if (!(Number(g1.g_veh_gunner_shots) > Number(g0.g_veh_gunner_shots))) failures.push('no gunner shots counted');
    const before = Number(g1.g_veh_distance);
    await hold(s, throttle);
    const after = Number((await readValues(s)).g_veh_distance);
    rows.push(`gunner: the stick moved the buggy ${(after - before).toFixed(1)} units (driverless, parked)`);
    if (after - before > 1) failures.push('the gunner drove the buggy');

    // 4. out
    await hold(s, use);
    await s.step(20);
    const o = await readValues(s);
    b = p0(o);
    const at = await s.read('player_origin');
    const veh = String(o.g_veh0 || '').split(' ').map(Number);
    const apart = Math.hypot(at[0] - veh[1], at[1] - veh[2]);
    rows.push(`out: vehicle ${b.veh}, exits ${o.g_veh_exits}, standing ${apart.toFixed(1)} units from the buggy's center`);
    if (b.veh >= 0) failures.push('use did not get the gunner out');
    if (!(apart > 60 && apart < 200)) failures.push(`the player got out ${apart.toFixed(1)} units from the buggy`);
    await s.screenshot(`${out}/vehicle-seats.png`);
  } finally {
    await s.shutdown();
  }
  return { ok: failures.length === 0, failures, rows };
}
