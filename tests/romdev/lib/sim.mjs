// sim.mjs: run a pad script from a placed player on the cart and on the
// native client, returning the server player-state trace (with yaw) and the
// named debug values at the end. Rows: [t, x, y, z, vx, vy, vz, ground, yaw]
// with t the command time relative to the script's aligned start.

import fs from 'node:fs';
import path from 'node:path';
import { loadScene, placeAt } from './scenes.mjs';
import { runNative } from './native.mjs';
import { parseDebugValues, readValues } from './values.mjs';
import { SIM_SCRIPTS } from './sim-scripts.mjs';
import { TRACE_ROWS, TRACE_COLS } from './movement.mjs';

export const scriptFrames = (name) => SIM_SCRIPTS[name].reduce((n, st) => n + st.frames, 0);

function relRows(rows, t0) {
  const seen = new Set();
  const out = [];
  for (const r of rows) {
    if (r[0] < t0 || seen.has(r[0])) continue;
    seen.add(r[0]);
    out.push([r[0] - t0, ...r.slice(1)]);
  }
  return out;
}

// p: {x, y, z, yaw}; setup: console commands run after placing (one line)
export async function cartRun(s, map, p, script, { setup = '', after = null, seed = 1 } = {}) {
  await loadScene(s, map, { seed });
  await placeAt(s, p);
  if (setup) { await s.command(setup); await s.step(30); }
  const before = await s.read('trace_count');
  await s.command(`padscript padscripts/${script}.pad`);
  await s.step(scriptFrames(script) + 30);
  if (after) await after(s);
  const count = await s.read('trace_count');
  const flat = await s.read('trace');
  const raw = [];
  for (let i = before; i < count; i++) {
    const at = (i % TRACE_ROWS) * TRACE_COLS;
    const r = flat.slice(at, at + TRACE_COLS);
    raw.push([r[8], ...[r[1], r[2], r[3], r[4], r[5], r[6]].map(Math.fround), r[7], Math.fround(r[9])]);
  }
  const t0 = await s.read('pad_start_time');
  return { start: t0, rows: relRows(raw, t0), values: await readValues(s) };
}

export function nativeRun(name, map, p, script, { setup = [] } = {}) {
  const home = runNative(name, map, [
    'exec padscripts/padbinds.cfg', 'bot_enable 0', 'wait 120',
    `setviewpos ${p.x} ${p.y} ${p.z} ${p.yaw}`, 'wait 200',
    ...setup.flatMap((c) => [c, 'wait 60']),
    `trace ${name}.txt`,
    `padscript padscripts/${script}.pad "wait 60; trace stop; debugvalues ${name}.values; quit"`,
  ], { quit: false });
  const tf = path.join(home, 'baseoa', `${name}.txt`);
  if (!fs.existsSync(tf)) throw new Error(`native ${name}: no trace written (see ${home}/native.log)`);
  const lines = fs.readFileSync(tf, 'utf8').trim().split('\n');
  const st = lines.find((l) => l.startsWith('# start '));
  const t0 = st ? Number(st.split(' ')[2]) : 0;
  const raw = lines.filter((l) => !l.startsWith('#')).map((l) => l.split(' ').map((v, k) => (k >= 1 && k <= 6) || k === 8 ? Math.fround(Number(v)) : Number(v)));
  const vf = path.join(home, 'baseoa', `${name}.values`);
  return { start: t0, rows: relRows(raw, t0), values: fs.existsSync(vf) ? parseDebugValues(fs.readFileSync(vf, 'utf8')) : {}, home };
}

// First difference between two traces over the command times both have.
export function compareRows(a, b) {
  const mb = new Map(b.map((r) => [r[0], r]));
  let common = 0, firstDiff = null;
  for (const r of a) {
    const o = mb.get(r[0]);
    if (!o) continue;
    common++;
    for (let k = 1; k <= 8 && !firstDiff; k++) {
      if (r[k] !== o[k]) firstDiff = `t+${r[0]}ms col ${k}: ${r[k]} vs ${o[k]}`;
    }
  }
  return { common, firstDiff };
}
