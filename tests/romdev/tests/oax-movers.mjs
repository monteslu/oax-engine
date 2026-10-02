// Keyframed and spline movers (oax_movers map, gamecode g_oax_mover.c), on
// the cart and the native build:
//
// - mover0 (four keys, glide, looping) against a JS evaluation of the same
//   keys (lib/oaxtraj.mjs) every server frame of a full cycle on the cart,
//   and at each sample native publishes: tolerance 0.01, exact at key
//   times, monotonic progress along each segment. Control: the same check
//   with one key moved 1 unit must fail.
// - The player spawns on the spline platform (a closed CatmullRom loop) and
//   must still be standing on it 10 s later, with no prediction misses
//   while riding (cg_mispredicts unchanged). Control: teleported to the
//   floor, the riding time drops to 0.
// - Native and cart hash every oax mover's origin and angles each server
//   frame from 1 s to 21 s after it first moved (the platform starts when
//   the player lands, which differs per build): the hashes must be identical. Control: a
//   cart run on a different map reports no mover frames.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues, readValues } from '../lib/values.mjs';
import { keyedLoop } from '../lib/oaxtraj.mjs';
import { MOVER0 } from '../../maps/src/oax_movers.mjs';
import { readPng, distinctColors } from '../lib/png.mjs';

export const name = 'oax-movers';

const TOL = 0.01;
const vec = (s) => String(s || '').split(' ').map(Number);
const maxDiff = (a, b) => Math.max(...a.map((v, i) => Math.abs(v - b[i])));

// angle difference modulo 360
const angDiff = (a, b) => Math.max(...a.map((v, i) => { const d = Math.abs(((v - b[i]) % 360 + 540) % 360 - 180); return d; }));

// samples: [{time, origin, angles}]
function checkKeys(m, start, samples) {
  let maxErr = 0, keyHits = 0, keyErr = 0, nonMono = 0, worst = null;
  const progress = new Map();
  for (const s of samples) {
    const e = keyedLoop(m, start, s.time);
    const err = Math.max(maxDiff(s.origin, e.origin), angDiff(s.angles, e.angles));
    if (err > maxErr) { maxErr = err; worst = `t=${s.time - start}ms got ${s.origin.join(',')} want ${e.origin.map((v) => v.toFixed(4)).join(',')}`; }
    if (e.keyTime) { keyHits++; keyErr = Math.max(keyErr, err); }
    if (e.segment) {
      const d = e.segment.to.map((v, i) => v - e.segment.from[i]);
      const len2 = d.reduce((a, v) => a + v * v, 0);
      const f = len2 ? s.origin.reduce((a, v, i) => a + (v - e.segment.from[i]) * d[i], 0) / len2 : 0;
      const prev = progress.get(e.segment.index);
      if (prev !== undefined && f < prev - 1e-6) nonMono++;
      progress.set(e.segment.index, f);
    }
  }
  return { maxErr, keyHits, keyErr, nonMono, worst };
}

function perturbed() {
  return { ...MOVER0, keys: MOVER0.keys.map((k, i) => (i === 2 ? [k[0] + 1, k[1], k[2]] : k)) };
}

async function cartRun(out) {
  const s = new Session('oax-movers');
  try {
    await s.load();
    await s.command('bot_enable 0; devmap oax_movers');
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
    await s.step(30);
    const v0 = await readValues(s);

    // one full mover0 cycle (8 s) plus a margin, every frame
    const samples = new Map();
    for (let f = 0; f < 600; f++) {
      await s.step(1);
      const v = await readValues(s);
      const t = Number(v.g_mover0_time);
      if (!samples.has(t)) samples.set(t, { time: t, origin: vec(v.g_mover0_origin), angles: vec(v.g_mover0_angles) });
    }
    await s.command('cg_drawGun 0; cg_draw2D 0');
    await s.step(5);
    await s.screenshot(path.join(out, 'oax_movers.png'));

    // ride until 12 s after the level start
    await s.stepUntil('debug_values', (t) => Number(parseDebugValues(t).g_level_rel) >= 12500, 2000, 25);
    const ride = await readValues(s);

    // control: off the platform
    await s.command('setviewpos -700 700 40 0');
    await s.step(60);
    const off = await readValues(s);
    await s.screenshot(path.join(out, 'oax_movers_floor.png'));

    await s.stepUntil('debug_values', (t) => Number(parseDebugValues(t).g_level_rel) > 25000, 3000, 50);
    const end = await readValues(s);
    return { v0, samples: [...samples.values()], ride, off, end };
  } finally {
    await s.shutdown();
  }
}

async function cartControlMap() {
  const s = new Session('oax-movers-control');
  try {
    await s.load();
    await s.command('bot_enable 0; devmap oax_box');
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
    await s.step(30);
    return await readValues(s);
  } finally {
    await s.shutdown();
  }
}

