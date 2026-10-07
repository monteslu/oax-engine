// The map sidecar (oax_overlay.h, docs/map-format.md "Map overlay"):
// maps/<name>.oaxmap is merged onto the map's entity string at load, so a stock
// BSP gets lights and worldspawn keys without a recompile; com_oaxEnhanced 0
// loads the map as it came.
//
// oax_overlay is a plain lightmapped room with a sidecar that sets hybrid
// lighting and adds one bright realtime light.
//   - enhanced (default): the map loads as hybrid with that light, and the
//     frame is brighter where the light falls than with the sidecar off;
//   - com_oaxEnhanced 0 (set before the map loads): the map loads as a stock
//     map (no unified lighting) and its frame is the plain lightmap one;
//   - the cart and the native client render the enhanced frame alike.

import { shootCart, shootNative, readPng } from '../lib/ulight.mjs';
import { comparePng, halfSize } from '../lib/png.mjs';

export const name = 'map-overlay';

const MAP = 'oax_overlay';
const VIEW = '60 60 200 40 45 0';
const PLAYER = '-384 -384 32 45';

function mean(img) {
  let s = 0;
  for (let i = 0; i < img.data.length; i += 4) s += Math.max(img.data[i], img.data[i + 1], img.data[i + 2]);
  return s / (img.data.length / 4);
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const on = await shootCart(MAP, [VIEW], { name: 'map-overlay-on', out, player: PLAYER });
  const off = await shootCart(MAP, [VIEW], { name: 'map-overlay-off', out, player: PLAYER, preload: 'set com_oaxEnhanced 0' });
  const nat = shootNative(MAP, [VIEW], { name: 'map-overlay-native', out, player: PLAYER });
  const a = readPng(on.files[0]), b = readPng(off.files[0]);
  const d = comparePng(a, b, { tolerance: 16 });
  rows.push(`enhanced: model ${on.values.r_ulight_model}, ${on.values.r_ulights_visible ?? '?'} visible lights, mean brightness ${mean(a).toFixed(1)}; com_oaxEnhanced 0: model ${off.values.r_ulight_model}, mean ${mean(b).toFixed(1)}; ${(d.badFraction * 100).toFixed(1)}% of pixels differ by > 16`);
  if (on.values.r_ulight_model !== '2') failures.push(`the sidecar did not turn the map hybrid (model ${on.values.r_ulight_model})`);
  if (off.values.r_ulight_model === '2' || off.values.r_ulight_model === '1') failures.push(`com_oaxEnhanced 0 still loaded the sidecar (model ${off.values.r_ulight_model})`);
  if (mean(b) < 8) failures.push('the stock lightmapped base is black');
  if (d.badFraction < 0.02) failures.push('the sidecar light added nothing visible');
  if (mean(a) <= mean(b)) failures.push('the sidecar light did not brighten the frame');
  const n = comparePng(halfSize(readPng(nat.files[0])), halfSize(a), { tolerance: 32 });
  rows.push(`cart vs native: mean ${n.meanDiff.toFixed(2)}, ${(n.badFraction * 100).toFixed(2)}% beyond 32`);
  if (n.meanDiff > 4 || n.badFraction > 0.03) failures.push('cart differs from native');
  return { ok: failures.length === 0, failures, rows };
}
