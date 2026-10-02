// Unified lighting goldens (DESIGN 3.8 check 1): eight cameras on the
// oax_unified test map, the NATIVE desktop build as the reference
// (tests/romdev/reference/ulight, written with --update), the cart within
// tolerance of it.
//
// Must-fail controls, each measured where it matters:
//   - r_ulightShadows 0 must differ in the shadowed regions (pixels the
//     reference shows darker than the unshadowed frame), where the normal
//     cart frame must match;
//   - r_normalMapping 0 must differ on the bump-mapped wall.
// cg_oaxLightTime pins the moving light; r_fixedShaderTime pins the
// flicker table, so both builds light the same instant.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { shootCart, shootNative, uniqueColors } from '../lib/ulight.mjs';
import { comparePng, readPng, halfSize, writePng } from '../lib/png.mjs';

export const name = 'ulight-goldens';

const here = path.dirname(fileURLToPath(import.meta.url));
const refDir = path.join(here, '..', 'reference', 'ulight');
const MAP = 'oax_unified';
const PIN = 'cg_oaxLightTime 1000';
const TOLERANCE = 32;
const MAX_MEAN = 4;
const MAX_BAD = 0.03;
const MIN_COLORS = 2000;

export const GROUPS = [
  {
    // room A: the player (MD3, third person) among the pillars
    player: '-680 -120 30 90', pre: `${PIN};cg_thirdPerson 1`,
    cams: [
      ['pillar shadows', '-880 -370 280 50 90 0'],
      ['room A', '-450 -300 200 15 160 0'],
      ['projected gobo', '-1000 -50 230 50 -120 0'],
      ['MD3 player', '-680 250 110 10 -90 0'],
    ],
  },
  {
    // room B: the bound (moving) light and the flicker table
    player: '-150 -330 30 45', pre: PIN,
    cams: [
      ['moving light', '-250 -360 220 25 50 0'],
      ['flicker wall', '0 100 150 10 90 0'],
      ['room B west', '200 -300 150 15 160 0'],
    ],
  },
  {
    // room C: point light, crate, spot light
    player: '1250 -300 30 180', pre: PIN,
    cams: [['room C', '550 -330 200 25 30 0']],
  },
];

const cameras = GROUPS.flatMap((g, gi) => g.cams.map(([label, view]) => ({ group: gi, label, view })));

async function shootGroups(shoot, prefix, out) {
  const files = [];
  for (let gi = 0; gi < GROUPS.length; gi++) {
    const g = GROUPS[gi];
    const r = await shoot(MAP, g.cams.map((c) => c[1]), { name: `${prefix}-${gi}`, out, player: g.player, pre: g.pre });
    files.push(...r.files);
  }
  return files;
}

function half(file) {
  return halfSize(readPng(file));
}

// pixels where `dark` is darker than `light` by more than 24 (max channel)
function darkerMask(dark, light) {
  const mask = new Uint8Array(dark.width * dark.height);
  for (let p = 0; p < mask.length; p++) {
    const i = p * 4;
    const d = Math.max(light.data[i] - dark.data[i], light.data[i + 1] - dark.data[i + 1], light.data[i + 2] - dark.data[i + 2]);
    mask[p] = d > 24 ? 1 : 0;
  }
  return mask;
}

function maskedDiff(a, b, mask) {
  let n = 0, over = 0, sum = 0;
  for (let p = 0; p < mask.length; p++) {
    if (!mask[p]) continue;
    const i = p * 4;
    const d = Math.max(Math.abs(a.data[i] - b.data[i]), Math.abs(a.data[i + 1] - b.data[i + 1]), Math.abs(a.data[i + 2] - b.data[i + 2]));
    n++; sum += d;
    if (d > TOLERANCE) over++;
  }
  return { n, mean: n ? sum / n : 0, over: n ? over / n : 0 };
}

// a rectangle (fractions of the frame) as a mask
function rectMask(img, x0, y0, x1, y1) {
  const mask = new Uint8Array(img.width * img.height);
  for (let y = Math.floor(y0 * img.height); y < y1 * img.height; y++) {
    for (let x = Math.floor(x0 * img.width); x < x1 * img.width; x++) mask[y * img.width + x] = 1;
  }
  return mask;
}

