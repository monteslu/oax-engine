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
import { goldensDir, requireTestdata } from './lib/testdata.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const update = args.includes('--update');
const only = args.filter((a) => !a.startsWith('--'));
// --bail: stop at the first failure. A test that runs past its time limit
// (export timeoutSec, else OA_TEST_TIMEOUT_S, default 900 s) fails instead of
// hanging the run, and romdev going away ends the run at once: every later
// test would only wait for its own timeout.
const bail = args.includes('--bail');
const defaultTimeout = Number(process.env.OA_TEST_TIMEOUT_S || 900);

// goldens live in the oax-engine-testdata repo (lib/testdata.mjs); without it
// the image tests would write new goldens and pass, so stop unless updating
const goldens = goldensDir;
if (!update) {
  try { requireTestdata(); } catch (e) { console.error(e.message); process.exit(2); }
}
const out = path.resolve(process.env.OA_TEST_OUT || path.join(here, '..', '..', 'build-cart', 'test-out'));
fs.mkdirSync(out, { recursive: true });

const files = fs.readdirSync(path.join(here, 'tests')).filter((f) => f.endsWith('.mjs')).sort();
// a misspelled test name must fail, not run nothing and report success
const known = new Set(files.map((f) => f.replace(/\.mjs$/, '')));
const unknown = only.filter((n) => !known.has(n));
if (unknown.length) {
  console.error(`unknown test${unknown.length > 1 ? 's' : ''}: ${unknown.join(', ')} (have: ${[...known].join(', ')})`);
  process.exit(2);
}
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
  const limit = t.timeoutSec || defaultTimeout;
  console.log(`RUN  ${t.name} (limit ${limit}s)`);
  let timer;
  try {
    const r = await Promise.race([
      t.run({ goldens, out, update }),
      new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(`timed out after ${limit}s`)), limit * 1000); }),
    ]);
    clearTimeout(timer);
    const secs = ((Date.now() - t0) / 1000).toFixed(1);
    console.log(`${r.ok ? 'PASS' : 'FAIL'} ${t.name} (${secs}s)`);
    for (const row of r.rows || []) console.log(`     ${row}`);
    for (const fl of r.failures || []) console.log(`  !! ${fl}`);
    if (!r.ok) failed++;
  } catch (e) {
    clearTimeout(timer);
    if (e instanceof RomdevUnavailable) {
      console.log(`INFRA ${t.name}: ${e.message} (stopping: romdev is gone)`);
      infra++;
      break;
    } else if (e.name === 'TimeoutError') {
      console.log(`INFRA ${t.name}: a romdev call timed out: ${e.message}`);
      infra++;
    } else {
      console.log(`FAIL ${t.name}: ${e.stack || e.message}`);
      failed++;
    }
  }
  if (bail && (failed || infra)) {
    console.log('stopping at the first failure (--bail)');
    break;
  }
}
console.log(`artifacts: ${out}`);
process.exit(infra ? 3 : failed ? 1 : 0);
