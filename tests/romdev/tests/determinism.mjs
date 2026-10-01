// Determinism: the same seed and input script must give the same movement
// trace and the same final frame, run after run. Everything else (goldens,
// movement traces, native-vs-wasm) depends on this holding.

import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { SCRIPTS, runScript, compareTraces } from '../lib/movement.mjs';
import { comparePng, readPng } from '../lib/png.mjs';

export const name = 'determinism';

export async function run({ out }) {
  const failures = [];
  const runs = [];
  for (let i = 0; i < 2; i++) {
    const s = new Session(`det-${i}`);
    try {
      const t = await runScript(s, 'oa_dm1', SCRIPTS.basic, { seed: 7 });
      const shot = path.join(out, `determinism_${i}.png`);
      await s.screenshot(shot);
      runs.push({ trace: t.rows, shot });
    } finally {
      await s.shutdown();
    }
  }
  const tr = compareTraces(runs[0].trace, runs[1].trace);
  if (!tr.equal) failures.push(`traces differ between identical runs: ${tr.reason}`);
  const px = comparePng(readPng(runs[0].shot), readPng(runs[1].shot), { tolerance: 0 });
  if (px.badPixels !== 0) failures.push(`final frames differ in ${px.badPixels} pixels (max ${px.maxDiff})`);

  // Control: a different input must give a different trace, or the
  // comparison above could not fail.
  const s = new Session('det-control');
  try {
    const script = SCRIPTS.basic.map((st, k) => (k === 1 ? { ...st, pad: { axes: { ly: -1, lx: -1 } } } : st));
    const t = await runScript(s, 'oa_dm1', script, { seed: 7 });
    if (compareTraces(runs[0].trace, t.rows).equal) failures.push('control: a different script produced the same trace');
  } finally {
    await s.shutdown();
  }

  return {
    ok: failures.length === 0,
    failures,
    rows: [`trace: ${tr.equal ? 'identical' : tr.reason}`, `final frame: ${px.badPixels} differing pixels`],
  };
}
