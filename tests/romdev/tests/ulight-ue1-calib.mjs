// The ue1 light profile against UE1's own lighting, in linear space.
//
// The calibration rooms were baked with the UE1 editor's lighting build and
// shot by the UE1 reference renderer with its display chain neutral
// (Brightness 0.5, GammaOffset 0), so its pixels are texel x light; tests/romdev/reference/ue1-light-calib.json
// keeps the medians per room set, lamp, x = d / WorldLightRadius and N.L
// (misc/tools/ue1-light-calib.mjs): four small-lamp corridors and
// two big halls (LightRadius 200 and 255). oax_ue1_calib rebuilds those rooms
// with ue1_* lamp keys and walls of the texture's mean colour; the same
// cameras render it on the cart and native (r_toneMap 0: light 1 is the
// texture at 1x), each pixel is traced to its wall point, and:
//   1. every pixel equals the JS oracle of the profile (mean <= 1 grey, max <= 6);
//   2. the rendered cells reproduce the reference: mean |log render / ref|
//      over the lit cells (ref > 8 grey, x < 0.85) at most 0.06, on each set.
// Control that must fail: oax_ue1_calib_curve, the profile as fitted to the
// reference display-curve shots before (a line to 0.89 R, a ceiling, gain 0.02778).

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { readPng, writePng, readTga } from '../lib/ulight.mjs';
import { runNative } from '../lib/native.mjs';
import { profile, light } from '../lib/lightoracle.mjs';
import { CALIB, centre } from '../../maps/src/oax_ue1_calib.mjs';

export const name = 'ulight-ue1-calib';
export const timeoutSec = 420;

const W = 1280, H = 720, TAN_X = 1, TAN_Y = 720 / 1280;
const LINEAR = 'r_toneMap 0;r_autoExposure 0;r_cameraExposure 1;r_gamma 1';
const MAX_LOG = 0.06;
const SETS = Object.keys(CALIB.sets);

// every camera, in the map's coordinates: { set, lamp, cam: [x, y, z, yaw] }
const CAMS = SETS.flatMap((set) => {
  const S = CALIB.sets[set], per = S.cams.length / S.lamps.length;
  return S.cams.map((c, k) => {
    const lamp = Math.floor(k / per);
    return { set, lamp, cam: [c[0], centre(set, lamp) + (c[1] - lamp * S.box.gap), c[2], c[3]] };
  });
});
const viewOf = ({ cam: [x, y, z, yaw] }) => `${x} ${y} ${z} 0 ${yaw} 0`;
const playerIn = ({ set, lamp }) => { const b = CALIB.sets[set].box; return `${b.len - 500} ${centre(set, lamp) + b.halfWidth - 100} 30 0`; };
const newRoom = (k) => k === 0 || CAMS[k].set !== CAMS[k - 1].set || CAMS[k].lamp !== CAMS[k - 1].lamp;

async function shootCart(map, out) {
  const s = new Session(`${map}-cart`);
  const files = [];
  try {
    await loadScene(s, map, { seed: 1, view: CLEAN_VIEW });
    for (let k = 0; k < CAMS.length; k++) {
      if (newRoom(k)) {
        // one console line per frame: the view cvars go with the first move
        await s.command(`${k === 0 ? `${LINEAR};` : ''}setviewpos ${playerIn(CAMS[k])}`);
        await s.step(40);
      }
      await s.command(`cl_overrideView "${viewOf(CAMS[k])}"`);
      await s.step(16);
      const file = path.join(out, `${map}-cart_${k}.png`);
      await s.screenshot(file);
      files.push(file);
    }
    return files;
  } finally {
    await s.shutdown();
  }
}

