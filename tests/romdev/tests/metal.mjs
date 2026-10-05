// oaxMetal (docs/materials.md): a metal reflects the nearest misc_cubemap
// probe, captured when the map loads.
//
// oax_metal: a silver mirror (roughness 0) faces the eye; a red panel is on
// the wall behind the eye. Checks, each with a control:
//   - the map has its probe and the mirror draws its reflection
//     (r_metal_probes, r_metal_draws); control: r_oaxReflect 0 (latched, so
//     set before the map loads) has no probe and no reflection draw;
//   - the mirror shows the red panel; control: none with r_oaxReflect 0;
//   - the cart matches the native build (control: the cart's frame without
//     the reflection).

import { shootCart, shootNative, readPng } from '../lib/ulight.mjs';
import { comparePng, halfSize } from '../lib/png.mjs';
import { fraction } from '../lib/imgstat.mjs';

export const name = 'metal';

const MAP = 'oax_metal';
const VIEW = '0 -500 160 0 90 0';
const PLAYER = '0 -500 24 90';
// the red panel's reflection, in the middle of the mirror
const RED_BOX = { x0: 0.45, x1: 0.55, y0: 0.45, y1: 0.52 };
const red = (r, g, b) => r > g + 25 && r > b + 25;
const num = (v) => Number(v ?? NaN);

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const on = await shootCart(MAP, [VIEW], { name: 'metal-on', out, player: PLAYER });
  const off = await shootCart(MAP, [VIEW], { name: 'metal-off', out, player: PLAYER, preload: 'r_oaxReflect 0' });
  const nat = shootNative(MAP, [VIEW], { name: 'metal-native', out, player: PLAYER });
  const a = readPng(on.files[0]), b = readPng(off.files[0]), n = readPng(nat.files[0]);

  for (const [what, r, want] of [['cart', on, 1], ['cart r_oaxReflect 0', off, 0], ['native', nat, 1]]) {
    const probes = num(r.values.r_metal_probes), draws = num(r.values.r_metal_draws);
    rows.push(`${what}: model ${r.values.r_ulight_model}, probes ${probes}, metal draws ${draws}`);
    if (r.values.r_ulight_model !== '1') failures.push(`${what}: the map did not load with unified lighting`);
    if (want && (probes !== 1 || !(draws >= 1))) failures.push(`${what}: no probe or no reflection draw`);
    if (!want && (probes !== 0 || draws !== 0)) failures.push(`${what}: control: a probe or a reflection draw with r_oaxReflect 0`);
  }

  const redOn = fraction(a, red, RED_BOX), redOff = fraction(b, red, RED_BOX), redNat = fraction(n, red, RED_BOX);
  rows.push(`red panel in the mirror: cart ${(redOn * 100).toFixed(1)}%, native ${(redNat * 100).toFixed(1)}%, r_oaxReflect 0 ${(redOff * 100).toFixed(1)}%`);
  if (redOn < 0.5) failures.push('cart: the mirror does not show the red panel');
  if (redNat < 0.5) failures.push('native: the mirror does not show the red panel');
  if (redOff > 0.02) failures.push('control: red in the mirror without the reflection');

  const d = comparePng(halfSize(n), halfSize(a), { tolerance: 32 });
  const c = comparePng(halfSize(n), halfSize(b), { tolerance: 32 });
  rows.push(`cart vs native: mean ${d.meanDiff.toFixed(2)}, ${(d.badFraction * 100).toFixed(2)}% beyond 32 (control ${(c.badFraction * 100).toFixed(2)}%)`);
  if (d.meanDiff > 4 || d.badFraction > 0.03) failures.push('cart differs from native');
  if (c.badFraction <= d.badFraction) failures.push('control: native matches the frame without the reflection as well');
  return { ok: failures.length === 0, failures, rows };
}
