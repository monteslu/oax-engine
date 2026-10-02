// Unified lighting against an analytic oracle (DESIGN 3.8 check 2).
//
// oax_ulight_flat: one point light (0 0 200, radius 800) over a white
// floor, no ambient, no specular, a 40x40x64 box. The camera looks straight
// down from 600 units with tone mapping and exposure off, so a pixel is
// 255 * N.L * projection * falloff of the light's own images. A JS oracle
// rebuilds those images (_pointlight, _quadratic) and the bilinear lookups.
//   1. radial profile along the center row vs the oracle;
//   2. the box's shadow edge on the floor vs the projected silhouette, within
//      2 px.
// Controls: the oracle without its falloff term must not match; with
// r_ulightShadows 0 the edge must not be found. Checked on the cart and the
// native build.

import fs from 'node:fs';
import path from 'node:path';
import { shootCart, shootNative, readPng } from '../lib/ulight.mjs';
import { LIGHT, BOX } from '../../maps/src/oax_ulight_flat.mjs';

export const name = 'ulight-analytic';

const W = 1280, H = 720, CAM_Z = 600;
const TAN_X = 1, TAN_Y = 720 / 1280;     // cg_fov 90 at 16:9
export const VIEW = `0 0 ${CAM_Z} 90 0 0`;
export const LINEAR = 'r_toneMap 0;r_autoExposure 0;r_cameraExposure 0;r_gamma 1';

// --- the oracle: the engine's generated light images and GL's bilinear lookup

function pointLightImage() {
  const img = new Float64Array(64 * 64);
  for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) {
    const u = (x + 0.5) / 64 * 2 - 1, v = (y + 0.5) / 64 * 2 - 1;
    const r = Math.sqrt(u * u + v * v);
    const d = r < 1 ? (1 - r) * (1 - r) : 0;
    img[y * 64 + x] = Math.trunc(d * 255 + 0.5) / 255;
  }
  return { w: 64, h: 64, img };
}

function quadraticImage() {
  const img = new Float64Array(32 * 4);
  for (let x = 0; x < 32; x++) for (let y = 0; y < 4; y++) {
    let d = Math.abs(x - (32 / 2 - 0.5)) - 0.5;
    d = 1 - d / (32 / 2);
    d *= d;
    img[y * 32 + x] = Math.min(255, Math.max(0, Math.trunc(d * 255))) / 255;
  }
  return { w: 32, h: 4, img };
}

