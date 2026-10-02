// Deterministic math: the QVM math traps (sin, cos, atan2, acos and the
// engine's AngleVectors) run on our musl-derived detmath, so the native and
// wasm builds must hash 1.2M results to the same value. Also reports whether
// the native host libm alone would have differed (the reason detmath exists).

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues } from '../lib/values.mjs';

export const name = 'detmath';

export async function run() {
  const failures = [];
  const rows = [];

  const home = runNative('detmath', 'oa_dm1', [
    'detmathhash', 'detmathhash host', 'detmathhash perturb', 'debugvalues detmath.txt',
  ]);
  const nfile = path.join(home, 'baseoa', 'detmath.txt');
  if (!fs.existsSync(nfile)) return { ok: false, failures: ['native wrote no debug values'], rows };
  const native = parseDebugValues(fs.readFileSync(nfile, 'utf8'));

  const s = new Session('detmath');
  let cart;
  try {
    await s.load();
    // one write: console_cmd holds a single line until the next frame takes it
    await s.command('detmathhash; detmathhash host; detmathhash perturb');
    await s.step(2);
    cart = parseDebugValues(await s.read('debug_values'));
  } finally {
    await s.shutdown();
  }

  rows.push(`detmath: native ${native.detmath_hash} cart ${cart.detmath_hash}`);
  rows.push(`host libm: native ${native.detmath_hash_host} cart ${cart.detmath_hash_host}${native.detmath_hash_host === cart.detmath_hash_host ? '' : ' (differ: why detmath exists)'}`);
  rows.push(`control (perturbed inputs): ${cart.detmath_hash_perturbed}`);
  if (!native.detmath_hash || !cart.detmath_hash) failures.push('a build published no detmath_hash');
  else if (native.detmath_hash !== cart.detmath_hash) failures.push(`detmath differs: native ${native.detmath_hash} vs cart ${cart.detmath_hash}`);
  // a wasmcart's libc is musl, which detmath copies: on the cart they agree
  if (cart.detmath_hash_host !== cart.detmath_hash) failures.push(`cart: detmath ${cart.detmath_hash} is not the cart's own libm ${cart.detmath_hash_host}`);
  if (cart.detmath_hash_perturbed === cart.detmath_hash) failures.push('control: perturbed inputs gave the same hash');
  return { ok: failures.length === 0, failures, rows };
}
