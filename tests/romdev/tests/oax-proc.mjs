// Procedural textures and image programs (design 2.6) on both builds.
//
// oax_proc has six fullbright panels: fire, water, wet, ice and plasma
// procedurals, and a DOOM-3 heightmap() image program. Shader time is
// pinned with r_fixedShaderTime at 1, 3 and 5 s (each more than the
// simulation window apart, so every frame is the canonical state for its
// time).
//
// Checks, each next to a control that must fail:
// - image programs are byte-exact against a JS port (lib/imageprogram.mjs)
//   on a 16x16 image: heightmap() plus four other programs, on both
//   builds; control: heightmap() with another scale has another hash, in
//   the engine and in JS;
// - a frozen time renders the same twice; different times differ for
//   every procedural panel; control: the static heightmap panel does not
//   change with time (and would fail the "differ" check);
// - cart goldens per panel and time; native frames match the cart's
//   (procedurals render the same on both hosts); control: a panel against
//   another panel's golden fails.

import fs from 'node:fs';
import path from 'node:path';
import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { readPng, writePng } from '../lib/png.mjs';
import { diffFraction, meanDiff, shrink } from '../lib/imgstat.mjs';
import { programResult } from '../lib/imageprogram.mjs';
import { PANELS, panelCenter, heightImage, HEIGHT_IMAGE, HEIGHT_PROGRAM } from '../../maps/src/oax_proc.mjs';

export const name = 'oax-proc';

const MAP = 'oax_proc';
const SETUP = ['r_fixedShaderTime 1', 'r_autoExposure 0'];
const TIMES = [1, 3, 5];
const PANEL_BOX = { x0: 0.3, y0: 0.25, x1: 0.7, y1: 0.75 };   // inside the panel in every panel view
const PROGRAMS = [
  HEIGHT_PROGRAM,
  `heightmap(${HEIGHT_IMAGE}, 7)`,
  `addnormals(heightmap(${HEIGHT_IMAGE}, 4), smoothnormals(heightmap(${HEIGHT_IMAGE}, 9)))`,
  `scale(add(${HEIGHT_IMAGE}, invertAlpha(${HEIGHT_IMAGE})), 0.5, 1.7, 2, 1.1)`,
  `makeIntensity(makeAlpha(invertColor(${HEIGHT_IMAGE})))`,
];
const GOLDEN_TOL = 24;
const NATIVE_MEAN = 6;

const view = (i) => {
  const [x] = panelCenter(i);
  return `cl_overrideView "${x} 100 128 0 90 0"`;
};

function shotList() {
  const list = [];
  PROGRAMS.forEach((p, k) => list.push({ cmd: `imageprogram ${p};debugcvar r_imageprogram_result`, settle: 2, values: `prog${k}` }));
  for (const t of TIMES) {
    PANELS.forEach((p, i) => list.push({ cmd: `r_fixedShaderTime ${t};${view(i)}`, name: `${p}_${t}` }));
  }
  // frozen time: the same time again after other frames
  list.push({ cmd: `r_fixedShaderTime 3;${view(0)}`, name: 'fire_3_again', settle: 30 });
  list.push({ cmd: view(1), name: 'water_3_again', settle: 30 });
  return list;
}

function checkPrograms(build, r, failures, rows) {
  const images = { [HEIGHT_IMAGE]: heightImage() };
  PROGRAMS.forEach((p, k) => {
    const got = r.valuesAt[`prog${k}`]?.r_imageprogram_result;
    const want = programResult(p, images);
    rows.push(`${build}: ${p} -> ${got} (JS ${want})`);
    if (got !== want) failures.push(`${build}: image program ${p} gives ${got}, JS gives ${want}`);
  });
  const h4 = r.valuesAt.prog0?.r_imageprogram_result, h7 = r.valuesAt.prog1?.r_imageprogram_result;
  if (!h4 || h4 === h7) failures.push(`${build}: control did not fail: heightmap scale 4 and 7 give the same bytes`);
}

