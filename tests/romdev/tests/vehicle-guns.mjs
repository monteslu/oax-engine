// Mounted vehicle guns and seat views (cg_oax_vehicle.c, g_oax_vehicle.c
// G_VehGunThink). On oax_vehicle_test (vehicle rule on), native and cart run
// the same console script:
//   0. seats: in the buggy's driver seat, jump (A) moves to the gunner
//      seat, the number keys (weapon 1, weapon 2) pick seats, a tap of use
//      keeps the player in (getting out takes a hold);
//   1. the buggy's gunner seat: first person, on the gun (cg_veh_view);
//   2. control: the gun turned away from the hover craft fires and hits
//      nothing (so the hits below are the aim's, not the counter's);
//   3. aimed at the hover craft (vehaim ... veh 1): the machine gun fires
//      (shots counted, the gunner's own ammo untouched), hits it, and its
//      health drops; the cgame sees the same shot count the server fired;
//   4. held down it overheats: the server counts the lock, the cgame sees
//      the heat's lock bit, fewer shots leave than the hold allows at the
//      gun's rate, then it cools and unlocks;
//   5. toggleview switches the seat to the chase camera and back;
//   6. the hover craft's driver is on its plasma guns: chase camera by
//      default, firing counts shots, the own ammo untouched.
// The two builds step frames of different lengths of game time, so every
// rate is measured on the server's own clock (the vehicle world time in
// g_veh0_exact), and each build is checked against these rules rather than
// against the other's counts.

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';

export const name = 'vehicle-guns';

const MAP = 'oax_vehicle_test';
const SETUP = ['set g_oaxVehicles 1', 'cg_oaxVehView 0', 'cg_draw2D 1'];

function shots() {
  return [
    { cmd: 'vehseat 0 0', settle: 30, values: 'driver' },
    { cmd: '+moveup', settle: 10 },
    { cmd: '-moveup', settle: 20, values: 'swapped' },
    { cmd: 'weapon 1', settle: 20, values: 'keyed' },
    { cmd: '+button2', settle: 2 },
    { cmd: '-button2', settle: 20, values: 'tapped' },
    { cmd: 'weapon 2', settle: 30, values: 'gunner' },
    { cmd: 'vehaim 0 0 -90', settle: 10 },
    { cmd: '+attack', settle: 20 },
    { cmd: '-attack', settle: 10, values: 'control' },
    { cmd: 'vehaim 0 veh 1', settle: 10 },
    { cmd: '+attack', settle: 40 },
    { cmd: '-attack', settle: 20, values: 'aimed', name: 'aimed' },
    { cmd: '+attack', settle: 600, values: 'hot' },
    { cmd: '-attack', settle: 10, values: 'released' },
    { cmd: 'wait', settle: 400, values: 'cooled' },
    { cmd: 'toggleview', settle: 10, values: 'toggled', name: 'chase' },
    { cmd: 'toggleview', settle: 10, values: 'toggled2' },
    { cmd: 'cg_oaxVehView 0;+button2', settle: 60 },
    { cmd: '-button2', settle: 30, values: 'out' },
    { cmd: 'vehplace 0 -1600 420 120', settle: 60 },
    { cmd: 'vehseat 0 0', settle: 30, values: 'hover' },
    { cmd: '+attack', settle: 90 },
    { cmd: '-attack', settle: 10, values: 'plasma', name: 'plasma' },
  ];
}

const num = (v, k) => Number(v?.[k]);
const list = (v, k) => String(v?.[k] ?? '').split(' ').map(Number);
const p0 = (v) => { const [veh, seat, weapon, ammo] = list(v, 'g_veh_p0'); return { veh, seat, weapon, ammo }; };
const view = (v) => { const [seat, fp, gun] = list(v, 'cg_veh_view'); return { seat, fp, gun }; };
const cgGun = (v) => { const [shots, heat] = list(v, 'cg_veh_gun'); return { shots, heat }; };
// g_veh1 is the hover craft (spawned second): health is the 8th number
const hoverHealth = (v) => list(v, 'g_veh1')[7];
// the vehicle world's time, ms (the last number of g_veh0_exact)
const worldTime = (v) => list(v, 'g_veh0_exact').at(-1);

