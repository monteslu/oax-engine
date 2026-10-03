// Zone ambient on a zone of 300 brushes (step 7.5 B): the renderer once kept
// only the first 256 zone brushes, silently. oax_zone_many's zone has 300;
// the panel under its last brush must get the zone's ambient like the one
// under its first, the panel outside the zone none, and the renderer's
// debug value r_ulight_zones ("zones brushes planes errors") must count all
// 300 brushes and no errors. Control: the outside panel against the zone's
// expectation must fail. Cart and native.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';
import { readPng, writePng, readTga } from '../lib/ulight.mjs';
import { runNative } from '../lib/native.mjs';
import { PANELS, PANEL_HALF, AMBIENT, FLAT, GRID } from '../../maps/src/oax_zone_many.mjs';

export const name = 'ulight-zone-brushes';
export const timeoutSec = 180;

const MAP = 'oax_zone_many';
const W = 1280, H = 720, CAM = 600, TAN_X = 1, TAN_Y = 720 / 1280;
const LINEAR = 'r_toneMap 0;r_autoExposure 0;r_cameraExposure 1;r_gamma 1';
const VIEW = `0 0 ${CAM} 90 0 0`;
const BRUSHES = GRID[0] * GRID[1];

const pixelOf = (x, y) => [Math.round(((-y / (TAN_X * CAM)) + 1) * W / 2 - 0.5), Math.round((1 - x / (TAN_Y * CAM)) * H / 2 - 0.5)];
function panelMean(img, [cx, cy]) {
  const r = PANEL_HALF * 0.6, [ax, ay] = pixelOf(cx + r, cy + r), [bx, by] = pixelOf(cx - r, cy - r);
  const s = [0, 0, 0];
  let n = 0;
  for (let py = Math.min(ay, by); py <= Math.max(ay, by); py++) for (let px = Math.min(ax, bx); px <= Math.max(ax, bx); px++) {
    for (let c = 0; c < 3; c++) s[c] += img.data[(py * W + px) * 4 + c];
    n++;
  }
  return s.map((v) => v / n);
}

async function cart(out) {
  const s = new Session('ulight-zone-brushes-cart');
  try {
    await loadScene(s, MAP, { seed: 1, view: CLEAN_VIEW, picmip: null });
    await s.command(`${LINEAR};cl_overrideView "${VIEW}"`);
    await s.step(40);
    const file = path.join(out, 'ulight-zone-brushes-cart.png');
    await s.screenshot(file);
    return { file, values: await readValues(s) };
  } finally {
    await s.shutdown();
  }
}

function native(out) {
  const home = runNative('ulight-zone-brushes-native', MAP, [...CLEAN_VIEW.split(';'), ...LINEAR.split(';'), `cl_overrideView "${VIEW}"`, 'wait 80', 'screenshot shot_0', 'wait 2', 'debugvalues values.txt'], { picmip: null });
  const game = path.join(home, 'baseoa');
  const tga = path.join(game, 'screenshots', 'shot_0.tga');
  if (!fs.existsSync(tga)) throw new Error(`native client wrote no screenshot (see ${home}/native.log)`);
  const file = path.join(out, 'ulight-zone-brushes-native.png');
  writePng(file, readTga(tga));
  return { file, values: parseDebugValues(fs.readFileSync(path.join(game, 'values.txt'), 'utf8')) };
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const inZone = FLAT.map((v) => v * AMBIENT), outside = [0, 0, 0];
  const err = (got, want) => Math.max(...got.map((v, c) => Math.abs(v - want[c])));
  for (const [label, r] of [['cart', await cart(out)], ['native', native(out)]]) {
    const img = readPng(r.file);
    const got = Object.fromEntries(Object.entries(PANELS).map(([k, p]) => [k, panelMean(img, p)]));
    const zones = String(r.values.r_ulight_zones || '');
    const [nz, nb, , ne] = zones.split(' ').map(Number);
    rows.push(`${label}: r_ulight_zones "${zones}"; first ${got.first.map((v) => v.toFixed(1)).join('/')}, last ${got.last.map((v) => v.toFixed(1)).join('/')}, outside ${got.out.map((v) => v.toFixed(1)).join('/')} (zone ${inZone.join('/')})`);
    if (nz !== 1 || nb !== BRUSHES || ne !== 0) failures.push(`${label}: r_ulight_zones "${zones}", want 1 zone, ${BRUSHES} brushes, 0 errors`);
    if (err(got.first, inZone) > 1) failures.push(`${label}: panel under the first brush ${got.first.join(' ')}`);
    if (err(got.last, inZone) > 1) failures.push(`${label}: panel under the last brush (number ${BRUSHES}) ${got.last.join(' ')}`);
    if (err(got.out, outside) > 1) failures.push(`${label}: panel outside the zone ${got.out.join(' ')}`);
    if (err(got.out, inZone) <= 1) failures.push(`${label}: control (outside panel vs the zone's ambient) matched`);
  }
  return { ok: failures.length === 0, failures, rows };
}
