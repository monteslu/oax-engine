#!/usr/bin/env node
// ue1-light-calib.mjs: UE1's own lighting, in linear space, as the
// calibration points the engine's ue1 light profile is tested against
// (tests/romdev/reference/ue1-light-calib.json), and the fit of the profile.
//
// The shots: UE1 test maps baked with the UE1 editor's lighting build
// (driven by an external converter's calibration shim) and shot by the UE1
// reference renderer with its display chain neutral (Brightness 0.5,
// GammaOffset 0: no brightness lift, no gamma), so a pixel is texel x light,
// linear:
//   calib  four calibration corridors (2000 x 800 x 800), one lamp each
//          with small-lamp settings from a reference map, five cameras each;
//   big    two 7000 x 3000 x 2400 halls, lamps at LightRadius 200 and 255
//          (a reference map's deck lamps), eight cameras each.
// Every wall is a smooth stone texture; only its mean colour is kept (as a
// number). Each pixel is traced to its wall point (UE1 is left-handed: the
// screen's right is (-sin yaw, cos yaw)), giving x = d / WorldLightRadius and
// N.L; cells of (set, lamp, x, N.L) keep the median reference pixel minus
// its black level. No reference art is kept.
//
// Shots are read as ref_000.png, ref_001.png, ... in camera order.
//
//   node misc/tools/ue1-light-calib.mjs --calib <dir> --big <dir> --texture wall.tga [--out file.json]
//
// It fits the profile's gain (light per unit of LightBrightness) and its
// per-lamp floor (light taken off each lamp's own contribution) together,
// with the falloff 1 - smoothstep(d / R) held fixed, and prints the error of
// that model and of the curve-space profile it replaces on the same cells.
// (A gain fitted alone comes out low: it absorbs the floor's missing tail.)

import fs from 'node:fs';
import path from 'node:path';
import { readPng } from '../../tests/romdev/lib/png.mjs';
import { profile, light, UE1 } from '../../tests/romdev/lib/lightoracle.mjs';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => (v.startsWith('--') ? [...a, [v.slice(2), all[i + 1]]] : a), []));
if (!args.calib || !args.big || !args.texture) {
  console.error('usage: ue1-light-calib.mjs --calib <dir> --big <dir> --texture wall.tga [--out file.json]');
  process.exit(2);
}

const smallLamp = (x, z, props) => ({ x, z, props });
export const SETS = {
  calib: {
    box: { len: 2000, halfWidth: 400, height: 800, gap: 3400 },
    lamps: [
      smallLamp(100, 64, { LightBrightness: 50, LightHue: 32, LightSaturation: 128 }),
      smallLamp(100, 400, { LightBrightness: 96, LightHue: 32, LightSaturation: 128, LightRadius: 18 }),
      smallLamp(100, 100, { LightBrightness: 96, LightHue: 170, LightSaturation: 0, LightRadius: 12 }),
      smallLamp(100, 400, { LightBrightness: 96, LightHue: 160, LightSaturation: 128, LightRadius: 24 }),
    ],
    cams: (c) => [[1900, c, 400, 180], [1000, c, 400, 180], [700, c, 400, 90], [700, c, 400, -90], [400, c, 200, 180]],
  },
  big: {
    box: { len: 7000, halfWidth: 1500, height: 2400, gap: 9000 },
    lamps: [
      smallLamp(300, 700, { LightBrightness: 96, LightHue: 32, LightSaturation: 128, LightRadius: 200 }),
      smallLamp(300, 700, { LightBrightness: 96, LightHue: 32, LightSaturation: 128, LightRadius: 255 }),
    ],
    cams: (c) => [[6800, c, 1200, 180], [4000, c, 1200, 180], [2000, c, 1200, 180], [1200, c, 700, 180],
      [1500, c, 1200, 90], [3500, c, 1200, 90], [5500, c, 1200, 90], [700, c, 300, 180]],
  },
};
const XBIN = 0.025, CBIN = 0.1, MIN_COUNT = 15;

function readTgaMean(file) {
  const b = fs.readFileSync(file);
  const type = b[2], w = b.readUInt16LE(12), h = b.readUInt16LE(14), bpp = b[16] / 8;
  let p = 18 + b[0];
  const sum = [0, 0, 0];
  let n = 0;
  const add = (o) => { sum[0] += b[o + 2]; sum[1] += b[o + 1]; sum[2] += b[o]; n++; };
  if (type === 2) for (let i = 0; i < w * h; i++, p += bpp) add(p);
  else if (type === 10) {
    while (n < w * h) {
      const c = b[p++], cnt = (c & 127) + 1;
      if (c & 128) { for (let i = 0; i < cnt; i++) add(p); p += bpp; } else for (let i = 0; i < cnt; i++, p += bpp) add(p);
    }
  } else throw new Error(`${file}: TGA type ${type}`);
  return sum.map((s) => s / n / 255);
}