function check(build, at, rows, failures) {
  const f = (msg) => failures.push(`${build}: ${msg}`);
  const seat = (k) => p0(at[k]).seat;
  rows.push(`${build}: seats: driver ${seat('driver')}, jump -> ${seat('swapped')}, weapon 1 -> ${seat('keyed')}, ` +
    `a tap of use -> ${seat('tapped')}, weapon 2 -> ${seat('gunner')}, held use -> ${seat('out')}; ` +
    `swaps ${at.gunner.g_veh_seat_swaps}, enters ${at.gunner.g_veh_enters}`);
  if (seat('driver') !== 0) f(`vehseat 0 0 gave seat ${seat('driver')}`);
  if (seat('swapped') !== 1) f(`jump moved the driver to seat ${seat('swapped')}, not the gunner seat`);
  if (seat('keyed') !== 0) f(`weapon 1 gave seat ${seat('keyed')}`);
  if (seat('tapped') !== 0) f('a tap of use got the player out');
  if (seat('gunner') !== 1) f(`weapon 2 gave seat ${seat('gunner')}`);
  if (!(p0(at.out).veh < 0)) f('holding use did not get the player out');
  if (num(at.gunner, 'g_veh_seat_swaps') !== 3) f(`${at.gunner.g_veh_seat_swaps} seat swaps counted (want 3)`);
  if (num(at.gunner, 'g_veh_enters') !== 1) f(`seat swaps counted as entries (${at.gunner.g_veh_enters})`);
  const g = at.gunner, c = at.control, a = at.aimed, h = at.hot, k = at.cooled;
  const vg = view(g);
  rows.push(`${build}: gunner seat ${p0(g).seat}, first person ${vg.fp}, on the gun ${vg.gun}`);
  if (!(p0(g).seat === 1 && vg.fp === 1 && vg.gun === 1)) f(`gunner view ${g?.cg_veh_view} seat ${g?.g_veh_p0}`);

  const cShots = num(c, 'g_veh_gun_shots'), cHits = num(c, 'g_veh_gun_hits');
  rows.push(`${build}: control (turned away): ${cShots} shots, ${cHits} hits`);
  if (!(cShots >= 2)) f(`the control fired ${cShots} shots`);
  if (cHits !== 0) f(`the gun turned away hit something ${cHits} times`);

  const aShots = num(a, 'g_veh_gun_shots') - cShots, aHits = num(a, 'g_veh_gun_hits') - cHits;
  rows.push(`${build}: aimed: ${aShots} shots, ${aHits} hits, hover health ${hoverHealth(g)} -> ${hoverHealth(a)}, ` +
    `gunner ammo ${p0(g).ammo} -> ${p0(a).ammo}, cgame shot count ${cgGun(a).shots} (server ${num(a, 'g_veh_gun_shots')})`);
  if (!(aShots >= 3)) f(`aimed fire: ${aShots} shots`);
  if (!(aHits >= aShots / 2)) f(`aimed fire hit ${aHits} of ${aShots}`);
  if (!(hoverHealth(a) < hoverHealth(g))) f(`the hover craft took no damage (${hoverHealth(g)} -> ${hoverHealth(a)})`);
  if (p0(a).ammo !== p0(g).ammo) f('the gunner spent its own ammo');
  if (!(num(a, 'g_veh_gunner_shots') >= num(a, 'g_veh_gun_shots'))) f('gun shots not counted as gunner shots');
  if (cgGun(a).shots !== num(a, 'g_veh_gun_shots')) f(`the cgame saw ${cgGun(a).shots} shots, the server fired ${num(a, 'g_veh_gun_shots')}`);

  const hShots = num(h, 'g_veh_gun_shots') - num(a, 'g_veh_gun_shots');
  const heldMs = worldTime(h) - worldTime(a), allowed = Math.floor(heldMs / 75);
  rows.push(`${build}: held ${heldMs} ms: ${hShots} shots of ${allowed} at the gun's rate, overheats ${num(h, 'g_veh_gun_overheats')}, ` +
    `locks seen by the cgame ${num(h, 'cg_veh_gun_locks')}; after cooling: heat ${cgGun(k).heat}`);
  if (!(heldMs >= 3000)) f(`the hold lasted ${heldMs} ms of game time, too short to overheat`);
  if (!(num(h, 'g_veh_gun_overheats') >= 1)) f('holding the trigger never overheated the gun');
  if (!(num(h, 'cg_veh_gun_locks') >= 1)) f('the cgame never saw the overheat lock');
  if (!(hShots > 10 && hShots < allowed * 0.85)) f(`${hShots} shots in a ${heldMs} ms hold (${allowed} without the lock)`);
  if (!(cgGun(k).heat < 128)) f(`still locked after cooling (${cgGun(k).heat})`);

  const t1 = view(at.toggled), t2 = view(at.toggled2);
  rows.push(`${build}: toggleview: first person ${vg.fp} -> ${t1.fp} -> ${t2.fp}`);
  if (!(t1.fp === 0 && t2.fp === 1)) f(`toggleview went ${vg.fp} -> ${t1.fp} -> ${t2.fp}`);

  const hv = at.hover, pl = at.plasma, vh = view(hv);
  const pShots = num(pl, 'g_veh_gun_shots') - num(hv, 'g_veh_gun_shots');
  rows.push(`${build}: hover driver seat ${p0(hv).seat} vehicle ${p0(hv).veh}, first person ${vh.fp}, on the gun ${vh.gun}; ` +
    `plasma ${pShots} shots, ammo ${p0(hv).ammo} -> ${p0(pl).ammo}`);
  if (!(p0(hv).seat === 0 && vh.gun === 1 && vh.fp === 0)) f(`hover driver view ${hv?.cg_veh_view} seat ${hv?.g_veh_p0}`);
  if (!(pShots >= 3)) f(`the hover's plasma guns fired ${pShots} shots`);
  if (p0(pl).ammo !== p0(hv).ammo) f('the hover driver spent its own ammo');
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const cart = await cartShots('vehicle-guns', MAP, shots(), { setup: SETUP, out });
  const native = nativeShots('vehicle-guns', MAP, shots(), {
    setup: SETUP.filter((c) => !c.startsWith('set g_oaxVehicles')), startArgs: ['+set', 'g_oaxVehicles', '1'],
  });
  for (const [b, r] of [['cart', cart], ['native', native]]) {
    for (const k of ['driver', 'swapped', 'keyed', 'tapped', 'out', 'gunner', 'control', 'aimed', 'hot', 'released', 'cooled', 'toggled', 'toggled2', 'hover', 'plasma']) {
      if (!r.valuesAt[k] || !Object.keys(r.valuesAt[k]).length) failures.push(`${b}: no values at ${k}`);
    }
  }
  if (failures.length) return { ok: false, failures, rows };
  check('cart', cart.valuesAt, rows, failures);
  check('native', native.valuesAt, rows, failures);
  return { ok: failures.length === 0, failures, rows };
}
