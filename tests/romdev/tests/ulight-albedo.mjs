// Rendered albedo at ambient 1 (step 7.5 B): with no lights and
// oax_ambient 1 1 1 every surface must render at exactly texel x tint.
// oax_albedo (and oax_albedo_ob2, oax_overbright 2) seen from straight
// above; each panel's centre region is compared with the expected color:
//   flat   (100, 150, 200)                 brush face
//   noise  the noise texture's mean        brush face (mean over the panel)
//   tint   flat x oaxTint 0.8 0.6 1.2      brush face, material tint
//   surf   flat x 0.5 1 1.5                OAX_SURFACES quad, surface tint
//   zone   flat x 0.5                      func_oax_zone ambient 0.5
//   surfzone flat x 0.5                    OAX_SURFACES quad in a zone, ambient 0.5
// The contract (docs/lights.md): with r_toneMap 0 and the other view cvars
// at their defaults (final exposure 2^1, the overbright compensation), and
// with picmip 0, rendered / expected is 1 within 1 grey level, for
// oax_overbright 1 and 2. Reported, not checked: the default tone map, and
// picmip 1. Controls: the flat panel against the tinted expectation, and
// the frame without the final exposure (r_cameraExposure 0: 0.5x), must
// fail. Cart and native.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { readPng, writePng, readTga } from '../lib/ulight.mjs';
import { runNative } from '../lib/native.mjs';
import { FLAT, NOISE, NOISE_SIZE, TINT, SURF_TINT, ZONE_AMBIENT, PANELS, HALF } from '../../maps/src/oax_albedo.mjs';

export const name = 'ulight-albedo';
export const timeoutSec = 300;

const W = 1280, H = 720, CAM = 700, TAN_X = 1, TAN_Y = 720 / 1280;
const LINEAR = 'r_toneMap 0;r_autoExposure 0;r_cameraExposure 1;r_gamma 1';
const DEFAULTS = 'r_toneMap 1;r_autoExposure 1;r_cameraExposure 1;r_gamma 1';
const TONEMAP_OFF_ONLY = 'r_toneMap 0;r_gamma 1';
// control: without the final exposure the frame shows light 1 at identityLight (0.5)
const EXPOSURE_0 = 'r_toneMap 0;r_autoExposure 0;r_cameraExposure 0;r_gamma 1';
const VIEW = `0 0 ${CAM} 90 0 0`;

const noiseMean = (() => {
  const s = [0, 0, 0];
  for (let y = 0; y < NOISE_SIZE; y++) for (let x = 0; x < NOISE_SIZE; x++) NOISE(x, y).forEach((v, c) => { s[c] += v; });
  return s.map((v) => v / (NOISE_SIZE * NOISE_SIZE));
})();
export const EXPECT = {
  flat: FLAT,
  noise: noiseMean,
  tint: FLAT.map((v, c) => v * TINT[c]),
  surf: FLAT.map((v, c) => v * SURF_TINT[c]),
  zone: FLAT.map((v) => v * ZONE_AMBIENT),
  surfzone: FLAT.map((v) => v * ZONE_AMBIENT),
};
const clamp = (v) => Math.min(255, v);

// pixel of a floor point (camera straight down: image up is +x, right is -y)
function pixelOf(x, y) {
  return [Math.round(((-y / (TAN_X * CAM)) + 1) * W / 2 - 0.5), Math.round((1 - x / (TAN_Y * CAM)) * H / 2 - 0.5)];
}

// mean color over the panel's centre (inner 60%)
function panelMean(img, name) {
  const [cx, cy] = PANELS[name];
  const s = [0, 0, 0];
  let n = 0;
  const r = HALF * 0.6;
  const [p0x, p0y] = pixelOf(cx + r, cy + r), [p1x, p1y] = pixelOf(cx - r, cy - r);
  for (let py = Math.min(p0y, p1y); py <= Math.max(p0y, p1y); py++) for (let px = Math.min(p0x, p1x); px <= Math.max(p0x, p1x); px++) {
    const o = (py * W + px) * 4;
    for (let c = 0; c < 3; c++) s[c] += img.data[o + c];
    n++;
  }
  return s.map((v) => v / n);
}

