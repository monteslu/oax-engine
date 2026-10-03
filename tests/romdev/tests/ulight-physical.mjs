// The physical light description (step 7.5 B, docs/lights.md) against a JS
// oracle (tests/romdev/lib/lightoracle.mjs), on the cart and the native build.
//
// oax_ulight_phys: one light per room over a flat grey floor (texture 128),
// shot straight down from 500 units with tone mapping and exposure off, so
// a floor pixel is 255 * 0.502 * light per channel, clamped at 255. Every
// room's whole visible floor is compared with the oracle of its profile:
//   ue1, q3 (inverse square), q3lin (linear, no angle), doom3, custom (table
//   falloff, soft cap, no angular term, color over 1), image (falloff image),
//   mask (light group 2: the default-group half must stay dark),
//   zone / zonectl (func_oax_zone ambient on half the floor / the same zone
//   without the key), effect (oax_sin pulse at two pinned times).
// Each check has a control that must fail: the oracle with the cap removed,
// the color normalised, the angular term flipped, the falloff swapped, masks
// ignored, the zone ambient dropped or added, the effect at the other time.
// Also: the derived light parameters the engine publishes equal the
// oracle's; cart and native frames agree; zone names drive the location a
// team message reports (g_location: zone, other zone, none).

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';
import { readPng, writePng, readTga } from '../lib/ulight.mjs';
import { runNative } from '../lib/native.mjs';
import { profile, light } from '../lib/lightoracle.mjs';
import { ROOMS, ALBEDO, SPACING, RAMP, roomIndex } from '../../maps/src/oax_ulight_phys.mjs';

export const name = 'ulight-physical';
export const timeoutSec = 180;

const MAP = 'oax_ulight_phys';
const W = 1280, H = 720, CAM = 500, TAN_X = 1, TAN_Y = 720 / 1280;
const LINEAR = 'r_toneMap 0;r_autoExposure 0;r_cameraExposure 1;r_gamma 1';
const T = ALBEDO / 255;
const PARK = `${roomIndex('park') * SPACING} 0 30 90`;
const OK_MEAN = 1.0, OK_MAX = 6;

// the shots: room, pinned shader time (seconds)
const SHOTS = [
  ['ue1', 0], ['q3', 0], ['q3lin', 0], ['doom3', 0], ['custom', 0], ['image', 0],
  ['mask', 0], ['zone', 0], ['zonectl', 0], ['effect', 0], ['effect', 0.5], ['ue1', 0.5],
].map(([room, time]) => ({ room, time, view: `${roomIndex(room) * SPACING} 0 ${CAM} 90 0 0` }));

// where the player stands for the location checks (setviewpos x y z yaw:
// the teleport pushes it on about 175 units along the yaw)
const PLACES = [
  { at: `${roomIndex('zone') * SPACING} 100 30 90`, want: 'Amber Hall' },
  { at: `${roomIndex('zonectl') * SPACING} 100 30 90`, want: 'Plain Hall' },
  { at: PARK, want: 'none' },
];

// floor point under a pixel (camera straight down: image up is +x, right is -y)
function floorAt(px, py) {
  const nx = 2 * (px + 0.5) / W - 1, ny = 1 - 2 * (py + 0.5) / H;
  return [ny * TAN_Y * CAM, -nx * TAN_X * CAM];
}

// 8-bit falloff image, sampled as GL does (bilinear, clamped) at (x, 0.5)
function rampSample(x) {
  const u = x * RAMP.length - 0.5, i = Math.floor(u), f = u - i;
  const at = (k) => RAMP[Math.min(RAMP.length - 1, Math.max(0, k))] / 255;
  return at(i) * (1 - f) + at(i + 1) * f;
}

// the expected floor color at (x, y) (room-local) for a room, with tweaks
// for the controls
function expect(room, x, y, time, tweak = {}) {
  const r = ROOMS[roomIndex(room)];
  let L = [0, 0, 0];
  if (r.keys) {
    const desc = profile(r.keys);
    if (tweak.desc) tweak.desc(desc);
    const dist = Math.hypot(x, y, r.h);
    const masked = !tweak.ignoreMask && !(desc.mask & (y > 0 && room === 'mask' ? 2 : 1));
    if (!masked) L = light(desc, dist, r.h / dist, { ceiling: 2, time, sample: rampSample });
  }
  if (r.zone) {
    const amb = tweak.zone !== undefined ? tweak.zone : r.zone.ambient;
    if (amb && y > 0) L = L.map((c, i) => c + Number(String(amb).split(' ')[i]));
  }
  return L.map((c) => Math.min(255, Math.round(255 * T * c)));
}