const median = (v) => { const s = Float64Array.from(v).sort(); return s.length ? s[s.length >> 1] : NaN; };
export const lampKeys = (l) => Object.fromEntries(Object.entries(l.props).map(([k, v]) => [`ue1_${k}`, v]));
export const lampChannel = (l) => ((l.props.LightSaturation ?? 255) < 64 && (l.props.LightHue ?? 0) > 150 ? 'blue' : 'mean');

// trace pixel (px, py) of a 4:3 reference shot to the room's wall: { d, cos } or null
export function traceShot(set, lampIndex, cam, px, py, W, H) {
  const { box } = SETS[set], lamp = SETS[set].lamps[lampIndex];
  const [ex, ey, ez, yd] = cam, yaw = yd * Math.PI / 180;
  const f = [Math.cos(yaw), Math.sin(yaw), 0], r = [-Math.sin(yaw), Math.cos(yaw), 0];
  const nx = 2 * (px + 0.5) / W - 1, ny = 1 - 2 * (py + 0.5) / H, ty = H / W;
  const d = [0, 1, 2].map((q) => f[q] + nx * r[q] + ny * ty * (q === 2 ? 1 : 0));
  const c = lampIndex * box.gap, lo = [0, c - box.halfWidth, 0], hi = [box.len, c + box.halfWidth, box.height], o = [ex, ey, ez];
  let best = null;
  for (let ax = 0; ax < 3; ax++) for (const [pl, sg] of [[lo[ax], 1], [hi[ax], -1]]) {
    if (!d[ax]) continue;
    const t = (pl - o[ax]) / d[ax];
    if (t <= 0) continue;
    const p = o.map((v, q) => v + t * d[q]);
    if (p.every((v, q) => v >= lo[q] - 1e-3 && v <= hi[q] + 1e-3) && (!best || t < best.t)) { const n = [0, 0, 0]; n[ax] = sg; best = { t, p, n }; }
  }
  if (!best) return null;
  const L = [lamp.x, c, lamp.z].map((v, q) => v - best.p[q]);
  const dist = Math.hypot(...L);
  return { d: dist, cos: L.reduce((s, v, q) => s + v * best.n[q], 0) / dist };
}

// the black level: the median pixel beyond every lamp's reach
function points(set, dir) {
  const S = SETS[set], pts = [], dark = [];
  const R = (l) => 25 * ((l.props.LightRadius ?? 64) + 1);
  let k = 0;
  S.lamps.forEach((lamp, li) => {
    for (const cam of S.cams(li * S.box.gap)) {
      const img = readPng(path.join(dir, `ref_${String(k++).padStart(3, '0')}.png`));
      const blue = lampChannel(lamp) === 'blue';
      for (let py = 1; py < img.height; py += 2) for (let px = 1; px < img.width; px += 2) {
        const tr = traceShot(set, li, cam, px, py, img.width, img.height);
        if (!tr) continue;
        const o = (py * img.width + px) * 4, x = tr.d / R(lamp);
        const v = blue ? img.data[o + 2] : (img.data[o] + img.data[o + 1] + img.data[o + 2]) / 3;
        if (x > 1.05) { dark.push(v); continue; }
        if (tr.cos <= 0.2 || Math.max(img.data[o], img.data[o + 1], img.data[o + 2]) >= 250) continue;
        pts.push({ set, lamp: li, x, cos: tr.cos, v });
      }
    }
  });
  return { pts, black: median(dark) };
}

const tex = readTgaMean(args.texture);
const calib = points('calib', args.calib), big = points('big', args.big);
const black = +((calib.black + big.black) / 2).toFixed(2);   // the two sets agree within a grey level
const all = [...calib.pts, ...big.pts].map((p) => ({ ...p, ref: p.v - black }));

// the oracle's pixel for a lamp at (x, cos), with the profile constants given
export function predict(set, li, x, cos, texMean, constants = {}) {
  const lamp = SETS[set].lamps[li];
  const saved = { ...UE1 };
  Object.assign(UE1, constants);
  const desc = profile(lampKeys(lamp));
  Object.assign(UE1, saved);
  const L = light(desc, x * desc.radius, cos, { ceiling: 2 });
  const px = L.map((c, i) => 255 * texMean[i] * c);
  return lampChannel(lamp) === 'blue' ? px[2] : (px[0] + px[1] + px[2]) / 3;
}

