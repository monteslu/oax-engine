// Zone volumes (func_oax_zone, design 2.2) on oax_zones, on the cart and the
// native build:
// - jump apex in the low-gravity zone (200) against v0^2/2g, within 1 unit;
//   control: the same jump outside every zone (800) must not reach it;
// - the +X current: airborne X velocity grows 8 ups per 8 ms step to 200
//   and the drift matches that model; control: no drift outside;
// - a run through all three zones: native and cart traces identical, and
//   the published per-frame zone (g_zone_log) identical, visiting 0 -1 1 -1 2;
// - reverb: the audio tail after an impulse is longer in the reverb zone
//   than outside it (cart: recorded WAV and s_meter; native: s_meter).
// Zero drift on maps without zones is movement-traces / native-parity.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, placeAt } from '../lib/scenes.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues, readValues } from '../lib/values.mjs';
import { cartRun, nativeRun, compareRows } from '../lib/sim.mjs';

export const timeoutSec = 2400;
export const name = 'oax-zones';

const MAP = 'oax_zones';
// setviewpos launches the player 400 ups along the yaw; these land (about
// 175 units on) in: the low-gravity zone, the current zone, the plain gap,
// and the fog + reverb room
const LOW = { x: -768, y: 0, z: 32, yaw: 0 };
const CUR = { x: -150, y: 0, z: 32, yaw: 0 };
const GAP = { x: 480, y: 0, z: 32, yaw: 180 };
const FOG = { x: 1200, y: 0, z: 32, yaw: 180 };
const JUMP_V = 270;
const IMPULSE = 'sound/misc/menu1.wav';

const rise = (rows) => Math.max(...rows.map((r) => r[3])) - rows[0][3];
const airborne = (rows) => rows.filter((r) => r[7] === -1);

// RMS envelope tail of a 16-bit WAV: ms from the loudest 10 ms window until
// the last window above -40 dB of it
function wavTail(file) {
  const b = fs.readFileSync(file);
  let p = 12, ch = 2, rate = 48000, data = null;
  while (p + 8 <= b.length) {
    const id = b.toString('latin1', p, p + 4), n = b.readUInt32LE(p + 4);
    if (id === 'fmt ') { ch = b.readUInt16LE(p + 10); rate = b.readUInt32LE(p + 12); }
    if (id === 'data') data = b.subarray(p + 8, p + 8 + n);
    p += 8 + n + (n & 1);
  }
  const frames = data.length / 2 / ch, win = Math.round(rate / 100), rms = [];
  for (let i = 0; i + win <= frames; i += win) {
    let e = 0;
    for (let k = i; k < i + win; k++) for (let c = 0; c < ch; c++) e += data.readInt16LE((k * ch + c) * 2) ** 2;
    rms.push(Math.sqrt(e / win / ch));
  }
  const peak = Math.max(...rms), at = rms.indexOf(peak);
  let last = at;
  rms.forEach((r, i) => { if (r > peak / 100) last = i; });
  return { peak: Math.round(peak), tailMs: (last - at) * 10 };
}

const meter = (v) => { const [peak, tail] = String(v || '0 0').split(' ').map(Number); return { peak, tailMs: tail }; };

async function cartReverb(out) {
  const s = new Session('oax-zones-reverb');
  const res = {};
  try {
    await loadScene(s, MAP);
    // control first, so no earlier sound is still ringing in the hall
    for (const [tag, p] of [['outside', GAP], ['inside', FOG]]) {
      await placeAt(s, p, 300);   // the teleport sound rings out first
      const v = await readValues(s);
      await s.command(`s_meter 1; play ${IMPULSE}`);
      const wav = path.join(out, `oax_zones_reverb_${tag}.wav`);
      await s.call('audioDebug', { op: 'record', frames: 180, path: wav });
      const m = meter((await readValues(s)).s_meter);
      res[tag] = { zone: v.cg_eyezone, reverb: v.cg_reverb, wav: wavTail(wav), meter: m };
      await s.command('s_meter 0');
      await s.step(1);
    }
  } finally {
    await s.shutdown();
  }
  return res;
}