function compare(img, room, time, tweak) {
  let sum = 0, max = 0, n = 0;
  for (let py = 2; py < H; py += 4) for (let px = 2; px < W; px += 4) {
    const [x, y] = floorAt(px, py);
    if (Math.abs(x) > 490 || Math.abs(y) > 490 || Math.abs(y) < 4) continue;   // the floor, off the y = 0 seam
    const want = expect(room, x, y, time, tweak);
    const o = (py * W + px) * 4;
    for (let c = 0; c < 3; c++) {
      const d = Math.abs(img.data[o + c] - want[c]);
      sum += d; max = Math.max(max, d); n++;
    }
  }
  return { mean: sum / n, max };
}
const passes = (s) => s.mean <= OK_MEAN && s.max <= OK_MAX;
const fmt = (s) => `mean ${s.mean.toFixed(2)} max ${s.max}`;

// each room's controls: oracle variants that must NOT match
const CONTROLS = {
  ue1: [['line falloff (curve-space)', { desc: (d) => { d.falloff = { mode: 'table', points: [[0, 1], [0.89, 0]] }; } }], ['color normalised', { desc: (d) => { const m = Math.max(...d.color); d.color = d.color.map((c) => c / m); } }]],
  q3: [['no angular term', { desc: (d) => { d.lambert = false; } }]],
  q3lin: [['Lambert on', { desc: (d) => { d.lambert = true; } }]],
  doom3: [['linear falloff', { desc: (d) => { d.falloff = { mode: 'table', points: [[0, 1], [1, 0]] }; } }]],
  custom: [['hard cap', { desc: (d) => { d.knee = 0; } }], ['Lambert on', { desc: (d) => { d.lambert = true; } }]],
  image: [['linear falloff', { desc: (d) => { d.falloff = { mode: 'table', points: [[0, 1], [1, 0]] }; } }]],
  mask: [['masks ignored', { ignoreMask: true }]],
  zone: [['no zone ambient', { zone: null }]],
  zonectl: [['ambient added', { zone: '0.3 0.2 0.1' }]],
  effect: [['effect at the other time', { otherTime: true }]],
};

async function shootCart(out) {
  const s = new Session('ulight-physical-cart');
  try {
    await loadScene(s, MAP, { seed: 1, view: CLEAN_VIEW });
    await s.command(`${LINEAR};r_fixedShaderTime 0;setviewpos ${PARK}`);
    await s.step(60);
    const files = [];
    for (let i = 0; i < SHOTS.length; i++) {
      await s.command(`r_fixedShaderTime ${SHOTS[i].time};cl_overrideView "${SHOTS[i].view}"`);
      await s.step(20);
      const file = path.join(out, `ulight-physical-cart_${i}.png`);
      await s.screenshot(file);
      files.push(file);
    }
    const values = await readValues(s);
    await s.command('cl_overrideView ""');
    const locations = [];
    for (const p of PLACES) {
      await s.command(`setviewpos ${p.at}`);
      await s.step(60);
      locations.push((await readValues(s)).g_location);
    }
    return { files, values, locations };
  } finally {
    await s.shutdown();
  }
}

function shootNative(out) {
  const lines = [...CLEAN_VIEW.split(';'), ...LINEAR.split(';'), 'r_fixedShaderTime 0', `setviewpos ${PARK}`, 'wait 120'];
  SHOTS.forEach((v, i) => lines.push(`r_fixedShaderTime ${v.time}`, `cl_overrideView "${v.view}"`, 'wait 40', `screenshot shot_${i}`, 'wait 2'));
  lines.push('debugvalues values.txt', 'cl_overrideView ""');
  PLACES.forEach((p, i) => lines.push(`setviewpos ${p.at}`, 'wait 120', `debugvalues loc_${i}.txt`));
  const home = runNative('ulight-physical-native', MAP, lines);
  const game = path.join(home, 'baseoa');
  const files = SHOTS.map((_, i) => {
    const tga = path.join(game, 'screenshots', `shot_${i}.tga`);
    if (!fs.existsSync(tga)) throw new Error(`native client wrote no screenshot ${i} (see ${home}/native.log)`);
    const file = path.join(out, `ulight-physical-native_${i}.png`);
    writePng(file, readTga(tga));
    return file;
  });
  const vals = (f) => (fs.existsSync(path.join(game, f)) ? parseDebugValues(fs.readFileSync(path.join(game, f), 'utf8')) : {});
  return { files, values: vals('values.txt'), locations: PLACES.map((_, i) => vals(`loc_${i}.txt`).g_location) };
}