function checkTimes(build, im, failures, rows) {
  for (const p of PANELS) {
    const d13 = meanDiff(im[`${p}_1`], im[`${p}_3`], PANEL_BOX);
    const d35 = meanDiff(im[`${p}_3`], im[`${p}_5`], PANEL_BOX);
    const isStatic = p === 'heightmap';
    rows.push(`${build}: ${p} mean change 1->3 s ${d13.toFixed(1)}, 3->5 s ${d35.toFixed(1)}${isStatic ? ' (static control)' : ''}`);
    if (isStatic) {
      if (d13 > 1 || d35 > 1) failures.push(`${build}: the static heightmap panel changed with time`);
    } else if (d13 < 4 || d35 < 4) {
      failures.push(`${build}: ${p} did not change between shader times (${d13.toFixed(1)}, ${d35.toFixed(1)})`);
    }
  }
  for (const [a, b] of [['fire_3', 'fire_3_again'], ['water_3', 'water_3_again']]) {
    const f = diffFraction(im[a], im[b], 2, PANEL_BOX);
    rows.push(`${build}: frozen time: ${a} again differs in ${(f * 100).toFixed(3)}% of the panel`);
    if (f > 0.001) failures.push(`${build}: ${a} rendered twice at the same shader time differs (${(f * 100).toFixed(2)}%)`);
  }
}

// the native half alone (also usable without a romdev server)
export function nativeChecks(failures, rows) {
  const native = nativeShots('oax-proc', MAP, shotList(), { setup: SETUP });
  checkPrograms('native', native, failures, rows);
  checkTimes('native', native.images, failures, rows);
  return native;
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const gdir = path.join(goldens, 'oax');
  fs.mkdirSync(gdir, { recursive: true });

  const cart = await cartShots('oax-proc', MAP, shotList(), { setup: SETUP, out });
  checkPrograms('cart', cart, failures, rows);
  checkTimes('cart', cart.images, failures, rows);

  // cart goldens, quarter size (procedurals are compared per panel)
  let worst = 0;
  for (const t of TIMES) {
    for (const p of PANELS) {
      const golden = path.join(gdir, `proc_${p}_${t}.png`);
      const small = shrink(cart.images[`${p}_${t}`], 4);
      if (update || !fs.existsSync(golden)) {
        writePng(golden, small);
        continue;
      }
      const bad = diffFraction(readPng(golden), small, GOLDEN_TOL);
      worst = Math.max(worst, bad);
      if (bad > 0.005) failures.push(`cart golden ${p} at ${t} s: ${(bad * 100).toFixed(2)}% differ`);
    }
  }
  if (update || !fs.existsSync(path.join(gdir, 'proc_fire_1.png'))) rows.push(`cart goldens written (${PANELS.length * TIMES.length})`);
  else rows.push(`cart goldens: worst ${(worst * 100).toFixed(3)}% over tolerance`);
  const ctl = diffFraction(readPng(path.join(gdir, 'proc_water_3.png')), shrink(cart.images.fire_3, 4), GOLDEN_TOL);
  rows.push(`cart control: fire against the water golden ${(ctl * 100).toFixed(1)}% differ`);
  if (ctl <= 0.005) failures.push('cart control did not fail: fire matches the water golden');

  const native = nativeChecks(failures, rows);

  // native against cart, per panel and time
  let worstMean = 0, worstName = '';
  for (const t of TIMES) {
    for (const p of PANELS) {
      const m = meanDiff(native.images[`${p}_${t}`], cart.images[`${p}_${t}`], PANEL_BOX);
      if (m > worstMean) { worstMean = m; worstName = `${p} ${t} s`; }
      if (m > NATIVE_MEAN) failures.push(`native ${p} at ${t} s differs from the cart (mean ${m.toFixed(1)})`);
    }
  }
  const nctl = meanDiff(native.images.fire_3, cart.images.plasma_3, PANEL_BOX);
  rows.push(`native vs cart: worst mean difference ${worstMean.toFixed(2)} (${worstName}); control fire vs plasma ${nctl.toFixed(1)}`);
  if (nctl <= NATIVE_MEAN) failures.push('native control did not fail: fire matches the cart plasma');
  return { ok: failures.length === 0, failures, rows };
}
