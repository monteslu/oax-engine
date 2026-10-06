// Unified lights light blended surfaces (docs/map-format.md: lit additive,
// OSF_TRANSLUCENT|OSF_ADDITIVE, UE1 Translucent water). After the opaque
// pass each light adds its light x texel over them, with its shadows
// (tr_ulight.c oaxLitBlend, tb_ulight.c RB_DrawULightsBlend); their own
// stage stays the ambient x texel.
//
// oax_litblend: a water quad over a black floor under a point light, a slab
// shadowing part of it. Looking straight down at the water:
//   - open: in the open under the light, the water must read far brighter
//     with the pass than with it off (r_ulightLitBlend 0: the ambient alone);
//   - shadow: in the slab's shadow the water must be much darker than the
//     same view without shadows (r_ulightShadows 0);
//   - the cart renders the open view like the native build.
// Controls: r_ulightLitBlend 0 is the old picture (the two must differ), and
// the shadow view without shadows must itself be lit by the pass.

import { shootCart, shootNative, readPng } from '../lib/ulight.mjs';
import { comparePng, halfSize } from '../lib/png.mjs';
import { meanRGB } from '../lib/imgstat.mjs';
import { OPEN_CAM, SHADOW_CAM } from '../../maps/src/oax_litblend.mjs';

export const name = 'lit-blend';

const MAP = 'oax_litblend';
const OPEN = `${OPEN_CAM.join(' ')} 90 0 0`;
const SHADOW = `${SHADOW_CAM.join(' ')} 90 0 0`;
const PLAYER = '-400 -400 24 45';
// the middle of the frame: water only (the slab is above the camera)
const MID = { x0: 0.3, x1: 0.7, y0: 0.3, y1: 0.7 };
const luma = (img) => { const [r, g, b] = meanRGB(img, MID); return (r + g + b) / 3; };

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const on = await shootCart(MAP, [OPEN, SHADOW], { name: 'lit-blend-on', out, player: PLAYER });
  const off = await shootCart(MAP, [OPEN, SHADOW], { name: 'lit-blend-off', out, player: PLAYER, pre: 'r_ulightLitBlend 0' });
  const noShadow = await shootCart(MAP, [SHADOW], { name: 'lit-blend-noshadow', out, player: PLAYER, pre: 'r_ulightShadows 0' });
  const nat = shootNative(MAP, [OPEN], { name: 'lit-blend-native', out, player: PLAYER });
  const [open1, shadow1] = on.files.map(readPng), [open0, shadow0] = off.files.map(readPng);
  const [shadowNS] = noShadow.files.map(readPng), [openNat] = nat.files.map(readPng);

  const o1 = luma(open1), o0 = luma(open0), s1 = luma(shadow1), sNS = luma(shadowNS), oN = luma(openNat);
  rows.push(`model ${on.values.r_ulight_model}; open water: ${o1.toFixed(1)} (native ${oN.toFixed(1)}), pass off ${o0.toFixed(1)}`);
  rows.push(`shadowed water: ${s1.toFixed(1)}, no shadows ${sNS.toFixed(1)}, pass off ${luma(shadow0).toFixed(1)}`);
  if (on.values.r_ulight_model !== '1') failures.push(`the map did not load with unified lighting (model ${on.values.r_ulight_model})`);
  if (!(o0 < 40)) failures.push(`control: the water is bright without the pass (${o0.toFixed(1)}): the floor or the ambient shows`);
  if (!(o1 > 60 && o1 > o0 * 3)) failures.push('the light does not light the water (open view)');
  if (!(sNS > 40)) failures.push(`control: the water in the shadow view is not lit without shadows (${sNS.toFixed(1)})`);
  if (!(s1 < sNS * 0.5)) failures.push('the slab casts no shadow on the water');

  const d = comparePng(halfSize(openNat), halfSize(open1), { tolerance: 32 });
  const c = comparePng(halfSize(openNat), halfSize(open0), { tolerance: 32 });
  rows.push(`cart vs native, open view: mean ${d.meanDiff.toFixed(2)}, ${(d.badFraction * 100).toFixed(2)}% beyond 32 (control: pass off ${(c.badFraction * 100).toFixed(2)}%)`);
  if (d.meanDiff > 4 || d.badFraction > 0.03) failures.push('cart differs from native');
  if (c.badFraction <= d.badFraction) failures.push('control: native matches the frame without the pass as well');
  return { ok: failures.length === 0, failures, rows };
}