function shootNative(map, out) {
  const lines = [...CLEAN_VIEW.split(';'), ...LINEAR.split(';')];
  CAMS.forEach((c, k) => {
    if (newRoom(k)) lines.push(`setviewpos ${playerIn(c)}`, 'wait 80');
    lines.push(`cl_overrideView "${viewOf(c)}"`, 'wait 30', `screenshot shot_${k}`, 'wait 2');
  });
  const home = runNative(`${map}-native`, map, lines);
  return CAMS.map((_, k) => {
    const tga = path.join(home, 'baseoa', 'screenshots', `shot_${k}.tga`);
    if (!fs.existsSync(tga)) throw new Error(`native client wrote no screenshot ${k} (see ${home}/native.log)`);
    const file = path.join(out, `${map}-native_${k}.png`);
    writePng(file, readTga(tga));
    return file;
  });
}

// trace a pixel of a render to its wall point: { d, cos, edge } or null
function trace({ set, lamp: li, cam }, px, py) {
  const S = CALIB.sets[set], { len, halfWidth, height } = S.box, lamp = S.lamps[li];
  const [ex, ey, ez, yawDeg] = cam, yaw = yawDeg * Math.PI / 180;
  const fwd = [Math.cos(yaw), Math.sin(yaw), 0], right = [Math.sin(yaw), -Math.cos(yaw), 0];
  const nx = 2 * (px + 0.5) / W - 1, ny = 1 - 2 * (py + 0.5) / H;
  const dir = [0, 1, 2].map((k) => fwd[k] + nx * TAN_X * right[k] + ny * TAN_Y * (k === 2 ? 1 : 0));
  const c = centre(set, li);
  const lo = [0, c - halfWidth, 0], hi = [len, c + halfWidth, height], o = [ex, ey, ez];
  let best = null;
  for (let ax = 0; ax < 3; ax++) {
    if (Math.abs(dir[ax]) < 1e-9) continue;
    for (const [plane, sign] of [[lo[ax], 1], [hi[ax], -1]]) {
      const t = (plane - o[ax]) / dir[ax];
      if (t <= 0) continue;
      const p = o.map((v, k) => v + t * dir[k]);
      if (p.every((v, k) => v >= lo[k] - 1e-3 && v <= hi[k] + 1e-3) && (!best || t < best.t)) { const n = [0, 0, 0]; n[ax] = sign; best = { t, p, n }; }
    }
  }
  if (!best) return null;
  const L = [lamp.x, c, lamp.z].map((v, k) => v - best.p[k]);
  const d = Math.hypot(...L);
  const cos = Math.max(0, L.reduce((s, v, k) => s + v * best.n[k], 0) / d);
  const edge = Math.min(...[0, 1, 2].filter((k) => best.n[k] === 0).map((k) => Math.min(best.p[k] - lo[k], hi[k] - best.p[k])));
  return { d, cos, edge, t: best.t };
}

const median = (v) => { const s = Float64Array.from(v).sort(); return s[s.length >> 1]; };
const isBlue = (l) => (l.props.LightSaturation ?? 255) < 64 && (l.props.LightHue ?? 0) > 150;

