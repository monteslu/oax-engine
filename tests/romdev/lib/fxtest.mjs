// fxtest.mjs: shared checks of the phase 6 effect tests (fx-*.mjs): cart
// goldens, native-vs-cart comparison with a control, the oax_fx map's
// shared setup.

import fs from 'node:fs';
import path from 'node:path';
import { comparePng, readPng, halfSize, writePng, distinctColors } from './png.mjs';
import { diffFraction } from './imgstat.mjs';

export const MAP = 'oax_fx';
export const SETUP = ['r_autoExposure 0', 'r_fixedShaderTime 5'];
export const TOLERANCE = 24;

const num = (v) => Number(v ?? NaN);
export { num };

// a half-size cart golden in goldens/oax/<name>.png, created when missing
// or with --update; fails over 0.5% of pixels past TOLERANCE
export function golden(ctx, name, img) {
  const { goldens, out, update, rows, failures } = ctx;
  const gdir = path.join(goldens, 'oax');
  fs.mkdirSync(gdir, { recursive: true });
  const file = path.join(gdir, `${name}.png`);
  const half = halfSize(img);
  if (update || !fs.existsSync(file)) {
    writePng(file, half);
    rows.push(`cart golden ${name} ${update ? 'updated' : 'created'}`);
    return;
  }
  const r = comparePng(readPng(file), half, { tolerance: TOLERANCE, diffPath: path.join(out, `${name}.diff.png`) });
  rows.push(`cart golden ${name}: ${(r.badFraction * 100).toFixed(3)}% over tolerance`);
  if (!r.sameSize || r.badFraction > 0.005) failures.push(`cart golden ${name}: ${(r.badFraction * 100).toFixed(2)}% differ`);
}

// the golden must reject a frame without the feature
export function goldenControl(ctx, name, img, what) {
  const file = path.join(ctx.goldens, 'oax', `${name}.png`);
  const r = comparePng(readPng(file), halfSize(img), { tolerance: TOLERANCE });
  ctx.rows.push(`cart control: ${what} against golden ${name} ${(r.badFraction * 100).toFixed(2)}% differ`);
  if (r.badFraction <= 0.005) ctx.failures.push(`control did not fail: golden ${name} matches ${what}`);
}

// native and cart render the same frame (diffFraction at TOLERANCE under
// `limit`), and the comparison can fail (native against the cart's control
// frame is over `limit`)
export function nativeMatchesCart(ctx, what, native, cart, cartControl, { limit = 0.005, box } = {}) {
  const d = diffFraction(native, cart, TOLERANCE, box);
  const c = diffFraction(native, cartControl, TOLERANCE, box);
  ctx.rows.push(`native vs cart ${what}: ${(d * 100).toFixed(3)}% differ (control ${(c * 100).toFixed(2)}%)`);
  if (d > limit) ctx.failures.push(`native ${what} differs from the cart's (${(d * 100).toFixed(2)}%)`);
  if (c <= limit) ctx.failures.push(`native-vs-cart control did not fail for ${what}`);
}

// a frame that is a picture (not black or one colour)
export function isPicture(ctx, what, img) {
  const n = distinctColors(img);
  if (n < 500) ctx.failures.push(`${what}: only ${n} colours, not a rendered view`);
}
