// Light styles (design 2.5) on both builds.
//
// oax_lightstyle: room A's far end is lit by a switched light (style 32),
// its near end by an unstyled fill light; a one-shot trigger toggles the
// switched light off. Room B has a flicker light (style 33, preset 1).
//
// Checks, each next to a control that must fail:
// - the game, cgame and renderer agree on the switched style (g_, cg_,
//   r_lightstyle32: 1 before the trigger, 0 after);
// - on and off frames differ in the lit region (far end) and match in the
//   fill-lit near region; controls: two "on" frames do not differ, and the
//   near region of two different cameras does not match;
// - every flicker value the cgame published equals the pattern letter at
//   its cg.time, (letter - 'a') / 12, and the renderer got the same value;
//   control: the same check against a shifted clock fails;
// - flicker frames with different letters differ in brightness, frames
//   with the same letter match (cart);
// - cart goldens of the on and off frames.

import fs from 'node:fs';
import path from 'node:path';
import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { comparePng, readPng, halfSize, writePng } from '../lib/png.mjs';
import { diffFraction, meanLuma } from '../lib/imgstat.mjs';
import { FLICKER_PATTERN, TRIGGER } from '../../maps/src/oax_lightstyle.mjs';

export const name = 'oax-lightstyle';

const MAP = 'oax_lightstyle';
const SETUP = ['r_fixedShaderTime 100', 'r_autoExposure 0'];
// the player stands in the room the camera looks at: the snapshot's area
// mask comes from the player, and the two rooms are separate areas
const IN_A = 'setviewpos -800 0 8 0';
const IN_B = 'setviewpos 400 0 8 0';
const CAM_A = 'cl_overrideView "-100 0 120 5 180 0"';
const CAM_A2 = 'cl_overrideView "-600 0 120 5 90 0"';
const CAM_B = 'cl_overrideView "200 0 100 10 0 0"';
const INTO_TRIGGER = `setviewpos ${(TRIGGER.mins[0] + TRIGGER.maxs[0]) / 2} ${(TRIGGER.mins[1] + TRIGGER.maxs[1]) / 2} 8 0`;
const FAR = { x0: 0.35, y0: 0.3, x1: 0.65, y1: 0.55 }; // the far end of room A
const NEAR = { y0: 0.82 };                              // floor by the camera, fill light only
const TOLERANCE = 24;

// the value a style has at cg.time t: one letter per 100 ms
export function patternValue(t, pattern = FLICKER_PATTERN, rate = 10) {
  const c = pattern[Math.floor(t / Math.floor(1000 / rate)) % pattern.length];
  return (c.charCodeAt(0) - 97) / 12;
}

function shotList(flickerFrames) {
  const list = [
    { cmd: `${IN_A};${CAM_A}`, name: 'on', values: 'on', settle: 40 },
    { name: 'on2', settle: 10 },
    { cmd: CAM_A2, name: 'a2' },
    { cmd: INTO_TRIGGER, settle: 20 },
    { cmd: `${IN_A};${CAM_A}`, name: 'off', values: 'off', settle: 40 },
    { cmd: `${IN_B};${CAM_B}`, settle: 40 },
  ];
  for (let i = 0; i < flickerFrames; i++) list.push({ name: `fl${i}`, values: `fl${i}`, settle: 3 });
  return list;
}