function bilinear({ w, h, img }, s, t) {
  const u = s * w - 0.5, v = t * h - 0.5;
  const x0 = Math.floor(u), y0 = Math.floor(v), fx = u - x0, fy = v - y0;
  const at = (x, y) => img[Math.min(h - 1, Math.max(0, y)) * w + Math.min(w - 1, Math.max(0, x))];
  return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

const PROJ = pointLightImage(), FALL = quadraticImage();

// light value at a floor point, as interaction_fp computes it
export function oracle(x, y, { falloff = true } = {}) {
  const [lx, ly, lz] = LIGHT.origin, [rx, ry, rz] = LIGHT.radius;
  const s = 0.5 + (x - lx) / (2 * rx), t = 0.5 + (y - ly) / (2 * ry), f = 0.5 + (0 - lz) / (2 * rz);
  if (s < 0 || s > 1 || t < 0 || t > 1 || f < 0 || f > 1) return 0;
  const L = [lx - x, ly - y, lz];
  const nl = Math.max(0, L[2] / Math.hypot(...L));
  return nl * bilinear(PROJ, s, t) * (falloff ? bilinear(FALL, f, 0.5) : 1);
}

// floor point under a pixel (camera straight down: image up is +x, right is -y)
function floorAt(px, py, z = 0) {
  const nx = 2 * (px + 0.5) / W - 1, ny = 1 - 2 * (py + 0.5) / H;
  return [ny * TAN_Y * (CAM_Z - z), -nx * TAN_X * (CAM_Z - z)];
}

function pixel(img, px, py) {
  return img.data[(py * img.width + px) * 4];   // white light on white: red channel
}

export function profile(img, opts) {
  const py = H / 2 - 1;     // floor rows just above the center
  let sum = 0, max = 0, n = 0;
  for (let px = 0; px < W; px += 2) {
    const [x, y] = floorAt(px, py);
    const want = Math.round(255 * Math.min(1, oracle(x, y, opts)));
    const d = Math.abs(pixel(img, px, py) - want);
    sum += d; max = Math.max(max, d); n++;
  }
  return { mean: sum / n, max };
}

// expected shadow edge: the box's far top edge projected from the light
export function expectedEdgeRow() {
  const [, , lz] = LIGHT.origin;
  const xEdge = BOX.maxs[0] * lz / (lz - BOX.maxs[2]);
  return (1 - xEdge / (TAN_Y * CAM_Z)) * (H / 2) - 0.5;
}

// scan the center column from the box outward for the 50% crossing
export function measuredEdgeRow(img) {
  const px = W / 2;
  const startRow = Math.floor((1 - BOX.maxs[0] / (TAN_Y * (CAM_Z - BOX.maxs[2]))) * (H / 2)) - 3;
  let prev = null;
  for (let py = startRow; py > startRow - 120; py--) {
    const [x, y] = floorAt(px, py);
    const ratio = pixel(img, px, py) / Math.max(1, 255 * oracle(x, y));
    if (prev && prev.ratio < 0.5 && ratio >= 0.5) {
      return prev.py - (0.5 - prev.ratio) / (ratio - prev.ratio);
    }
    prev = { py, ratio };
  }
  return null;
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const bsp = path.join(out, '..', '..', 'tests', 'maps', 'out', 'baseoa', 'maps', 'oax_ulight_flat.bsp');
  void bsp;
  const opts = { player: '-800 -800 30 45', pre: LINEAR };

  const cart = await shootCart('oax_ulight_flat', [VIEW], { ...opts, name: 'ulight-analytic-cart', out });
  const cartOff = await shootCart('oax_ulight_flat', [VIEW], { ...opts, pre: `${LINEAR};r_ulightShadows 0`, name: 'ulight-analytic-cart-noshadow', out });
  const native = shootNative('oax_ulight_flat', [VIEW], { ...opts, name: 'ulight-analytic-native', out });

  const expected = expectedEdgeRow();
  for (const [label, shot] of [['cart', cart.files[0]], ['native', native.files[0]]]) {
    const img = readPng(shot);
    const p = profile(img);
    const control = profile(img, { falloff: false });
    rows.push(`${label}: radial profile vs oracle mean ${p.mean.toFixed(2)} max ${p.max} (control without falloff: mean ${control.mean.toFixed(1)} max ${control.max})`);
    if (p.mean > 2 || p.max > 8) failures.push(`${label}: radial profile off the oracle (mean ${p.mean.toFixed(2)}, max ${p.max})`);
    if (control.mean <= 2 && control.max <= 8) failures.push(`${label}: control (oracle without falloff) matched too`);
    const edge = measuredEdgeRow(img);
    rows.push(`${label}: shadow edge row ${edge === null ? 'none' : edge.toFixed(2)}, silhouette ${expected.toFixed(2)}`);
    if (edge === null || Math.abs(edge - expected) > 2) failures.push(`${label}: shadow edge ${edge === null ? 'not found' : edge.toFixed(2)} vs ${expected.toFixed(2)} (> 2 px)`);
  }
  const off = measuredEdgeRow(readPng(cartOff.files[0]));
  rows.push(`control r_ulightShadows 0: edge ${off === null ? 'none (as it must)' : off.toFixed(2)}`);
  if (off !== null && Math.abs(off - expected) <= 2) failures.push('control: an edge was found with shadows off');
  if (!fs.existsSync(cart.files[0])) failures.push('no cart frame');
  return { ok: failures.length === 0, failures, rows };
}