// r_ulight_phys_lights: "ordinal:profile radius intensity cap knee lambert mask r g b effect;..."
function checkParams(values) {
  const failures = [];
  const recs = String(values.r_ulight_phys_lights || '').split(';').filter((r) => r.includes(':'));
  const lit = ROOMS.filter((r) => r.keys);
  if (recs.length !== lit.length) failures.push(`engine published ${recs.length} physical lights, the map has ${lit.length}`);
  recs.forEach((rec, i) => {
    const f = rec.split(':')[1].trim().split(/\s+/).map(Number);
    const d = profile(lit[i].keys);
    const want = [d.radius, d.intensity, d.cap, d.knee, d.lambert ? 1 : 0, d.mask, ...d.color];
    const got = f.slice(1, 10);
    const bad = want.findIndex((w, k) => Math.abs(w - got[k]) > 1e-3 * Math.max(1, Math.abs(w)));
    if (bad >= 0) failures.push(`${lit[i].name}: engine parameter ${bad} = ${got[bad]}, oracle ${want[bad]}`);
  });
  return failures;
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const cart = await shootCart(out);
  const native = shootNative(out);

  for (const [label, run] of [['cart', cart], ['native', native]]) {
    const imgs = run.files.map(readPng);
    SHOTS.forEach((shot, i) => {
      if (shot.room === 'ue1' && shot.time !== 0) return;     // the steady control below
      const s = compare(imgs[i], shot.room, shot.time);
      const ctl = (CONTROLS[shot.room] || []).map(([what, tweak]) => {
        const c = compare(imgs[i], shot.room, tweak.otherTime ? 0.5 - shot.time : shot.time, tweak);
        if (passes(c)) failures.push(`${label} ${shot.room} t=${shot.time}: control (${what}) matched too (${fmt(c)})`);
        return `${what}: ${fmt(c)}`;
      });
      rows.push(`${label} ${shot.room}${shot.time ? ` t=${shot.time}` : ''}: vs oracle ${fmt(s)}; controls ${ctl.join(', ')}`);
      if (!passes(s)) failures.push(`${label} ${shot.room} t=${shot.time}: off the oracle (${fmt(s)})`);
    });
    // a steady light does not change with time; the effect does
    const frameDiff = (a, b) => { let m = 0; for (let k = 0; k < a.data.length; k++) m = Math.max(m, Math.abs(a.data[k] - b.data[k])); return m; };
    const ue1 = SHOTS.findIndex((s) => s.room === 'ue1'), ue1b = SHOTS.findIndex((s) => s.room === 'ue1' && s.time);
    const eff = SHOTS.findIndex((s) => s.room === 'effect'), effb = SHOTS.findIndex((s) => s.room === 'effect' && s.time);
    const steady = frameDiff(imgs[ue1], imgs[ue1b]), pulse = frameDiff(imgs[eff], imgs[effb]);
    rows.push(`${label} effect: steady ue1 lamp t=0 vs 0.5 max diff ${steady}; pulse t=0 vs 0.5 max diff ${pulse}`);
    if (steady !== 0) failures.push(`${label}: the steady lamp changed with time (${steady})`);
    if (pulse < 20) failures.push(`${label}: the pulse did not change with time (${pulse})`);

    const pf = checkParams(run.values);
    rows.push(`${label} parameters: r_ulight_phys "${run.values.r_ulight_phys}", ${pf.length ? pf.join('; ') : 'every derived parameter equals the oracle'}`);
    failures.push(...pf.map((f) => `${label}: ${f}`));
    if (!String(run.values.r_ulight_phys || '').startsWith('8 2.000 1')) failures.push(`${label}: r_ulight_phys "${run.values.r_ulight_phys}", want 8 lights, overbright 2, 1 ambient zone`);

    rows.push(`${label} locations: ${PLACES.map((p, i) => `${p.want} -> ${run.locations[i]}`).join(', ')}`);
    PLACES.forEach((p, i) => { if (run.locations[i] !== p.want) failures.push(`${label}: location at ${p.at} is "${run.locations[i]}", want "${p.want}"`); });
  }

  // cart and native agree
  const a = cart.files.map(readPng), b = native.files.map(readPng);
  let worst = 0, worstMean = 0;
  a.forEach((img, i) => {
    let sum = 0, max = 0;
    for (let k = 0; k < img.data.length; k += 4) for (let c = 0; c < 3; c++) {
      const d = Math.abs(img.data[k + c] - b[i].data[k + c]);
      sum += d; max = Math.max(max, d);
    }
    worst = Math.max(worst, max);
    worstMean = Math.max(worstMean, sum / (img.data.length / 4 * 3));
  });
  rows.push(`cart vs native: worst view mean ${worstMean.toFixed(3)}, max ${worst}`);
  if (worstMean > 0.5 || worst > 6) failures.push(`cart and native differ (mean ${worstMean.toFixed(3)}, max ${worst})`);
  return { ok: failures.length === 0, failures, rows };
}
