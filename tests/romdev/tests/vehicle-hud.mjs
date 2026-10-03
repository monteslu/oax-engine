// The vehicle HUD (vehicle fixes after phase 8): a rider sees the
// vehicle's health (a bar and a percentage) above the status bar, and the
// driver also its speed (cg_oax_vehicle.c CG_OAXVehicleHUD; the health is
// the entity's frame, the speed the predicted vehicle's).
//
// On oax_vehicle_test (vehicle rule on, the 2D HUD on) the player takes the
// buggy's driver seat, then the gunner seat, then the driver seat again
// and a rocket hits the buggy. Native and cart:
//   - the HUD panel (cropped from the frame) matches the cart golden
//     goldens/oax/vehicle_hud.png (driver, parked: 100%, 0 u/s);
//   - native draws the same panel as the cart;
//   - the HUD's numbers (cg_veh_hud: vehicle, seat, health, speed) are the
//     driver's, then the gunner's (no speed), then lower health after the hit;
// Controls: the same view with the panel off (cg_oaxVehHud 0) must fail the
// golden and the native comparison, and the damaged panel must differ from
// the golden.

import fs from 'node:fs';
import path from 'node:path';
import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { comparePng, readPng, writePng } from '../lib/png.mjs';

export const name = 'vehicle-hud';

const MAP = 'oax_vehicle_test';
const SETUP = ['set g_oaxVehicles 1', 'cg_draw2D 1', 'cg_lagometer 0', 'cg_drawFPS 0', 'cg_drawTimer 0', 'r_fixedShaderTime 100'];
// the panel: 640x480 virtual (232,400)-(408,432), with a margin
const BOX = { x0: 228 / 640, x1: 412 / 640, y0: 397 / 480, y1: 433 / 480 };
const TOLERANCE = 24;
const LIMIT = 0.005;
const HIT = 'vehrocket -1600 -250 60 -1600 0 50';

function shots() {
  return [
    { cmd: 'vehseat 0 0', settle: 40, name: 'driver', values: 'driver' },
    { cmd: 'cg_oaxVehHud 0', settle: 4, name: 'nohud' },
    { cmd: 'cg_oaxVehHud 1;vehseat 0 1', settle: 30, name: 'gunner', values: 'gunner' },
    { cmd: 'vehseat 0 0', settle: 20 },
    { cmd: HIT, settle: 60, name: 'damaged', values: 'damaged' },
  ];
}

function crop(img, box = BOX) {
  const x0 = Math.floor(box.x0 * img.width), x1 = Math.ceil(box.x1 * img.width);
  const y0 = Math.floor(box.y0 * img.height), y1 = Math.ceil(box.y1 * img.height);
  const w = x1 - x0, h = y1 - y0, data = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) img.data.copy(data, y * w * 4, ((y0 + y) * img.width + x0) * 4, ((y0 + y) * img.width + x1) * 4);
  return { width: w, height: h, data };
}

const hud = (v) => { const [veh, seat, health, speed] = String(v?.cg_veh_hud || '').split(' ').map(Number); return { veh, seat, health, speed }; };

function checkValues(build, r, rows, failures) {
  const d = hud(r.valuesAt.driver), g = hud(r.valuesAt.gunner), x = hud(r.valuesAt.damaged);
  rows.push(`${build}: HUD numbers: driver seat ${d.seat} health ${d.health}% speed ${d.speed}; gunner seat ${g.seat} health ${g.health}% speed ${g.speed}; after the hit health ${x.health}%`);
  if (!(d.veh > 0 && d.seat === 0 && d.health === 100 && d.speed === 0)) failures.push(`${build}: driver HUD numbers ${r.valuesAt.driver?.cg_veh_hud}`);
  if (!(g.seat === 1 && g.health === 100 && g.speed === -1)) failures.push(`${build}: gunner HUD numbers ${r.valuesAt.gunner?.cg_veh_hud}`);
  if (!(x.health < 100 && x.health > 0)) failures.push(`${build}: the hit did not show on the HUD (${r.valuesAt.damaged?.cg_veh_hud})`);
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const cart = await cartShots('vehicle-hud', MAP, shots(), { setup: SETUP, out });
  const native = nativeShots('vehicle-hud', MAP, shots(), { setup: SETUP.filter((c) => !c.startsWith('set g_oaxVehicles')), startArgs: ['+set', 'g_oaxVehicles', '1'] });
  for (const [b, r] of [['cart', cart], ['native', native]]) {
    for (const k of ['driver', 'nohud', 'gunner', 'damaged']) if (!r.images[k]) failures.push(`${b}: no ${k} frame`);
  }
  if (failures.length) return { ok: false, failures, rows };
  const c = Object.fromEntries(Object.entries(cart.images).map(([k, im]) => [k, crop(im)]));
  const nv = Object.fromEntries(Object.entries(native.images).map(([k, im]) => [k, crop(im)]));
  for (const [k, im] of Object.entries(c)) writePng(path.join(out, `vehicle-hud-cart-${k}.png`), im);
  for (const [k, im] of Object.entries(nv)) writePng(path.join(out, `vehicle-hud-native-${k}.png`), im);

  // the golden
  const gdir = path.join(goldens, 'oax');
  fs.mkdirSync(gdir, { recursive: true });
  const gfile = path.join(gdir, 'vehicle_hud.png');
  if (update || !fs.existsSync(gfile)) {
    writePng(gfile, c.driver);
    rows.push(`cart golden vehicle_hud ${update ? 'updated' : 'created'}`);
  }
  const golden = readPng(gfile);
  const cmp = (a, b) => comparePng(a, b, { tolerance: TOLERANCE }).badFraction;
  const pct = (x) => `${(x * 100).toFixed(3)}%`;
  const g = { driver: cmp(golden, c.driver), nohud: cmp(golden, c.nohud), damaged: cmp(golden, c.damaged), gunner: cmp(golden, c.gunner) };
  rows.push(`cart golden vehicle_hud (${c.driver.width}x${c.driver.height} panel): driver ${pct(g.driver)} over tolerance; controls: panel off ${pct(g.nohud)}, after the hit ${pct(g.damaged)}, gunner ${pct(g.gunner)}`);
  if (g.driver > LIMIT) failures.push(`cart golden vehicle_hud: ${pct(g.driver)} differ`);
  if (g.nohud <= LIMIT) failures.push('control: the frame without the panel matches the golden');
  if (g.damaged <= LIMIT) failures.push('control: the damaged panel matches the golden');
  // native == cart
  const n = { driver: cmp(c.driver, nv.driver), nohud: cmp(c.driver, nv.nohud) };
  rows.push(`native vs cart panel: ${pct(n.driver)} differ (control: native without the panel ${pct(n.nohud)})`);
  if (n.driver > LIMIT) failures.push(`native panel differs from the cart's (${pct(n.driver)})`);
  if (n.nohud <= LIMIT) failures.push('control: native without the panel matches the cart panel');
  checkValues('cart', cart, rows, failures);
  checkValues('native', native, rows, failures);
  return { ok: failures.length === 0, failures, rows };
}