// the gain and the floor: least squares over a grid (the floor's clamp at 0
// makes it nonlinear)
let best = null;
for (let g = 0.0115; g <= 0.0140; g += 0.00005) {
  for (let q = 0; q <= 0.014; q += 0.0005) {
    let se = 0;
    for (const p of all) se += (predict(p.set, p.lamp, p.x, p.cos, tex, { gain: g, floor: q }) - p.ref) ** 2;
    if (!best || se < best.se) best = { se, g, q };
  }
}
const gain = Number(best.g.toFixed(5)), floor = Number(best.q.toFixed(4));

// cells
const groups = new Map();
for (const p of all) {
  const key = `${p.set}:${p.lamp}:${Math.floor(p.x / XBIN)}:${Math.floor(p.cos / CBIN)}`;
  if (!groups.has(key)) groups.set(key, []);
  groups.get(key).push(p);
}
const cells = [];
for (const [, g] of groups) {
  if (g.length < MIN_COUNT) continue;
  cells.push({ set: g[0].set, lamp: g[0].lamp, x: +median(g.map((p) => p.x)).toFixed(4), cos: +median(g.map((p) => p.cos)).toFixed(4), ref: +median(g.map((p) => p.ref)).toFixed(2), n: g.length });
}
cells.sort((a, b) => (a.set < b.set ? -1 : a.set > b.set ? 1 : 0) || a.lamp - b.lamp || a.x - b.x || a.cos - b.cos);

function stats(get) {
  let se = 0, n = 0, lg = 0, ln = 0;
  for (const c of cells) {
    const d = get(c) - c.ref;
    se += d * d; n++;
    if (c.ref > 8 && c.x < 0.85) { lg += Math.abs(Math.log(Math.max(get(c), 3) / Math.max(c.ref, 3))); ln++; }
  }
  return { rms: +Math.sqrt(se / n).toFixed(2), meanAbsLog: +(lg / ln).toFixed(4), cells: n, litCells: ln };
}
const model = stats((c) => predict(c.set, c.lamp, c.x, c.cos, tex, { gain, floor }));
// the profile fitted in the reference renderer's display space before (line to 0.89 R, a
// ceiling at 115 level / brightness of the peak, gain 0.02778)
export const CURVE_SPACE = { gain: 0.02778, floor: 0, falloff: 'line', lineZero: 0.89, capK: 115 };
const curveSpace = stats((c) => predict(c.set, c.lamp, c.x, c.cos, tex, CURVE_SPACE));

const fixture = {
  about: 'UE1 lamp calibration points in linear space: medians of reference pixels (ref, minus the black level) per set, lamp, x = d / WorldLightRadius and N.L, shot by the UE1 reference renderer with its display chain neutral (Brightness 0.5, GammaOffset 0). Sets: four calibration corridors, and two big halls (LightRadius 200 and 255). Built by misc/tools/ue1-light-calib.mjs; no reference art is kept.',
  sets: Object.fromEntries(Object.entries(SETS).map(([k, s]) => [k, { box: s.box, lamps: s.lamps, cams: s.lamps.flatMap((_, i) => s.cams(i * s.box.gap)) }])),
  textureMean: tex.map((v) => +v.toFixed(4)),
  blackLevel: black,
  bins: { x: XBIN, cos: CBIN, minCount: MIN_COUNT },
  fit: { gain, floor, falloff: '1 - smoothstep(d / R), less the floor per lamp', model, curveSpace: { constants: CURVE_SPACE, ...curveSpace } },
  cells,
};
if (args.out) fs.writeFileSync(args.out, JSON.stringify(fixture, null, 1) + '\n');
console.log(`texture mean ${tex.map((v) => v.toFixed(3)).join(' ')}; black ${calib.black} / ${big.black}; ${all.length} points, ${cells.length} cells`);
console.log(`gain ${gain}, floor ${floor} (engine UE1.gain ${UE1.gain}, floor ${UE1.floor})`);
console.log(`smoothstep profile vs reference: rms ${model.rms} grey, mean |log| ${model.meanAbsLog} over ${model.litCells} lit cells`);
console.log(`curve-space profile vs reference: rms ${curveSpace.rms} grey, mean |log| ${curveSpace.meanAbsLog}`);
