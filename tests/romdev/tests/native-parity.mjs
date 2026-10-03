// Native-versus-wasm movement parity (the phase 3 exit): the native client
// and the cart play the same pad script from the same spawn, and their
// server-side player states must be identical at every command time both
// recorded, from the moment the player starts moving.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { runScript, PADSCRIPT_MIN_START } from '../lib/movement.mjs';
import { runNative } from '../lib/native.mjs';
import { spawns } from '../lib/scenes.mjs';

export const name = 'native-parity';

const CASES = [
  { map: 'oa_dm1', script: 'basic', spawn: 0 },
  { map: 'oa_dm1', script: 'strafejump', spawn: 1 },
];

// rows as [cmdTime, x, y, z, vx, vy, vz, ground]
// floats were printed with 9 significant digits, which round-trips a float32
// The script's start command time is recorded as a "# start <t>" line.
function nativeRows(file) {
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  const start = lines.find((l) => l.startsWith('# start '));
  return {
    start: start ? Number(start.split(' ')[2]) : null,
    rows: lines.filter((l) => !l.startsWith('#')).map((l) => l.split(' ').map((v, k) => (k >= 1 && k <= 6 ? Math.fround(Number(v)) : Number(v)))),
  };
}

// rows from both builds in common, and the first that differs
function compare(n, w) {
  let common = 0, firstDiff = null, maxErr = 0;
  for (const [dt, a] of n) {
    const b = w.get(dt);
    if (!b) continue;
    common++;
    for (let k = 1; k <= 7; k++) {
      const e = Math.abs(a[k] - b[k]);
      if (e > maxErr) maxErr = e;
      if (e > 0 && !firstDiff) firstDiff = `t+${dt}ms col ${k}: native ${a[k]} vs cart ${b[k]}`;
    }
  }
  return { common, firstDiff, maxErr };
}

function cartRows(trace) {
  return trace.map((r) => [r[8], ...[r[1], r[2], r[3], r[4], r[5], r[6]].map(Math.fround), r[7]]);
}

// Rows from the script start on, keyed by command time relative to it.
function fromStart(rows, t0) {
  if (t0 == null || t0 < 0 || !rows.some((r) => Math.hypot(r[4], r[5]) > 0.5)) return null;
  const out = new Map();
  for (const r of rows) if (r[0] >= t0 && !out.has(r[0] - t0)) out.set(r[0] - t0, r);
  return out;
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  let nativeBasic = null;
  for (const c of CASES) {
    const label = `${c.map}/${c.script}`;
    const p = spawns(c.map)[c.spawn];

    const home = runNative(`parity-${c.script}`, c.map, [
      'exec padscripts/padbinds.cfg', 'bot_enable 0', 'wait 120',
      `setviewpos ${p.x} ${p.y} ${p.z} ${p.yaw}`, 'wait 200',
      `trace parity_${c.script}.txt`,
      // nothing may follow in the command buffer (see cl_testscript.c)
      `set padscript_minstart ${PADSCRIPT_MIN_START}`,
      `padscript padscripts/${c.script}.pad "wait 8; trace stop; quit"`,
    ], { quit: false });
    const nfile = path.join(home, 'baseoa', `parity_${c.script}.txt`);
    if (!fs.existsSync(nfile)) { failures.push(`${label}: native wrote no trace`); continue; }
    const nr = nativeRows(nfile);
    const n = fromStart(nr.rows, nr.start);

    const s = new Session(`parity-${c.script}`);
    let w, t;
    try {
      t = await runScript(s, c.map, c.script, { spawn: c.spawn });
      w = fromStart(cartRows(t.rows), t.padStart);
      fs.writeFileSync(path.join(out, `parity_${c.script}.cart.json`), JSON.stringify([...w.entries()]));
    } finally {
      await s.shutdown();
    }
    if (!n || !w) { failures.push(`${label}: no movement in ${!n ? 'native' : 'cart'} trace`); continue; }

    const { common, firstDiff, maxErr } = compare(n, w);
    if (c.script === 'basic') nativeBasic = n;
    rows.push(`${label}: start native ${nr.start} cart ${t.padStart}`);
    rows.push(`${label}: ${common} common command times (native ${n.size}, cart ${w.size}), max difference ${maxErr}`);
    if (common < n.size) failures.push(`${label}: only ${common} of native's ${n.size} command times recorded by the cart`);
    if (common < 100) failures.push(`${label}: only ${common} command times in common`);
    if (firstDiff) failures.push(`${label}: first difference at ${firstDiff}`);
  }

  // Control: a cart run of a different script must not match native's
  // basic run, or the comparison above could not fail.
  if (nativeBasic) {
    const s = new Session('parity-control');
    try {
      const t = await runScript(s, 'oa_dm1', 'basic_control', { spawn: 0 });
      const r = compare(nativeBasic, fromStart(cartRows(t.rows), t.padStart) || new Map());
      rows.push(`control: ${r.firstDiff ? 'differs at ' + r.firstDiff : 'IDENTICAL'}`);
      if (!r.firstDiff) failures.push('control: a different script matched native');
    } finally {
      await s.shutdown();
    }
  }
  return { ok: failures.length === 0, failures, rows };
}
