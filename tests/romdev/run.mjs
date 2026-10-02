#!/usr/bin/env node
// run.mjs: the romdev gate. Runs every test in tests/ against the cart
// through a running romdev server.
//
//   node tests/romdev/run.mjs [--update] [test-name ...]
//
// Exit 0 = all pass, 1 = a test failed, 3 = infrastructure (romdev down or a
// call timed out), so CI can tell a regression from a dead server.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { RomdevUnavailable } from './lib/romdev.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const update = args.includes('--update');
const only = args.filter((a) => !a.startsWith('--'));

const goldens = path.join(here, 'goldens');
const out = path.resolve(process.env.OA_TEST_OUT || path.join(here, '..', '..', 'build-cart', 'test-out'));
fs.mkdirSync(out, { recursive: true });

const files = fs.readdirSync(path.join(here, 'tests')).filter((f) => f.endsWith('.mjs')).sort();
let failed = 0, infra = 0;
for (const f of files) {
  const t = await import(path.join(here, 'tests', f));
  if (only.length && !only.includes(t.name)) continue;
  // tests that need content from outside this repo (locally built maps) run
  // only when named or with OA_EXTERNAL=1
  if (t.slow && !only.length && !process.env.OA_SLOW) {
    console.log(`SKIP ${t.name} (slow; name it or set OA_SLOW=1)`);
    continue;
  }
  if (t.external && !only.length && !process.env.OA_EXTERNAL) {
    console.log(`SKIP ${t.name} (external content; name it or set OA_EXTERNAL=1)`);
    continue;
  }
  const t0 = Date.now();
  try {
    const r = await t.run({ goldens, out, update });
    const secs = ((Date.now() - t0) / 1000).toFixed(1);
    console.log(`${r.ok ? 'PASS' : 'FAIL'} ${t.name} (${secs}s)`);
    for (const row of r.rows || []) console.log(`     ${row}`);
    for (const fl of r.failures || []) console.log(`  !! ${fl}`);
    if (!r.ok) failed++;
  } catch (e) {
    if (e instanceof RomdevUnavailable || e.name === 'TimeoutError') {
      console.log(`INFRA ${t.name}: ${e.message}`);
      infra++;
    } else {
      console.log(`FAIL ${t.name}: ${e.stack || e.message}`);
      failed++;
    }
  }
}
console.log(`artifacts: ${out}`);
process.exit(infra ? 3 : failed ? 1 : 0);