// [map, view cvars, boot picmip]: the engine default (picmip 0) is checked;
// the default view cvars and the legacy picmip 1 are reported
const SHOTS = [['oax_albedo', LINEAR, null], ['oax_albedo_ob2', LINEAR, null], ['oax_albedo', DEFAULTS, null], ['oax_albedo', LINEAR, 1], ['oax_albedo_ob2', TONEMAP_OFF_ONLY, null], ['oax_albedo', EXPOSURE_0, null]];

async function cartShot(map, cvars, out, label, picmip) {
  const s = new Session(`ulight-albedo-${label}`);
  try {
    await loadScene(s, map, { seed: 1, view: CLEAN_VIEW, picmip });
    await s.command(`${cvars};cl_overrideView "${VIEW}"`);
    await s.step(40);
    const file = path.join(out, `ulight-albedo-${label}.png`);
    await s.screenshot(file);
    return file;
  } finally {
    await s.shutdown();
  }
}

function nativeShot(map, cvars, out, label, picmip) {
  const home = runNative(`ulight-albedo-${label}`, map, [...CLEAN_VIEW.split(';'), ...cvars.split(';'), `cl_overrideView "${VIEW}"`, 'wait 80', 'screenshot shot_0', 'wait 2'], { picmip });
  const tga = path.join(home, 'baseoa', 'screenshots', 'shot_0.tga');
  if (!fs.existsSync(tga)) throw new Error(`native client wrote no screenshot (see ${home}/native.log)`);
  const file = path.join(out, `ulight-albedo-${label}.png`);
  writePng(file, readTga(tga));
  return file;
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  for (const [i, [map, cvars, picmip]] of SHOTS.entries()) {
    const checked = (cvars === LINEAR || cvars === TONEMAP_OFF_ONLY) && picmip === null;
    for (const host of ['cart', 'native']) {
      const label = `${host}-${map}-${cvars === LINEAR ? 'linear' : cvars === DEFAULTS ? 'defaults' : cvars === EXPOSURE_0 ? 'exposure0' : 'tonemap0only'}${picmip === null ? '' : `-picmip${picmip}`}`;
      const file = host === 'cart' ? await cartShot(map, cvars, out, `${label}-${i}`, picmip) : nativeShot(map, cvars, out, `${label}-${i}`, picmip);
      const img = readPng(file);
      const parts = [];
      for (const p of Object.keys(PANELS)) {
        const got = panelMean(img, p), want = EXPECT[p].map(clamp);
        const ratio = got.map((v, c) => v / want[c]);
        const err = Math.max(...got.map((v, c) => Math.abs(v - want[c])));
        parts.push(`${p} ${got.map((v) => v.toFixed(1)).join('/')} vs ${want.map((v) => v.toFixed(1)).join('/')} (x${(ratio.reduce((a, b) => a + b) / 3).toFixed(3)})`);
        if (checked && err > 1.0) failures.push(`${label}: ${p} renders ${got.map((v) => v.toFixed(1)).join(' ')}, want ${want.map((v) => v.toFixed(1)).join(' ')}`);
      }
      if (checked) {
        // control: the untinted panel must not pass for the tinted expectation
        const got = panelMean(img, 'flat'), want = EXPECT.tint;
        if (Math.max(...got.map((v, c) => Math.abs(v - want[c]))) <= 1.0) failures.push(`${label}: control (flat vs the tinted expectation) matched`);
        // control: the surface-world panel in its zone must not be at the world ambient
        const sz = panelMean(img, 'surfzone');
        if (Math.max(...sz.map((v, c) => Math.abs(v - EXPECT.flat[c]))) <= 1.0) failures.push(`${label}: control (surface-world panel at the world ambient, not its zone's) matched`);
      }
      if (cvars === EXPOSURE_0) {
        const got = panelMean(img, 'flat');
        if (Math.max(...got.map((v, c) => Math.abs(v - EXPECT.flat[c]))) <= 1.0) failures.push(`${label}: control (no final exposure) rendered the flat panel at 1x`);
      }
      rows.push(`${label}: ${parts.join('; ')}`);
    }
  }
  return { ok: failures.length === 0, failures, rows };
}
