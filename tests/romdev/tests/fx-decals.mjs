// Projected decals (phase 6): a box projected onto the world and brush
// models, kept by the renderer with a cap and a fade, replacing Q3 mark
// polygons for the cgame's lasting impact marks.
//
// oax_fx's decal zone; decals in magenta (the test shader is additive and
// the oaxdecal command takes a colour), so their pixels are easy to find.
// Checks, each with a control:
// - a decal on the wall, one wrapping a block's face and the floor (both
//   surfaces: more polygons than the single wall decal), one on a bobbing
//   brush model; r_decals_live counts them;
// - the brush-model decal moves with the model while the wall decal stays
//   (control: the wall decal's position does not change);
// - a decal fades out over the end of its life and is gone after it
//   (r_decals_live drops back);
// - r_oaxDecals 0 draws none (control for the colour detection);
// - weapon fire makes decals through the cgame (cg_decals_made, the wall's
//   pixels change); control: cg_oaxDecals 0 makes stock marks instead (no
//   new decals);
// - the cap: 300 decals leave exactly 256 live;
// - native draws the same decals as the cart (control: the clean frame).

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { centroid, fraction, diffFraction } from '../lib/imgstat.mjs';
import { MAP, SETUP, nativeMatchesCart, isPicture, num } from '../lib/fxtest.mjs';

export const name = 'fx-decals';

const CAM = 'cl_overrideView "420 -330 150 12 25 0"';
const SPOT = 'textures/oax_fx/spot';
const MAGENTA = ' 0 60000 1 0 1';
const magenta = (r, g, b) => r > g + 70 && b > g + 70;
const LEFT = { x0: 0, x1: 0.38, y0: 0, y1: 1 };       // the bobbing model
const WALL = { x0: 0.38, x1: 0.5, y0: 0.2, y1: 0.45 }; // the wall decal
const CORNER = { x0: 0.46, x1: 0.68, y0: 0.45, y1: 0.7 };
const FADE = { x0: 0.5, x1: 0.7, y0: 0.1, y1: 0.36 };   // a decal on the wall above the block
const inBox = (b) => (x, y) => x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1;

function centroidIn(img, box) {
  let sx = 0, sy = 0, n = 0;
  const inside = inBox(box);
  for (let y = 0; y < img.height; y++) {
    for (let x = 0; x < img.width; x++) {
      const i = (y * img.width + x) * 4;
      if (magenta(img.data[i], img.data[i + 1], img.data[i + 2]) && inside(x / img.width, y / img.height)) { sx += x; sy += y; n++; }
    }
  }
  return n ? { x: sx / n, y: sy / n, n } : { x: NaN, y: NaN, n: 0 };
}

// k: frames per settle frame (a native `wait N` lasts about N/2 frames)
function shotList(k = 1) {
  return [
    { cmd: CAM, name: 'clean', values: 'clean' },
    { cmd: `oaxdecal ${SPOT} 1023 40 120 -1 0 0 40${MAGENTA}`, settle: 2, values: 'wall' },
    { cmd: `oaxdecal ${SPOT} 876 -170 4 -1 0 1 40${MAGENTA};oaxdecal ${SPOT} 638 0 160 -1 0 0 40${MAGENTA}`, settle: 2, name: 'd1', values: 'd1' },
    { settle: 40, name: 'd2' },
    { cmd: `oaxdecal ${SPOT} 1023 -170 190 -1 0 0 30 0 1500 1 0 1`, settle: 2, name: 'f0', values: 'f0' },
    { settle: 63 * k, name: 'f1' },
    { settle: 40 * k, name: 'f2', values: 'f2' },
    { cmd: 'r_oaxDecals 0', name: 'off', values: 'off' },
    // weapon fire at the wall, through the cgame's impact marks
    { cmd: 'r_oaxDecals 1;setviewpos 700 60 40 0;give all;weapon 2', settle: 40, values: 'preweapon' },
    { cmd: '+attack', settle: 15 * k },
    { cmd: '-attack', settle: 20 * k, name: 'mg', values: 'mg' },
    { cmd: 'cg_oaxDecals 0', settle: 5 },
    { cmd: '+attack', settle: 15 * k },
    { cmd: '-attack', settle: 20 * k, values: 'mgstock' },
    { cmd: 'cg_oaxDecals 1;exec fx_cap.cfg', settle: 2, values: 'cap' },
  ];
}

