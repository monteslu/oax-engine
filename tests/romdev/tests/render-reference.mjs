// Renderer reference: the cart's GLES 3.0 frames against the NATIVE desktop
// renderer's frames from the same cameras (tests/romdev/reference/native,
// written by reference/capture-native.mjs). The goldens catch regressions;
// this checks the cart renders what the desktop engine renders.
//
// Reads the cart frames render-goldens just wrote, so run it after
// render-goldens (the runner orders tests by file name).

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { comparePng, readPng, halfSize } from '../lib/png.mjs';

export const name = 'render-reference';

const here = path.dirname(fileURLToPath(import.meta.url));
const refDir = path.join(here, '..', 'reference', 'native');
const TOLERANCE = 32;
const MAX_MEAN = 4;          // mean per-pixel max-channel difference
const MAX_BAD = 0.03;        // fraction of pixels beyond tolerance

export async function run({ goldens, out }) {
  const failures = [];
  const rows = [];
  const manifests = fs.readdirSync(path.join(goldens, 'render')).filter((f) => f.endsWith('.json'));
  for (const m of manifests) {
    const { map, cameras } = JSON.parse(fs.readFileSync(path.join(goldens, 'render', m), 'utf8'));
    for (let i = 0; i < cameras.length; i++) {
      const ref = path.join(refDir, `${map}_${i}.png`);
      const shot = path.join(out, `${map}_${i}.png`);
      if (!fs.existsSync(ref)) { failures.push(`${map}#${i}: no native reference (run reference/capture-native.mjs)`); continue; }
      if (!fs.existsSync(shot)) { failures.push(`${map}#${i}: no cart frame (run render-goldens first)`); continue; }
      const r = comparePng(readPng(ref), halfSize(readPng(shot)), { tolerance: TOLERANCE, diffPath: path.join(out, `${map}_${i}.native.diff.png`) });
      rows.push(`${map}#${i}: mean ${r.meanDiff.toFixed(2)}, ${(r.badFraction * 100).toFixed(2)}% beyond tolerance`);
      if (r.meanDiff > MAX_MEAN || r.badFraction > MAX_BAD) failures.push(`${map}#${i}: differs from native (mean ${r.meanDiff.toFixed(2)}, ${(r.badFraction * 100).toFixed(2)}%)`);
    }
    // Control: the cart's view 0 against native view 1 must fail.
    if (cameras.length >= 2) {
      const r = comparePng(readPng(path.join(refDir, `${map}_1.png`)), halfSize(readPng(path.join(out, `${map}_0.png`))), { tolerance: TOLERANCE });
      if (r.meanDiff <= MAX_MEAN && r.badFraction <= MAX_BAD) failures.push(`${map}: control did not fail`);
      else rows.push(`${map}: control fails as it must (mean ${r.meanDiff.toFixed(1)})`);
    }
  }
  return { ok: failures.length === 0, failures, rows };
}