function checkBuild(build, r, failures, rows, { flickerPixels = true } = {}) {
  const on = r.valuesAt.on, off = r.valuesAt.off;
  rows.push(`${build}: style 32 g/cg/r ${on.g_lightstyle32}/${on.cg_lightstyle32}/${on.r_lightstyle32} on, ${off.g_lightstyle32}/${off.cg_lightstyle32}/${off.r_lightstyle32} after the trigger`);
  if (on.g_lightstyle32 !== '1' || Number(on.cg_lightstyle32) !== 1 || Number(on.r_lightstyle32) !== 1) failures.push(`${build}: switched light not on at start`);
  if (off.g_lightstyle32 !== '0' || Number(off.cg_lightstyle32) !== 0 || Number(off.r_lightstyle32) !== 0) failures.push(`${build}: the trigger did not switch the light off`);

  const im = r.images;
  const farOnOff = diffFraction(im.on, im.off, TOLERANCE, FAR);
  const nearOnOff = diffFraction(im.on, im.off, 8, NEAR);
  const farOnOn = diffFraction(im.on, im.on2, TOLERANCE, FAR);
  const nearCtl = diffFraction(im.on, im.a2, 8, NEAR);
  rows.push(`${build}: lit region ${(farOnOff * 100).toFixed(1)}% changed on->off (control on->on ${(farOnOn * 100).toFixed(2)}%); near region ${(nearOnOff * 100).toFixed(2)}% changed (control other camera ${(nearCtl * 100).toFixed(1)}%)`);
  if (farOnOff < 0.2) failures.push(`${build}: switching the light changed only ${(farOnOff * 100).toFixed(1)}% of the lit region`);
  if (nearOnOff > 0.01) failures.push(`${build}: switching the light changed the fill-lit region (${(nearOnOff * 100).toFixed(2)}%)`);
  if (farOnOn > 0.01) failures.push(`${build}: control failed: two "on" frames differ (${(farOnOn * 100).toFixed(2)}%)`);
  if (nearCtl < 0.05) failures.push(`${build}: control did not fail: another camera's near region matches`);

  // flicker values against the pattern, and against a shifted clock
  const samples = Object.keys(r.valuesAt).filter((k) => k.startsWith('fl')).map((k) => r.valuesAt[k])
    .map((v) => ({ t: Number(v.cg_lightstyle33_time), v: Number(v.cg_lightstyle33), rv: Number(v.r_lightstyle33) }))
    .filter((s) => Number.isFinite(s.t));
  const uniq = [...new Map(samples.map((s) => [s.t, s])).values()];
  const bad = uniq.filter((s) => Math.abs(s.v - patternValue(s.t)) > 1e-5);
  const badR = uniq.filter((s) => Math.abs(s.rv - s.v) > 1e-5);
  const shifted = uniq.filter((s) => Math.abs(s.v - patternValue(s.t + 100)) > 1e-5);
  rows.push(`${build}: ${uniq.length} flicker samples (${[...new Set(uniq.map((s) => s.v.toFixed(3)))].join(' ')}), ${bad.length} off-pattern, renderer mismatches ${badR.length}; control: ${shifted.length} off a clock 100 ms late`);
  if (uniq.length < 4) failures.push(`${build}: only ${uniq.length} flicker samples`);
  if (bad.length) failures.push(`${build}: flicker values off the pattern: ${bad.slice(0, 3).map((s) => `t=${s.t} v=${s.v}`).join(', ')}`);
  if (badR.length) failures.push(`${build}: renderer style value differs from cgame's`);
  if (!shifted.length) failures.push(`${build}: control did not fail: values also match a shifted clock`);

  // pixels: frames grouped by the value current when they were taken (the
  // cart reads values from the frame it shot; a native debugvalues dump
  // lags its screenshot by a frame, so native checks values only)
  if (!flickerPixels) return;
  const frames = Object.keys(im).filter((k) => k.startsWith('fl')).map((k) => ({ img: im[k], v: Number(r.valuesAt[k].cg_lightstyle33) }));
  const byV = new Map();
  for (const f of frames) byV.set(f.v, [...(byV.get(f.v) || []), f.img]);
  const levels = [...byV.entries()].map(([v, imgs]) => ({ v, luma: meanLuma(imgs[0]) })).sort((a, b) => a.v - b.v);
  rows.push(`${build}: flicker brightness ${levels.map((l) => `${l.v.toFixed(3)}:${l.luma.toFixed(1)}`).join(' ')}`);
  for (let i = 1; i < levels.length; i++) {
    if (levels[i].luma <= levels[i - 1].luma) failures.push(`${build}: a brighter style value (${levels[i].v.toFixed(3)}) did not render brighter`);
  }
  for (const [v, imgs] of byV) {
    if (imgs.length > 1 && diffFraction(imgs[0], imgs[1], 8) > 0.01) failures.push(`${build}: two frames at style value ${v} differ`);
  }
  if (levels.length < 2) failures.push(`${build}: flicker frames covered only ${levels.length} value(s)`);
}

// the native half alone (also usable without a romdev server)
export function nativeChecks(failures, rows) {
  const native = nativeShots('oax-lightstyle', MAP, shotList(16), { setup: SETUP });
  checkBuild('native', native, failures, rows, { flickerPixels: false });
  return native;
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const gdir = path.join(goldens, 'oax');
  fs.mkdirSync(gdir, { recursive: true });

  const cart = await cartShots('oax-lightstyle', MAP, shotList(16), { setup: SETUP, out });
  checkBuild('cart', cart, failures, rows);
  for (const n of ['on', 'off']) {
    const golden = path.join(gdir, `lightstyle_${n}.png`);
    const half = halfSize(cart.images[n]);
    if (update || !fs.existsSync(golden)) {
      writePng(golden, half);
      rows.push(`cart golden ${n} ${update ? 'updated' : 'created'}`);
      continue;
    }
    const r = comparePng(readPng(golden), half, { tolerance: TOLERANCE, diffPath: path.join(out, `oax-lightstyle_${n}.diff.png`) });
    rows.push(`cart golden ${n}: ${(r.badFraction * 100).toFixed(3)}% over tolerance`);
    if (!r.sameSize || r.badFraction > 0.005) failures.push(`cart golden ${n}: ${(r.badFraction * 100).toFixed(2)}% differ`);
  }

  nativeChecks(failures, rows);
  return { ok: failures.length === 0, failures, rows };
}
