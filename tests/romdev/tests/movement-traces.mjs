// Movement traces: recorded input, per-frame origin/velocity/ground asserted
// against an approved trace. Guards Q3 movement feel across engine changes.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { SCRIPTS, runScript, compareTraces, summarize } from '../lib/movement.mjs';

export const name = 'movement-traces';

const CASES = [
  { map: 'oa_dm1', script: 'basic' },
  { map: 'oa_dm1', script: 'strafejump', spawn: 1 },
];

// Physical sanity, independent of any golden: the scripts move, run at Q3's
// speed and leave the ground. A golden recorded from a broken build would
// otherwise pass forever.
function sanity(sum, script) {
  const problems = [];
  if (sum.distance < 300) problems.push(`moved only ${sum.distance.toFixed(1)} units`);
  if (sum.topSpeed < 300 || sum.topSpeed > 900) problems.push(`top speed ${sum.topSpeed.toFixed(1)} is not Q3 running speed`);
  if (sum.airborneFrames < 10) problems.push(`airborne only ${sum.airborneFrames} frames; the jump never happened`);
  if (script === 'basic' && sum.rise < 20) problems.push(`rose only ${sum.rise.toFixed(1)} units on the jump`);
  return problems;
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  for (const c of CASES) {
    const s = new Session(`move-${c.script}`);
    try {
      const t = await runScript(s, c.map, SCRIPTS[c.script], { spawn: c.spawn || 0 });
      const sum = summarize(t.rows);
      const label = `${c.map}/${c.script}`;
      fs.writeFileSync(path.join(out, `${c.map}_${c.script}.trace.json`), JSON.stringify(t, null, 0));
      for (const p of sanity(sum, c.script)) failures.push(`${label}: ${p}`);
      const golden = path.join(goldens, 'movement', `${c.map}_${c.script}.json`);
      if (update || !fs.existsSync(golden)) {
        fs.mkdirSync(path.dirname(golden), { recursive: true });
        fs.writeFileSync(golden, JSON.stringify(t) + '\n');
        rows.push(`${label}: golden ${update ? 'updated' : 'created'} ${JSON.stringify(sum)}`);
        continue;
      }
      const g = JSON.parse(fs.readFileSync(golden, 'utf8'));
      const r = compareTraces(g.rows, t.rows);
      rows.push(`${label}: ${r.equal ? 'identical' : r.reason} ${JSON.stringify(sum)}`);
      if (!r.equal) failures.push(`${label}: trace differs from golden (${r.reason})`);
    } finally {
      await s.shutdown();
    }
  }
  return { ok: failures.length === 0, failures, rows };
}
