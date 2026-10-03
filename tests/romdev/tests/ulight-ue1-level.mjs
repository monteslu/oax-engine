// UE1 LevelInfo.Brightness as worldspawn ue1_LevelBrightness.
//
// Measured on the UE1 reference renderer, display chain neutral (linear), of
// the calibration corridors baked with the editor's lighting build at Brightness 1,
// 1.5 and 2, and of the big halls at 1 and 1.5: Brightness is a plain gain
// (fitted peaks 1.50x and 2.00x; one profile gain over corridors and halls
// at 1.5 came out 1.507x the gain at 1). The 0.65 power on a ceiling
// measured before was the reference renderer's display curve.
//
// oax_ue1_level (ue1_LevelBrightness 1.5) shot straight down on the cart and
// native; each lamp's floor is compared with the oracle of the ue1 profile
// at that brightness (mean <= 1, max <= 6 grey levels). Controls that must
// fail: the oracle at Brightness 1, and the old display-space rule
// (Brightness^0.65).

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';
import { readPng, writePng, readTga } from '../lib/ulight.mjs';
import { runNative } from '../lib/native.mjs';
import { profile, light } from '../lib/lightoracle.mjs';
import { LAMPS, WORLD, SPACING, ALBEDO, LEVEL_BRIGHTNESS } from '../../maps/src/oax_ue1_level.mjs';

export const name = 'ulight-ue1-level';
export const timeoutSec = 180;

const MAP = 'oax_ue1_level';
const W = 1280, H = 720, CAM = 500, TAN_X = 1, TAN_Y = 720 / 1280;
const LINEAR = 'r_toneMap 0;r_autoExposure 0;r_cameraExposure 1;r_gamma 1;r_fixedShaderTime 0';
const T = ALBEDO / 255;
const LIT = LAMPS.map((l, i) => ({ ...l, i })).filter((l) => l.keys);
const PARK = `${2 * SPACING} 0 30 90`;

function compare(img, lamp, world, tweak) {
  const desc = profile(lamp.keys, world);
  if (tweak) tweak(desc);
  let sum = 0, max = 0, n = 0;
  for (let py = 2; py < H; py += 4) for (let px = 2; px < W; px += 4) {
    const nx = 2 * (px + 0.5) / W - 1, ny = 1 - 2 * (py + 0.5) / H;
    const x = ny * TAN_Y * CAM, y = -nx * TAN_X * CAM;
    if (Math.abs(x) > 490 || Math.abs(y) > 490) continue;
    const dist = Math.hypot(x, y, lamp.h);
    const want = light(desc, dist, lamp.h / dist, { ceiling: 2 }).map((c) => Math.min(255, Math.round(255 * T * c)));
    const o = (py * W + px) * 4;
    for (let c = 0; c < 3; c++) { const d = Math.abs(img.data[o + c] - want[c]); sum += d; max = Math.max(max, d); n++; }
  }
  return { mean: sum / n, max };
}
const passes = (s) => s.mean <= 1 && s.max <= 6;
const fmt = (s) => `mean ${s.mean.toFixed(2)} max ${s.max}`;
const view = (l) => `${l.i * SPACING} 0 ${CAM} 90 0 0`;

async function cart(out) {
  const s = new Session('ulight-ue1-level-cart');
  try {
    await loadScene(s, MAP, { seed: 1, view: CLEAN_VIEW });
    await s.command(`${LINEAR};setviewpos ${PARK}`);
    await s.step(60);
    const files = [];
    for (const l of LIT) {
      await s.command(`cl_overrideView "${view(l)}"`);
      await s.step(20);
      const f = path.join(out, `ulight-ue1-level-cart-${l.name}.png`);
      await s.screenshot(f);
      files.push(f);
    }
    return { files, values: await readValues(s) };
  } finally {
    await s.shutdown();
  }
}

function native(out) {
  const lines = [...CLEAN_VIEW.split(';'), ...LINEAR.split(';'), `setviewpos ${PARK}`, 'wait 120'];
  LIT.forEach((l, k) => lines.push(`cl_overrideView "${view(l)}"`, 'wait 40', `screenshot shot_${k}`, 'wait 2'));
  lines.push('debugvalues values.txt');
  const home = runNative('ulight-ue1-level-native', MAP, lines);
  const game = path.join(home, 'baseoa');
  const files = LIT.map((l, k) => {
    const tga = path.join(game, 'screenshots', `shot_${k}.tga`);
    if (!fs.existsSync(tga)) throw new Error(`native client wrote no screenshot ${k} (see ${home}/native.log)`);
    const f = path.join(out, `ulight-ue1-level-native-${l.name}.png`);
    writePng(f, readTga(tga));
    return f;
  });
  return { files, values: parseDebugValues(fs.readFileSync(path.join(game, 'values.txt'), 'utf8')) };
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  for (const [label, r] of [['cart', await cart(out)], ['native', native(out)]]) {
    const phys = String(r.values.r_ulight_phys || '');
    if (!phys.endsWith(` ${LEVEL_BRIGHTNESS.toFixed(3)}`)) failures.push(`${label}: r_ulight_phys "${phys}" does not report ue1_LevelBrightness ${LEVEL_BRIGHTNESS}`);
    LIT.forEach((l, k) => {
      const img = readPng(r.files[k]);
      const s = compare(img, l, WORLD);
      const c1 = compare(img, l, {});
      const c2 = compare(img, l, {}, (d) => { d.intensity *= LEVEL_BRIGHTNESS ** 0.65; });
      rows.push(`${label} ${l.name}: vs oracle at ${LEVEL_BRIGHTNESS} ${fmt(s)}; controls: Brightness 1 ${fmt(c1)}, Brightness^0.65 ${fmt(c2)}`);
      if (!passes(s)) failures.push(`${label} ${l.name}: off the oracle (${fmt(s)})`);
      if (passes(c1)) failures.push(`${label} ${l.name}: control (Brightness 1) matched too`);
      if (passes(c2)) failures.push(`${label} ${l.name}: control (Brightness^0.65) matched too`);
    });
  }
  return { ok: failures.length === 0, failures, rows };
}
