#!/usr/bin/env node
// ue1-tail-fit.mjs: refit the ue1 light profile's gain, per-lamp floor and
// tail end on the UE1 calibration cells (tests/romdev/reference/
// ue1-light-calib.json, from misc/tools/ue1-light-calib.mjs), with the
// falloff 1 - smoothstep(x / zero): zero < 1 ends the tail earlier. Reports
// the best fit and the per-x-bin residual (oax - UE1, grey) of each model,
// so a tail change can be judged where the lamp count amplifies it.
//
//   node misc/tools/ue1-tail-fit.mjs [--zero 0.90,0.92,...] [--fixed gain,floor,zero]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { profile, light, UE1 } from '../../tests/romdev/lib/lightoracle.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const ref = JSON.parse(fs.readFileSync(path.join(here, '..', '..', 'tests', 'romdev', 'reference', 'ue1-light-calib.json'), 'utf8'));
const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(`--${k}`); return i >= 0 ? args[i + 1] : d; };
const zeros = opt('zero', '1.0,0.98,0.97,0.96,0.95,0.94,0.93,0.92,0.90').split(',').map(Number);
const tex = ref.textureMean;
const lampKeys = (l) => Object.fromEntries(Object.entries(l.props).map(([k, v]) => [`ue1_${k}`, v]));
const lampChannel = (l) => ((l.props.LightSaturation ?? 255) < 64 && (l.props.LightHue ?? 0) > 150 ? 'blue' : 'mean');

// candidate falloff shapes over x = d / R (1 at 0, 0 at 1)
const SHAPES = {
  smooth: (x) => 1 - (3 * x * x - 2 * x * x * x),          // 1 - smoothstep (the current profile)
  cos: (x) => (1 + Math.cos(Math.PI * x)) / 2,              // raised cosine
  quad: (x) => (1 - x) * (1 - x),                            // q3map2's quadratic
  cossq: (x) => Math.cos(Math.PI * x / 2) ** 2,              // = raised cosine (identity), kept as a check
  smoothpow: (x) => Math.pow(1 - (3 * x * x - 2 * x * x * x), 1.08),
};
const shape = opt('shape', 'smooth');

// the pixel for a lamp at (x, cos) under a model {gain, floor, zero, shape}
function predict(set, li, x, cos, m) {
  const lamp = ref.sets[set].lamps[li];
  const saved = { ...UE1 };
  Object.assign(UE1, { gain: m.gain, floor: m.floor, falloff: 'smooth' });
  const desc = profile(lampKeys(lamp));
  Object.assign(UE1, saved);
  const xz = x / m.zero;
  let L;
  if ((m.shape || shape) === 'smooth') {
    L = light(desc, xz * desc.radius, cos, { ceiling: 2 });
  } else {
    const fn = SHAPES[m.shape || shape];
    const v = xz >= 1 ? 0 : desc.intensity * fn(xz);
    const ang = Math.max(0, cos);
    L = desc.color.map((c) => Math.max(Math.min(v * ang * c, 2) - m.floor, 0));
  }
  const px = L.map((c, i) => 255 * tex[i] * c);
  return lampChannel(lamp) === 'blue' ? px[2] : (px[0] + px[1] + px[2]) / 3;
}

const cells = ref.cells;
function score(m) {
  let se = 0, n = 0;
  for (const c of cells) { const e = predict(c.set, c.lamp, c.x, c.cos, m) - c.ref; se += e * e; n++; }
  return Math.sqrt(se / n);
}
function residualBins(m) {
  const bins = new Map();
  for (const c of cells) {
    const b = Math.min(0.95, Math.floor(c.x / 0.1) * 0.1).toFixed(1);
    const e = predict(c.set, c.lamp, c.x, c.cos, m) - c.ref;
    const r = bins.get(b) || { n: 0, sum: 0, abs: 0 };
    r.n++; r.sum += e; r.abs += Math.abs(e);
    bins.set(b, r);
  }
  return [...bins.entries()].sort((a, b) => Number(a[0]) - Number(b[0])).map(([b, r]) => `${b}:${(r.sum / r.n).toFixed(2)}(${r.n})`).join(' ');
}

const fixed = opt('fixed', null);
if (fixed) {
  const [gain, floor, zero] = fixed.split(',').map(Number);
  const m = { gain, floor, zero };
  console.log(`model gain ${gain} floor ${floor} zero ${zero}: rms ${score(m).toFixed(3)}; mean residual by x bin ${residualBins(m)}`);
  process.exit(0);
}
const current = { gain: UE1.gain, floor: UE1.floor, zero: 1 };
console.log(`shape ${shape}; current gain ${current.gain} floor ${current.floor} zero 1: rms ${score(current).toFixed(3)}`);
console.log(`  residual by x bin ${residualBins(current)}`);
for (const zero of zeros) {
  let best = null;
  for (let g = 0.0120; g <= 0.0136; g += 0.00005) {
    for (let q = 0; q <= 0.012; q += 0.00025) {
      const rms = score({ gain: g, floor: q, zero });
      if (!best || rms < best.rms) best = { rms, gain: g, floor: q };
    }
  }
  const m = { gain: best.gain, floor: best.floor, zero };
  console.log(`zero ${zero}: best gain ${best.gain.toFixed(5)} floor ${best.floor.toFixed(5)} rms ${best.rms.toFixed(3)}`);
  console.log(`  residual by x bin ${residualBins(m)}`);
}
