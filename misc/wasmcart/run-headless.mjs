#!/usr/bin/env node
// run-headless.mjs: run a cart for N frames under plain node, printing its
// log. For debugging boots outside romdev, where a hung cart would block the
// shared server. Pass WASMCART_DIR (a wasmcart checkout) or have `wasmcart`
// resolvable.
//
//   node misc/wasmcart/run-headless.mjs build-cart/cart 120 [shot.png]
import path from 'node:path';
import fs from 'node:fs';

const [cartDir, framesArg] = process.argv.slice(2);
const frames = Number(framesArg || 60);
const wasmcartDir = process.env.WASMCART_DIR;
const mod = wasmcartDir
  ? await import(path.join(path.resolve(wasmcartDir), 'src', 'CartHost.js'))
  : await import('wasmcart');
const { CartHost } = mod;

const host = new CartHost();
const origWrite = process.stderr.write.bind(process.stderr);
await host.load(path.resolve(cartDir), { deterministic: { seed: 1234 } });
host.setFixedStep?.(1000 / 60);
const t0 = Date.now();
for (let i = 0; i < frames; i++) {
  const ft = Date.now();
  host.runFrame([]);
  const dt = Date.now() - ft;
  if (dt > 500) origWrite(`[harness] frame ${i} took ${dt} ms\n`);
}
origWrite(`[harness] ${frames} frames in ${Date.now() - t0} ms\n`);
process.exit(0);