function checkBuild(build, r, ctx) {
  const { rows, failures } = ctx;
  const im = r.images, v = r.valuesAt;
  isPicture(ctx, `${build} d1`, im.d1);

  rows.push(`${build}: live decals clean ${v.clean.r_decals_live}, wall ${v.wall.r_decals_live} (${v.wall.r_decal_polys} polygons), +corner +model ${v.d1.r_decals_live} (${v.d1.r_decal_polys} polygons)`);
  if (num(v.wall.r_decals_live) !== 1 || num(v.d1.r_decals_live) !== 3) failures.push(`${build}: the decals did not all project`);
  // the corner decal covers the block face and the floor: more polygons than one flat decal adds
  const cornerPolys = num(v.d1.r_decal_polys) - num(v.wall.r_decal_polys);
  const wallPolys = num(v.wall.r_decal_polys);
  if (!(cornerPolys > wallPolys + 1)) failures.push(`${build}: the corner and model decals add too few polygons (${cornerPolys})`);
  const corner = fraction(im.d1, magenta, CORNER), cornerClean = fraction(im.clean, magenta, CORNER);
  rows.push(`${build}: corner decal ${(corner * 100).toFixed(2)}% of its box magenta (clean ${(cornerClean * 100).toFixed(2)}%)`);
  if (corner < 0.02 || cornerClean > 0) failures.push(`${build}: the corner decal does not show`);

  const m1 = centroidIn(im.d1, LEFT), m2 = centroidIn(im.d2, LEFT);
  const w1 = centroidIn(im.d1, WALL), w2 = centroidIn(im.d2, WALL);
  const dm = Math.hypot(m1.x - m2.x, m1.y - m2.y), dw = Math.hypot(w1.x - w2.x, w1.y - w2.y);
  rows.push(`${build}: model decal moved ${dm.toFixed(1)} px with the model (${m1.n} -> ${m2.n} px); wall decal moved ${dw.toFixed(2)} px`);
  if (!(m1.n > 200 && m2.n > 200 && dm > 15)) failures.push(`${build}: the brush-model decal does not ride the model`);
  if (!(w1.n > 100 && dw < 0.5)) failures.push(`${build}: control: the wall decal moved or is missing`);

  const f0 = fraction(im.f0, magenta, FADE), f1 = fraction(im.f1, magenta, FADE), f2 = fraction(im.f2, magenta, FADE);
  rows.push(`${build}: fading decal ${(f0 * 100).toFixed(2)}% -> ${(f1 * 100).toFixed(2)}% -> ${(f2 * 100).toFixed(2)}% magenta; live ${v.f0.r_decals_live} -> ${v.f2.r_decals_live}`);
  if (!(f0 > 0.02 && f1 < f0 * 0.8 && f1 > 0 && f2 === 0)) failures.push(`${build}: the decal does not fade out over the end of its life`);
  if (num(v.f2.r_decals_live) !== num(v.f0.r_decals_live) - 1) failures.push(`${build}: the expired decal is still counted`);

  const off = fraction(im.off, magenta);
  rows.push(`${build}: r_oaxDecals 0: ${(off * 100).toFixed(3)}% magenta, ${v.off.r_decals_drawn} drawn`);
  if (off > 0 || num(v.off.r_decals_drawn) !== 0) failures.push(`${build}: r_oaxDecals 0 still draws decals`);

  const made = num(v.mg.cg_decals_made) - num(v.preweapon.cg_decals_made);
  const stock = num(v.mgstock.cg_decals_made) - num(v.mg.cg_decals_made);
  rows.push(`${build}: machine gun: ${made} decals (live ${v.preweapon.r_decals_live} -> ${v.mg.r_decals_live}); with cg_oaxDecals 0: ${stock} (live ${v.mgstock.r_decals_live})`);
  if (made < 1 || num(v.mg.r_decals_live) <= num(v.preweapon.r_decals_live)) failures.push(`${build}: weapon impacts made no decals`);
  if (stock !== 0 || num(v.mgstock.r_decals_live) !== num(v.mg.r_decals_live)) failures.push(`${build}: control: cg_oaxDecals 0 still makes decals`);

  rows.push(`${build}: 300 decals -> ${v.cap.r_decals_live} live`);
  if (num(v.cap.r_decals_live) !== 256) failures.push(`${build}: the cap does not hold 256 decals`);
}

export async function run({ goldens, out, update }) {
  const ctx = { goldens, out, update, rows: [], failures: [] };
  const cart = await cartShots('fx-decals', MAP, shotList(), { setup: SETUP, out });
  checkBuild('cart', cart, ctx);
  const native = nativeShots('fx-decals', MAP, shotList(2), { setup: SETUP });
  checkBuild('native', native, ctx);
  // the wall decal: static, same on both builds
  nativeMatchesCart(ctx, 'wall and corner decals', native.images.d1, cart.images.d1, cart.images.clean, { box: { x0: 0.38, x1: 0.7, y0: 0.2, y1: 0.7 }, limit: 0.01 });
  return { ok: ctx.failures.length === 0, failures: ctx.failures, rows: ctx.rows };
}
