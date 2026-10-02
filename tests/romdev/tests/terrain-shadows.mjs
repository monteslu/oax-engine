// Cascaded sun shadows on terrain (phase 7), on the cart (ES 3.0 path).
//
// oax_terrain's sky has q3gl2_sun, so renderergl2 renders the sun cascades
// and the screen-space shadow mask; terrain and trees draw into the
// cascades (casters) and multiply by the mask (receivers).
// The whole-level cascade is drawn once and kept, so the views stay within
// the near cascades (r_shadowCascadeZFar 1024), which are redrawn per frame.
// - receiving + terrain casting: r_oaxTerrainDebug 4 paints the terrain with
//   the mask itself (foliage off); the bank and hills must leave shadowed
//   terrain, and r_oaxTerrainDebug 5 (the same with terrain kept out of the
//   cascades) must shadow clearly less;
// - trees casting, in colour: a frame with foliage must have regions darker
//   than r_oaxTerrainDebug 3 (nothing of ours in the cascades);
// - control for the darker-region measure: two renders of the same frame
//   must show (almost) no darker region, or the measure could not fail.

import { cartShots } from '../lib/cartshot.mjs';

export const name = 'terrain-shadows';

const MAP = 'oax_terrain';
const SETUP = ['r_fixedShaderTime 3', 'r_autoExposure 0'];
const NEAR = 'cl_overrideView "-200 -1200 260 30 60 0"';
const BANK = 'cl_overrideView "-1150 -150 380 30 50 0"';

// fraction of pixels whose green channel (the mask is grey) is below 96
function darkFraction(img) {
  let n = 0;
  for (let i = 0; i < img.data.length; i += 4) if (img.data[i + 1] < 96) n++;
  return n / (img.data.length / 4);
}

// pixels shadowed in mask a and lit in mask b
function onlyIn(a, b) {
  let n = 0;
  for (let i = 0; i < a.data.length; i += 4) if (a.data[i + 1] < 96 && b.data[i + 1] >= 160) n++;
  return n / (a.data.length / 4);
}

// fraction of pixels where a is darker than b by more than 24 (max channel)
function darker(a, b) {
  let n = 0;
  for (let i = 0; i < a.data.length; i += 4) {
    if (Math.max(b.data[i] - a.data[i], b.data[i + 1] - a.data[i + 1], b.data[i + 2] - a.data[i + 2]) > 24) n++;
  }
  return n / (a.data.length / 4);
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const r = await cartShots('terrain-shadows', MAP, [
    { cmd: `${BANK};r_oaxFoliage 0;r_oaxTerrainDebug 4`, name: 'mask', values: 'mask' },
    { cmd: 'r_oaxTerrainDebug 5', name: 'masknocast' },
    { cmd: `${NEAR};r_oaxFoliage 1;r_oaxTerrainDebug 0`, name: 'color' },
    { cmd: 'r_oaxTerrainDebug 3', name: 'nocast' },
    { cmd: 'r_oaxTerrainDebug 0', name: 'color2' },
  ], { setup: SETUP, out });
  const v = r.valuesAt.mask;
  const im = r.images;
  rows.push(`cascades: terrain drawn into ${v.r_terrain_shadow_views} shadow views this frame (${v.r_terrain_shadow_chunks} chunk draws)`);
  if (!(Number(v.r_terrain_shadow_views) >= 3)) failures.push('terrain is not drawn into the sun shadow cascades');
  const dm = darkFraction(im.mask), dn = darkFraction(im.masknocast), own = onlyIn(im.mask, im.masknocast);
  rows.push(`shadow mask on terrain (the steep bank): ${(dm * 100).toFixed(1)}% shadowed with terrain casting, ${(dn * 100).toFixed(1)}% without; ${(own * 100).toFixed(2)}% of the frame shadowed only by the terrain itself`);
  if (!(dm > 0.02)) failures.push('terrain receives no sun shadow');
  if (!(own > 0.004)) failures.push('terrain casts no sun shadow onto itself');
  const dc = darker(im.color, im.nocast), ctl = darker(im.color, im.color2);
  rows.push(`colour: ${(dc * 100).toFixed(1)}% of the frame darker with terrain and trees casting; control (same frame twice) ${(ctl * 100).toFixed(3)}%`);
  if (!(dc > 0.02)) failures.push('terrain and trees cast no visible sun shadow');
  if (ctl > 0.001) failures.push('control did not fail: the same frame rendered twice differs');
  return { ok: failures.length === 0, failures, rows };
}
