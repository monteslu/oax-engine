// Sky faces cast unified-lighting shadows for point lights (docs/materials.md,
// "Sky faces as shadow casters"; tr_ulight.c WalkLightNodes adds them to a
// point or spot light's casters, never to its lit surfaces; tr_shade.c draws
// the face itself, not the sky box, in the shadow depth pass). UE1's
// FakeBackdrop and Q3's sky brushes block their lamps the same way; a
// courtyard lamp must not light the lawn through the courtyard's sky panel.
//
// oax_skyshadow: oax_movershadow with the slab made of sky.
//   - below: the eye just over the floor looking straight down, under the
//     slab, which is out of view: the floor is in the slab's shadow;
//   - side: the eye sees the slab (drawing the sky) and the floor under it;
// control: r_ulightShadows 0 lights the same floor fully; the shadowed
// view must be much darker than the control in both, and the cart must
// match the native build.

import { shootCart, shootNative, readPng } from '../lib/ulight.mjs';
import { comparePng, halfSize } from '../lib/png.mjs';
import { meanRGB } from '../lib/imgstat.mjs';

export const name = 'sky-shadow';

const MAP = 'oax_skyshadow';
const BELOW = '0 0 60 90 0 0';
const SIDE = '0 -480 300 35 90 0';
const PLAYER = '-400 -400 24 45';
// the floor right under the slab in the side view (the shadow's middle)
const SIDE_FLOOR = { x0: 0.42, x1: 0.58, y0: 0.36, y1: 0.52 };
const luma = (img, box) => { const [r, g, b] = meanRGB(img, box); return (r + g + b) / 3; };

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const on = await shootCart(MAP, [BELOW, SIDE], { name: 'sky-shadow-on', out, player: PLAYER });
  const off = await shootCart(MAP, [BELOW, SIDE], { name: 'sky-shadow-off', out, player: PLAYER, pre: 'r_ulightShadows 0' });
  const nat = shootNative(MAP, [BELOW, SIDE], { name: 'sky-shadow-native', out, player: PLAYER });
  const [b1, s1] = on.files.map(readPng), [b0, s0] = off.files.map(readPng), [bn] = nat.files.map(readPng);

  const below = luma(b1), belowOff = luma(b0), belowNat = luma(bn);
  const side = luma(s1, SIDE_FLOOR), sideOff = luma(s0, SIDE_FLOOR);
  rows.push(`model ${on.values.r_ulight_model}; floor under the sky slab, slab out of view: ${below.toFixed(1)} (native ${belowNat.toFixed(1)}), no shadows ${belowOff.toFixed(1)}`);
  rows.push(`floor under the sky slab, slab in view: ${side.toFixed(1)}, no shadows ${sideOff.toFixed(1)}`);
  if (on.values.r_ulight_model !== '1') failures.push(`the map did not load with unified lighting (model ${on.values.r_ulight_model})`);
  if (!(belowOff > 20)) failures.push(`control: the floor is not lit without shadows (${belowOff.toFixed(1)})`);
  if (!(below < belowOff * 0.5)) failures.push('the sky slab out of view casts no shadow on the floor');
  if (!(belowNat < belowOff * 0.5)) failures.push('native: the sky slab out of view casts no shadow on the floor');
  if (!(side < sideOff * 0.5)) failures.push('the sky slab in view casts no shadow on the floor');

  const d = comparePng(halfSize(readPng(nat.files[1])), halfSize(s1), { tolerance: 32 });
  const c = comparePng(halfSize(readPng(nat.files[1])), halfSize(s0), { tolerance: 32 });
  rows.push(`cart vs native, side view: mean ${d.meanDiff.toFixed(2)}, ${(d.badFraction * 100).toFixed(2)}% beyond 32 (control: no shadows ${(c.badFraction * 100).toFixed(2)}%)`);
  if (d.meanDiff > 4 || d.badFraction > 0.03) failures.push('cart differs from native');
  if (c.badFraction <= d.badFraction) failures.push('control: native matches the frame without shadows as well');
  return { ok: failures.length === 0, failures, rows };
}
