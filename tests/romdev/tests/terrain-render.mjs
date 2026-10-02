// Heightmap terrain rendering (phase 7): oax_terrain on the cart, with the
// native client as a cross-check.
//
// Checks, each next to a control that must fail:
// - three cart goldens (overview, top-down, close to the hill), each a real
//   picture (thousands of colours); control: the overview with the terrain
//   off (r_oaxTerrain 0, collision unaffected) against its golden;
// - no LOD cracks: r_oaxTerrainDebug 1 draws every terrain fragment flat
//   green; a steep top-down camera with a short LOD distance sees several
//   levels and stitched chunk edges, and every pixel must be green (a crack
//   shows the floor far below); control: r_oaxTerrainDebug 2 draws the same
//   levels without stitching and must show cracks;
// - the static data: every chunk loaded, stitch patterns verified at load;
// - the native client renders the close view like the cart (mean
//   difference over the hill, away from the box walls' shadow edge, which
//   the two GLs rasterize differently); control: against the cart's
//   terrain-off frame.

import fs from 'node:fs';
import path from 'node:path';
import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { comparePng, readPng, halfSize, writePng } from '../lib/png.mjs';
import { meanDiff } from '../lib/imgstat.mjs';

export const name = 'terrain-render';

const MAP = 'oax_terrain';
const SETUP = ['r_fixedShaderTime 3', 'r_autoExposure 0'];
const VIEWS = {
  overview: '-1400 -1400 350 12 45 0',
  top: '0 0 1100 60 90 0',
  near: '200 -1000 300 25 40 0',
};
const CRACK_VIEW = '0 -300 650 80 90 0';
const TOLERANCE = 24;
const HILL = { x0: 0, y0: 0.4, x1: 0.75, y1: 1 };
const MIN_COLORS = 3000;

export function uniqueColors(img) {
  const s = new Set();
  for (let i = 0; i < img.data.length; i += 4) s.add((img.data[i] << 16) | (img.data[i + 1] << 8) | img.data[i + 2]);
  return s.size;
}

// pixels that are not the flat terrain colour (the most common colour)
export function offColor(img, tol = 40) {
  const counts = new Map();
  for (let i = 0; i < img.data.length; i += 4) {
    const k = (img.data[i] << 16) | (img.data[i + 1] << 8) | img.data[i + 2];
    counts.set(k, (counts.get(k) || 0) + 1);
  }
  const top = [...counts.entries()].sort((a, b) => b[1] - a[1])[0][0];
  const c = [top >> 16, (top >> 8) & 255, top & 255];
  let off = 0;
  for (let i = 0; i < img.data.length; i += 4) {
    if (Math.max(Math.abs(img.data[i] - c[0]), Math.abs(img.data[i + 1] - c[1]), Math.abs(img.data[i + 2] - c[2])) > tol) off++;
  }
  return { color: c, off, total: img.data.length / 4 };
}

function shotList() {
  return [
    ...Object.entries(VIEWS).map(([n, v]) => ({ cmd: `cl_overrideView "${v}"`, name: n, values: n })),
    { cmd: `cl_overrideView "${VIEWS.overview}";r_oaxTerrain 0`, name: 'off', values: 'off' },
    { cmd: `r_oaxTerrain 1;cl_overrideView "${CRACK_VIEW}";r_oaxTerrainLodDist 150;r_oaxTerrainDebug 1`, name: 'crack', values: 'crack' },
    { cmd: 'r_oaxTerrainDebug 2', name: 'crackctl', values: 'crackctl' },
    { cmd: 'r_oaxTerrainDebug 0;r_oaxTerrainLodDist 1200', name: null },
  ];
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const gdir = path.join(goldens, 'terrain');
  fs.mkdirSync(gdir, { recursive: true });

  const cart = await cartShots('terrain-render', MAP, shotList(), { setup: SETUP, out });
  const v = cart.valuesAt;
  rows.push(`static: ${v.overview.r_terrain_chunks} chunks, stitch patterns ${v.overview.r_terrain_stitch_ok === '1' ? 'verified' : 'BROKEN'}, collision terrains ${v.overview.cm_terrains}`);
  if (v.overview.r_terrain_stitch_ok !== '1') failures.push('stitch patterns failed their load-time check');
  if (!(Number(v.overview.r_terrain_chunks) > 0)) failures.push('no terrain chunks loaded');

  for (const n of Object.keys(VIEWS)) {
    const img = cart.images[n];
    const colors = uniqueColors(img);
    rows.push(`${n}: ${colors} colours, ${v[n].r_terrain_chunks_drawn}/${v[n].r_terrain_chunks_visible} chunks drawn, ${v[n].r_terrain_tris} triangles, ${v[n].r_terrain_lods} LOD levels`);
    if (colors < MIN_COLORS) failures.push(`${n}: only ${colors} colours`);
    const golden = path.join(gdir, `terrain_${n}.png`);
    const half = halfSize(img);
    if (update || !fs.existsSync(golden)) {
      writePng(golden, half);
      rows.push(`golden ${n} ${update ? 'updated' : 'created'}`);
      continue;
    }
    const r = comparePng(readPng(golden), half, { tolerance: TOLERANCE, diffPath: path.join(out, `terrain-render_${n}.diff.png`) });
    rows.push(`golden ${n}: ${(r.badFraction * 100).toFixed(3)}% over tolerance`);
    if (!r.sameSize || r.badFraction > 0.005) failures.push(`golden ${n}: ${(r.badFraction * 100).toFixed(2)}% differ`);
  }
  const ctl = comparePng(readPng(path.join(gdir, 'terrain_overview.png')), halfSize(cart.images.off), { tolerance: TOLERANCE });
  rows.push(`control: terrain off against the overview golden ${(ctl.badFraction * 100).toFixed(1)}% differ (${v.off.r_terrain_chunks_drawn || 0} chunks drawn)`);
  if (ctl.badFraction <= 0.05) failures.push('control did not fail: the overview golden matches a frame without terrain');

  // cracks
  const crack = offColor(cart.images.crack), crackCtl = offColor(cart.images.crackctl);
  rows.push(`cracks: ${crack.off} non-terrain pixels of ${crack.total} (flat colour ${crack.color.join(',')}), ${v.crack.r_terrain_lods} LOD levels, ${v.crack.r_terrain_stitched} stitched chunks drawn; control without stitching: ${crackCtl.off} crack pixels`);
  if (!(Number(v.crack.r_terrain_lods) >= 2 && Number(v.crack.r_terrain_stitched) > 0)) failures.push('the crack view does not mix levels of detail (nothing to test)');
  if (crack.off > 0) failures.push(`${crack.off} crack pixels with stitching`);
  if (crackCtl.off === 0) failures.push('control did not fail: no cracks without stitching');

  // native cross-check
  const nat = nativeShots('terrain-render', MAP, [{ cmd: `cl_overrideView "${VIEWS.near}"`, name: 'near' }], { setup: SETUP });
  if (!nat.images.near) failures.push('native wrote no screenshot');
  else {
    const m = meanDiff(nat.images.near, cart.images.near, HILL), mctl = meanDiff(nat.images.near, cart.images.off, HILL);
    rows.push(`native vs cart near view: mean difference ${m.toFixed(2)} (control against the cart's terrain-off frame ${mctl.toFixed(1)})`);
    if (m > 6) failures.push(`native near view differs from the cart's (mean ${m.toFixed(2)})`);
    if (mctl <= 6) failures.push('native control did not fail');
  }
  return { ok: failures.length === 0, failures, rows };
}