export async function run({ out, update }) {
  const failures = [];
  const rows = [];
  fs.mkdirSync(refDir, { recursive: true });

  if (update || !fs.existsSync(path.join(refDir, `${MAP}_0.png`))) {
    const nat = await shootGroups(async (...a) => shootNative(...a), 'ulight-native', out);
    nat.forEach((f, i) => writePng(path.join(refDir, `${MAP}_${i}.png`), half(f)));
    rows.push(`wrote ${nat.length} native references`);
  }

  const cart = await shootGroups(shootCart, 'ulight-cart', out);
  for (let i = 0; i < cameras.length; i++) {
    const ref = readPng(path.join(refDir, `${MAP}_${i}.png`));
    const shot = half(cart[i]);
    const colors = uniqueColors(readPng(cart[i]));
    const r = comparePng(ref, shot, { tolerance: TOLERANCE, diffPath: path.join(out, `ulight-goldens_${i}.diff.png`) });
    rows.push(`#${i} ${cameras[i].label}: mean ${r.meanDiff.toFixed(2)}, ${(r.badFraction * 100).toFixed(2)}% beyond ${TOLERANCE}, ${colors} colours`);
    if (r.meanDiff > MAX_MEAN || r.badFraction > MAX_BAD) failures.push(`#${i} ${cameras[i].label}: cart differs from native (mean ${r.meanDiff.toFixed(2)}, ${(r.badFraction * 100).toFixed(2)}%)`);
    if (colors < MIN_COLORS) failures.push(`#${i} ${cameras[i].label}: only ${colors} colours`);
  }

  // control 1: shadows off, camera 0 (pillar shadows)
  {
    const g = GROUPS[0];
    const off = await shootCart(MAP, [g.cams[0][1]], { name: 'ulight-goldens-noshadow', out, player: g.player, pre: `${g.pre};r_ulightShadows 0` });
    const ref = readPng(path.join(refDir, `${MAP}_0.png`));
    const offImg = half(off.files[0]);
    const mask = darkerMask(ref, offImg);
    const shadowPx = mask.reduce((a, b) => a + b, 0);
    const control = maskedDiff(ref, offImg, mask);
    const normal = maskedDiff(ref, half(cart[0]), mask);
    rows.push(`shadow regions: ${(shadowPx / mask.length * 100).toFixed(1)}% of the frame; cart vs native there mean ${normal.mean.toFixed(1)}, shadows off vs native mean ${control.mean.toFixed(1)}`);
    if (shadowPx < mask.length * 0.02) failures.push('control: the reference shows almost no shadow (nothing to test)');
    if (control.mean < 20) failures.push(`control: r_ulightShadows 0 did not differ in the shadows (mean ${control.mean.toFixed(1)})`);
    if (normal.mean > 12) failures.push(`cart shadows differ from native in the shadow regions (mean ${normal.mean.toFixed(1)})`);
  }

  // control 2: normal mapping off, the bump-mapped wall (camera 5)
  {
    const g = GROUPS[1];
    const flat = await shootCart(MAP, [g.cams[1][1]], { name: 'ulight-goldens-nonormal', out, player: g.player, pre: g.pre, preload: 'r_normalMapping 0' });
    const ref = readPng(path.join(refDir, `${MAP}_5.png`));
    const wall = rectMask(ref, 0.1, 0.05, 0.9, 0.55);
    const control = maskedDiff(ref, half(flat.files[0]), wall);
    const normal = maskedDiff(ref, half(cart[5]), wall);
    rows.push(`bumped wall: cart vs native mean ${normal.mean.toFixed(2)}, r_normalMapping 0 vs native mean ${control.mean.toFixed(2)} (${(control.over * 100).toFixed(1)}% beyond ${TOLERANCE})`);
    if (control.mean <= normal.mean * 2 || control.mean < 3) failures.push(`control: r_normalMapping 0 did not change the bumped wall (mean ${control.mean.toFixed(2)})`);
  }

  return { ok: failures.length === 0, failures, rows };
}