function nativeRun() {
  const home = runNative('oax-movers', 'oax_movers', [
    'bot_enable 0', 'wait 200', 'debugvalues m1.txt',
    'wait 1300', 'debugvalues m2.txt',
    'setviewpos -700 700 40 0', 'wait 150', 'debugvalues m3.txt',
    'wait 2000', 'debugvalues m4.txt',
  ]);
  const read = (f) => {
    const p = path.join(home, 'baseoa', f);
    return fs.existsSync(p) ? parseDebugValues(fs.readFileSync(p, 'utf8')) : {};
  };
  return ['m1.txt', 'm2.txt', 'm3.txt', 'm4.txt'].map(read);
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const start = (v) => Number(v.g_mover0_start);

  const c = await cartRun(out);
  const cs = start(c.v0);
  const k = checkKeys(MOVER0, cs, c.samples);
  const kp = checkKeys(perturbed(), cs, c.samples);
  rows.push(`cart mover0: ${c.samples.length} frames, max error ${k.maxErr.toExponential(2)}, ${k.keyHits} key times (max error ${k.keyErr.toExponential(2)}), ${k.nonMono} backward steps`);
  rows.push(`cart mover0 control (key2 moved 1 unit): max error ${kp.maxErr.toFixed(4)}`);
  if (c.samples.length < 150) failures.push(`cart: only ${c.samples.length} mover0 samples`);
  if (!(k.maxErr <= TOL)) failures.push(`cart mover0 off the analytic path by ${k.maxErr} (${k.worst})`);
  if (k.keyHits < 6) failures.push(`cart mover0: only ${k.keyHits} samples at key times`);
  if (k.nonMono) failures.push(`cart mover0: ${k.nonMono} backward steps`);
  if (!(kp.maxErr > TOL)) failures.push('control: a perturbed key matched the mover');

  const plat = c.v0.g_platform_ent;
  rows.push(`cart platform: riding ${c.ride.g_platform_riding} ms at t+${c.ride.g_level_rel} (ground ${c.ride.g_client0_ground}, platform ${plat}), mispredicts ${c.v0.cg_mispredicts} -> ${c.ride.cg_mispredicts}; after setviewpos: riding ${c.off.g_platform_riding}, ground ${c.off.g_client0_ground}`);
  if (!(Number(c.ride.g_platform_riding) >= 10000)) failures.push(`cart: rode the platform only ${c.ride.g_platform_riding} ms`);
  if (c.ride.g_client0_ground !== plat) failures.push('cart: not standing on the platform at the end of the ride');
  if (c.ride.cg_mispredicts !== c.v0.cg_mispredicts) failures.push(`cart: ${Number(c.ride.cg_mispredicts) - Number(c.v0.cg_mispredicts)} prediction misses while riding`);
  if (Number(c.off.g_platform_riding) !== 0 || c.off.g_client0_ground === plat) failures.push('control: teleported off the platform but still riding');

  // native
  const [n1, n2, n3, n4] = nativeRun();
  const ns = start(n1);
  const nsamples = [n1, n2, n3, n4].filter((v) => v.g_mover0_time).map((v) => ({ time: Number(v.g_mover0_time), origin: vec(v.g_mover0_origin), angles: vec(v.g_mover0_angles) }));
  const nk = checkKeys(MOVER0, ns, nsamples);
  rows.push(`native mover0: ${nsamples.length} samples (t+${nsamples.map((x) => x.time - ns).join(', ')}), max error ${nk.maxErr.toExponential(2)}`);
  if (nsamples.length < 4) failures.push('native: missing debug values');
  if (!(nk.maxErr <= TOL)) failures.push(`native mover0 off the analytic path by ${nk.maxErr} (${nk.worst})`);
  rows.push(`native platform: riding ${n2.g_platform_riding} ms at t+${n2.g_level_rel}, mispredicts ${n1.cg_mispredicts} -> ${n2.cg_mispredicts}; after setviewpos: riding ${n3.g_platform_riding}`);
  if (!(Number(n2.g_platform_riding) >= 10000)) failures.push(`native: rode the platform only ${n2.g_platform_riding} ms (at t+${n2.g_level_rel})`);
  if (n2.g_client0_ground !== n1.g_platform_ent) failures.push('native: not standing on the platform');
  if (n2.cg_mispredicts !== n1.cg_mispredicts) failures.push('native: prediction misses while riding');
  if (Number(n3.g_platform_riding) !== 0) failures.push('native control: teleported off the platform but still riding');

  rows.push(`hash: cart ${c.end.g_mover_hash} (${c.end.g_mover_frames} frames), native ${n4.g_mover_hash} (${n4.g_mover_frames} frames, t+${n4.g_level_rel})`);
  if (!(Number(n4.g_level_rel) > 25000)) failures.push(`native ended at t+${n4.g_level_rel}, before the hash window closed`);
  if (!c.end.g_mover_hash || c.end.g_mover_hash !== n4.g_mover_hash || c.end.g_mover_frames !== n4.g_mover_frames) failures.push('native and cart mover traces differ');
  if (!(Number(c.end.g_mover_frames) >= 3 * 390)) failures.push(`cart hashed only ${c.end.g_mover_frames} frames`);

  const box = await cartControlMap();
  rows.push(`control oax_box: mover frames ${box.g_mover_frames}, hash ${box.g_mover_hash}`);
  if (box.g_mover_hash === c.end.g_mover_hash) failures.push('control: a map without movers produced the same hash');

  const shot = path.join(out, 'oax_movers.png');
  if (fs.existsSync(shot)) {
    const n = distinctColors(readPng(shot), 1 << 20);
    rows.push(`screenshot ${path.basename(shot)}: ${n} colours`);
    if (n < 500) failures.push(`screenshot has only ${n} colours`);
  } else failures.push('no screenshot');
  return { ok: failures.length === 0, failures, rows };
}
