// The opt-in UE1 display curve, r_displayCurve (tr_oax_display.c).
//
// oax_display: grey panels at texel 41, 74, 114 and 180 lit at exactly 1
// (r_toneMap 0, so without the curve a panel is its texel). With
// r_displayCurve 1 and the reference renderer's default settings (Brightness
// 1.0, GammaOffset 0.1) each panel must equal the curve's oracle
// (tests/romdev/lib/displaycurve.mjs) within 1 grey level, and the measured
// reference shots (84, 136, 183, 232) within 3. Controls: r_displayCurve 0 must not match the measured
// values (the panels stay at their texels), and the neutral settings
// (Brightness 0.5, GammaOffset 0) must give the texels back. Cart and native.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { readPng, writePng, readTga } from '../lib/ulight.mjs';
import { runNative } from '../lib/native.mjs';
import { displayCurve } from '../lib/displaycurve.mjs';
import { TEXELS, MEASURED, HALF, centre } from '../../maps/src/oax_display.mjs';

export const name = 'display-curve';
export const timeoutSec = 240;

const MAP = 'oax_display';
const W = 1280, H = 720, CAM = 600, TAN_X = 1, TAN_Y = 720 / 1280;
const VIEW_CVARS = 'r_toneMap 0;r_autoExposure 0;r_cameraExposure 1;r_gamma 1';
const VIEW = `0 0 ${CAM} 90 0 0`;
const SETTINGS = [
  { label: 'off', cvars: 'r_displayCurve 0' },
  { label: 'shipped', cvars: 'r_displayCurve 1;r_displayCurveBrightness 1.0;r_displayCurveGammaOffset 0.1', curve: { brightness: 1, gammaOffset: 0.1 } },
  { label: 'neutral', cvars: 'r_displayCurve 1;r_displayCurveBrightness 0.5;r_displayCurveGammaOffset 0', curve: { brightness: 0.5, gammaOffset: 0 } },
];

const pixelOf = (x, y) => [Math.round(((-y / (TAN_X * CAM)) + 1) * W / 2 - 0.5), Math.round((1 - x / (TAN_Y * CAM)) * H / 2 - 0.5)];
function panelMean(img, [cx, cy]) {
  const r = HALF * 0.6, [ax, ay] = pixelOf(cx + r, cy + r), [bx, by] = pixelOf(cx - r, cy - r);
  let s = 0, n = 0;
  for (let py = Math.min(ay, by); py <= Math.max(ay, by); py++) for (let px = Math.min(ax, bx); px <= Math.max(ax, bx); px++) {
    const o = (py * W + px) * 4;
    s += (img.data[o] + img.data[o + 1] + img.data[o + 2]) / 3;
    n++;
  }
  return s / n;
}

async function cart(out) {
  const s = new Session('display-curve-cart');
  const files = [];
  try {
    await loadScene(s, MAP, { seed: 1, view: CLEAN_VIEW, picmip: null });
    for (const st of SETTINGS) {
      await s.command(`${VIEW_CVARS};${st.cvars};cl_overrideView "${VIEW}"`);
      await s.step(20);
      const f = path.join(out, `display-curve-cart-${st.label}.png`);
      await s.screenshot(f);
      files.push(f);
    }
    return files;
  } finally {
    await s.shutdown();
  }
}

function native(out) {
  const lines = [...CLEAN_VIEW.split(';'), ...VIEW_CVARS.split(';'), `cl_overrideView "${VIEW}"`, 'wait 80'];
  SETTINGS.forEach((st, k) => lines.push(...st.cvars.split(';'), 'wait 30', `screenshot shot_${k}`, 'wait 2'));
  const home = runNative('display-curve-native', MAP, lines, { picmip: null });
  return SETTINGS.map((st, k) => {
    const tga = path.join(home, 'baseoa', 'screenshots', `shot_${k}.tga`);
    if (!fs.existsSync(tga)) throw new Error(`native client wrote no screenshot ${k} (see ${home}/native.log)`);
    const f = path.join(out, `display-curve-native-${st.label}.png`);
    writePng(f, readTga(tga));
    return f;
  });
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  for (const [host, files] of [['cart', await cart(out)], ['native', native(out)]]) {
    SETTINGS.forEach((st, k) => {
      const img = readPng(files[k]);
      const got = TEXELS.map((_, i) => panelMean(img, centre(i)));
      const want = TEXELS.map((t) => (st.curve ? 255 * displayCurve([t / 255, t / 255, t / 255], st.curve)[0] : t));
      const err = Math.max(...got.map((v, i) => Math.abs(v - want[i])));
      const vsRef = Math.max(...got.map((v, i) => Math.abs(v - MEASURED[i])));
      rows.push(`${host} ${st.label}: ${got.map((v) => v.toFixed(1)).join(' ')} (oracle ${want.map((v) => v.toFixed(1)).join(' ')}, reference measured ${MEASURED.join(' ')}; max |oracle| ${err.toFixed(2)}, max |reference| ${vsRef.toFixed(2)})`);
      if (err > 1) failures.push(`${host} ${st.label}: panels off the oracle by ${err.toFixed(2)}`);
      if (st.label === 'shipped' && vsRef > 3) failures.push(`${host} shipped: panels off the measured reference shots by ${vsRef.toFixed(2)}`);
      if (st.label !== 'shipped' && vsRef <= 3) failures.push(`${host} ${st.label}: control matched the measured reference curve`);
    });
  }
  return { ok: failures.length === 0, failures, rows };
}