function nativeReverb() {
  // the DMA mixer (not OpenAL) with SDL's silent dummy driver
  const env = process.env.SDL_AUDIODRIVER;
  process.env.SDL_AUDIODRIVER = 'dummy';
  const res = {};
  try {
    for (const [tag, p] of [['outside', GAP], ['inside', FOG]]) {
      const home = runNative(`oax-zones-reverb-${tag}`, MAP, [
        'set s_useOpenAL 0', 'snd_restart', 'wait 60', `setviewpos ${p.x} ${p.y} ${p.z} ${p.yaw}`,
        'wait 3000', `s_meter 1`, `play ${IMPULSE}`,
        // the DMA clock follows SDL's (real-time) dummy device while frames run
        // unthrottled: about 2.5 s of audio
        'wait 3000', 'debugvalues reverb.txt',
      ]);
      const f = path.join(home, 'baseoa', 'reverb.txt');
      const v = fs.existsSync(f) ? parseDebugValues(fs.readFileSync(f, 'utf8')) : {};
      res[tag] = { zone: v.cg_eyezone, reverb: v.cg_reverb, meter: meter(v.s_meter) };
    }
  } finally {
    if (env === undefined) delete process.env.SDL_AUDIODRIVER; else process.env.SDL_AUDIODRIVER = env;
  }
  return res;
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const fail = (m) => failures.push(m);

  // --- cart runs (one session each) ---
  const cart = {};
  for (const [key, p, script] of [['low', LOW, 'zjump'], ['gap', GAP, 'zjump'], ['cur', CUR, 'zjump'], ['tour', LOW, 'ztour']]) {
    const s = new Session(`oax-zones-${key}`);
    try {
      cart[key] = await cartRun(s, MAP, p, script);
    } finally {
      await s.shutdown();
    }
    fs.writeFileSync(path.join(out, `oax_zones_${key}.cart.json`), JSON.stringify(cart[key].rows));
  }
  const native = {
    low: nativeRun('oax-zones-low', MAP, LOW, 'zjump'),
    cur: nativeRun('oax-zones-cur', MAP, CUR, 'zjump'),
    tour: nativeRun('oax-zones-tour', MAP, LOW, 'ztour'),
  };

  // --- apex in low gravity, against the analytic height ---
  const g = 200, want = (JUMP_V * JUMP_V) / (2 * g);
  for (const [b, r] of [['cart', cart.low], ['native', native.low]]) {
    const h = rise(r.rows);
    rows.push(`${b}: low-gravity jump rise ${h.toFixed(3)} (v0^2/2g = ${want}), zone ${r.values.g_zone}`);
    if (Math.abs(h - want) > 1) fail(`${b}: low-gravity apex ${h.toFixed(3)}, want ${want} +- 1`);
    if (r.values.g_zone !== '0') fail(`${b}: g_zone ${r.values.g_zone} in the low-gravity zone, want 0`);
  }
  const hGap = rise(cart.gap.rows), want800 = (JUMP_V * JUMP_V) / (2 * 800);
  rows.push(`control: jump outside any zone rises ${hGap.toFixed(3)} (800: ${want800.toFixed(3)}), zone ${cart.gap.values.g_zone}`);
  if (Math.abs(hGap - want800) > 1) fail(`control: plain jump rose ${hGap.toFixed(3)}, want ${want800.toFixed(3)}`);
  if (Math.abs(hGap - want) <= 1) fail('control: a jump outside the zone reached the low-gravity apex');

  // --- current: 5 * 0.008 * 200 = 8 ups per 8 ms step, capped at 200 ---
  for (const [b, r] of [['cart', cart.cur], ['native', native.cur]]) {
    const air = airborne(r.rows);
    let maxDv = 0, steps = 0;
    for (let i = 1; i < air.length; i++) {
      const dt = air[i][0] - air[i - 1][0];
      if (dt !== 16 || air[i][4] >= 200) continue;
      maxDv = Math.max(maxDv, Math.abs(air[i][4] - air[i - 1][4] - 16));
      steps++;
    }
    const vmax = Math.max(...air.map((x) => x[4]));
    // drift model: each 8 ms step moves x by 0.008 * vx after that step's
    // pull; between two rows that is the step in the middle,
    // min(200, vx(prev row) + 8), and the row's own vx
    let model = 0, drift = 0;
    for (let i = 1; i < air.length; i++) {
      if (air[i][0] - air[i - 1][0] !== 16) continue;
      model += 0.008 * (Math.min(200, air[i - 1][4] + 8) + air[i][4]);
      drift += air[i][1] - air[i - 1][1];
    }
    rows.push(`${b}: current: ${air.length} airborne rows, dvx/16ms off by at most ${maxDv.toExponential(2)} over ${steps} rows, max vx ${vmax.toFixed(4)}, drift ${drift.toFixed(3)} (model ${model.toFixed(3)})`);
    if (steps < 10 || maxDv > 0.01) fail(`${b}: current accelerates wrong (${steps} rows, off by ${maxDv})`);
    if (Math.abs(vmax - 200) > 0.01) fail(`${b}: current top speed ${vmax}, want 200`);
    if (Math.abs(drift - model) > 1) fail(`${b}: current drift ${drift.toFixed(3)} vs model ${model.toFixed(3)}`);
  }
  const gapVx = Math.max(...airborne(cart.gap.rows).map((x) => Math.abs(x[4])));
  rows.push(`control: outside the current the airborne |vx| stays ${gapVx}`);
  if (gapVx > 0.01) fail(`control: drift outside the current (${gapVx})`);

  // --- native vs cart identity ---
  for (const k of ['low', 'cur', 'tour']) {
    const c = compareRows(native[k].rows, cart[k].rows);
    rows.push(`${k}: ${c.common} common command times (native ${native[k].rows.length}, cart ${cart[k].rows.length}), ${c.firstDiff ? 'first difference ' + c.firstDiff : 'identical'}`);
    if (c.common < 100) fail(`${k}: only ${c.common} rows in common`);
    if (c.firstDiff) fail(`${k}: native and cart differ at ${c.firstDiff}`);
  }
  // control for the comparison: a different script must not match
  const ctl = compareRows(native.tour.rows, cart.low.rows);
  rows.push(`control: tour vs zjump ${ctl.firstDiff ? 'differs at ' + ctl.firstDiff : 'IDENTICAL'}`);
  if (!ctl.firstDiff) fail('control: different scripts compared identical');

  // --- the per-frame zone, published by the game ---
  const logs = { cart: cart.tour.values.g_zone_log, native: native.tour.values.g_zone_log };
  const seq = (log, t0) => String(log || '').trim().split(' ').map((e) => e.split(':').map(Number)).filter(([t]) => t >= t0).map(([, z]) => z);
  for (const [b, r] of [['cart', cart.tour], ['native', native.tour]]) {
    const visited = seq(r.values.g_zone_log, r.start);
    rows.push(`${b}: g_zone_log "${r.values.g_zone_log}" -> zones after the start ${visited.join(' ')}; cg_zone ${r.values.cg_zone} g_zone ${r.values.g_zone}`);
    if (visited.join(' ') !== '-1 1 -1 2') fail(`${b}: tour visited zones ${visited.join(' ')}, want -1 1 -1 2 (after starting in 0)`);
    if (r.values.cg_zone !== r.values.g_zone) fail(`${b}: predicted zone ${r.values.cg_zone} vs game zone ${r.values.g_zone}`);
  }
  const rel = (log, t0) => String(log || '').trim().split(' ').map((e) => e.split(':').map(Number)).filter(([t]) => t >= t0).map(([t, z]) => `${t - t0}:${z}`).join(' ');
  const lc = rel(logs.cart, cart.tour.start), ln = rel(logs.native, native.tour.start);
  rows.push(`zone changes from the script start: cart ${lc} | native ${ln}`);
  if (lc !== ln) fail('zone change times differ between native and cart');

  // --- reverb ---
  const cr = await cartReverb(out);
  for (const t of ['outside', 'inside']) {
    rows.push(`cart reverb ${t}: eye zone ${cr[t].zone}, ${cr[t].reverb}; WAV tail ${cr[t].wav.tailMs} ms (peak rms ${cr[t].wav.peak}); s_meter tail ${cr[t].meter.tailMs} ms`);
  }
  if (!(cr.inside.wav.tailMs > cr.outside.wav.tailMs * 2 && cr.inside.wav.tailMs - cr.outside.wav.tailMs > 300)) {
    fail(`cart: reverb did not lengthen the recorded tail (${cr.outside.wav.tailMs} -> ${cr.inside.wav.tailMs} ms)`);
  }
  if (!(cr.inside.meter.tailMs > cr.outside.meter.tailMs + 150)) fail(`cart: s_meter tail ${cr.outside.meter.tailMs} -> ${cr.inside.meter.tailMs} ms`);
  if (cr.outside.wav.peak < 100) fail('cart: the impulse was not heard at all (control)');
  const nr = nativeReverb();
  for (const t of ['outside', 'inside']) rows.push(`native reverb ${t}: eye zone ${nr[t].zone}, ${nr[t].reverb}; s_meter peak ${nr[t].meter.peak} tail ${nr[t].meter.tailMs} ms`);
  if (!(nr.inside.meter.tailMs > nr.outside.meter.tailMs + 150)) fail(`native: s_meter tail ${nr.outside.meter.tailMs} -> ${nr.inside.meter.tailMs} ms`);
  if (!nr.outside.meter.peak) fail('native: the impulse was not metered (control)');

  return { ok: failures.length === 0, failures, rows };
}