export function analyse(files) {
  const tex = CALIB.textureMean;
  let sum = 0, max = 0, n = 0;
  const cells = new Map();
  files.forEach((file, k) => {
    const img = readPng(file);
    const c = CAMS[k], lamp = CALIB.sets[c.set].lamps[c.lamp];
    const desc = profile(Object.fromEntries(Object.entries(lamp.props).map(([kk, v]) => [`ue1_${kk}`, v])));
    for (let py = 1; py < H; py += 2) for (let px = 1; px < W; px += 2) {
      const tr = trace(c, px, py);
      if (!tr) continue;
      const o = (py * W + px) * 4;
      const got = [img.data[o], img.data[o + 1], img.data[o + 2]];
      // off the room's edges by a few pixels at that distance (one pixel is t x 2 / W)
      if (tr.edge > 4 + 3 * tr.t * 2 / W && tr.cos > 0.02) {
        const L = light(desc, tr.d, tr.cos, { ceiling: 2 });
        for (let ch = 0; ch < 3; ch++) {
          const e = Math.abs(got[ch] - Math.min(255, Math.round(255 * tex[ch] * L[ch])));
          sum += e; max = Math.max(max, e); n++;
        }
      }
      const x = tr.d / desc.radius;
      if (tr.cos <= 0.2 || x >= 1.2) continue;
      const key = `${c.set}:${c.lamp}:${Math.floor(x / CALIB.bins.x)}:${Math.floor(tr.cos / CALIB.bins.cos)}`;
      if (!cells.has(key)) cells.set(key, []);
      cells.get(key).push(isBlue(lamp) ? got[2] : (got[0] + got[1] + got[2]) / 3);
    }
  });
  const bySet = {};
  for (const set of SETS) {
    let se = 0, cn = 0, lg = 0, ln = 0;
    for (const cell of CALIB.cells.filter((x) => x.set === set)) {
      const v = cells.get(`${set}:${cell.lamp}:${Math.floor(cell.x / CALIB.bins.x)}:${Math.floor(cell.cos / CALIB.bins.cos)}`);
      if (!v || v.length < CALIB.bins.minCount) continue;
      const m = median(v);
      se += (m - cell.ref) ** 2; cn++;
      if (cell.ref > 8 && cell.x < 0.85) { lg += Math.abs(Math.log(Math.max(m, 3) / Math.max(cell.ref, 3))); ln++; }
    }
    bySet[set] = { rms: Math.sqrt(se / cn), meanAbsLog: lg / ln, cells: cn, lit: ln, of: CALIB.cells.filter((x) => x.set === set).length };
  }
  return { oracle: { mean: sum / n, max }, bySet };
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  rows.push(`fixture: ${CALIB.cells.length} reference cells (linear), black ${CALIB.blackLevel}; profile gain ${CALIB.fit.gain}, falloff ${CALIB.fit.falloff}: rms ${CALIB.fit.model.rms}, mean |log| ${CALIB.fit.model.meanAbsLog}; curve-space profile: rms ${CALIB.fit.curveSpace.rms}, mean |log| ${CALIB.fit.curveSpace.meanAbsLog}`);
  for (const [map, isControl] of [['oax_ue1_calib', false], ['oax_ue1_calib_curve', true]]) {
    for (const [label, files] of [['cart', await shootCart(map, out)], ['native', shootNative(map, out)]]) {
      const r = analyse(files);
      const sets = SETS.map((s) => `${s} mean |log| ${r.bySet[s].meanAbsLog.toFixed(4)} rms ${r.bySet[s].rms.toFixed(2)} (${r.bySet[s].cells}/${r.bySet[s].of} cells)`).join('; ');
      rows.push(`${map} ${label}: pixels vs ue1 oracle mean ${r.oracle.mean.toFixed(2)} max ${r.oracle.max}; vs reference: ${sets}`);
      const ok = SETS.every((s) => r.bySet[s].meanAbsLog <= MAX_LOG);
      if (!isControl) {
        if (r.oracle.mean > 1 || r.oracle.max > 6) failures.push(`${map} ${label}: render off the oracle (mean ${r.oracle.mean.toFixed(2)}, max ${r.oracle.max})`);
        for (const s of SETS) {
          if (r.bySet[s].meanAbsLog > MAX_LOG) failures.push(`${map} ${label}: ${s} mean |log| vs reference ${r.bySet[s].meanAbsLog.toFixed(4)} > ${MAX_LOG}`);
          if (r.bySet[s].cells < r.bySet[s].of * 0.5) failures.push(`${map} ${label}: ${s}: only ${r.bySet[s].cells} of ${r.bySet[s].of} cells covered`);
        }
      } else if (ok) {
        failures.push(`${map} ${label}: the control (the curve-space profile) reproduced the reference linear lighting too`);
      }
    }
  }
  return { ok: failures.length === 0, failures, rows };
}
