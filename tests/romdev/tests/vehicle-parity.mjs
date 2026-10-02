// Vehicle parity (phase 8): a scripted drive gives the same vehicle on the
// native client and on the cart. On oax_vehicle_test (vehicle rule on) the
// player gets into the buggy parked in front of the spawn with the use
// button, drives it over heightmap terrain and up a brush ramp, turns,
// slides with the handbrake, brakes, backs up and gets out, all from one pad
// script (vehicle_drive); a second script (hover_drive) walks to the hover
// craft and flies it over the hills, banking through turns. The server
// simulates both in its Box3D world (code/physics/phys_vehicle.c: raycast
// wheels with suspension and tire grip; thruster rays); the game publishes
// a hash of the vehicle's state at every server frame from the moment a
// driver got in (times relative to that moment), and the state once a
// second. Both must be identical native vs cart, and so must the sequence
// of seat states the rider's own trace shows.
//
// Also asserted: the drive goes somewhere (distance, top speed, the ramp
// climbed), the vehicle never sinks into the ground, one enter and one exit.
// Driver commands are applied by command time (g_oaxVehInputDelay), and
// no command may arrive after the frame that uses it on either build.
// Controls: another script on the cart must give another hash, and with the
// vehicle rule off (g_oaxVehicles 0) the map has no vehicles at all.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, mapPath, CLEAN_VIEW } from '../lib/scenes.mjs';
import { runNative } from '../lib/native.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';
import { TRACE_ROWS, TRACE_COLS } from '../lib/movement.mjs';
import { terrainFromBsp } from '../lib/terrain.mjs';

export const name = 'vehicle-parity';

const MAP = 'oax_vehicle_test';
const RULE = 'set g_oaxVehicles 1';

function scriptFrames(script) {
  const text = fs.readFileSync(path.join(path.dirname(new URL(import.meta.url).pathname), '..', 'data', 'padscripts', `${script}.pad`), 'utf8');
  return text.split('\n').filter((l) => l.trim() && !l.startsWith('#')).reduce((n, l) => n + Number(l.split(/\s+/)[0]), 0);
}

function nativeRows(file) {
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  const start = lines.find((l) => l.startsWith('# start '));
  return {
    start: start ? Number(start.split(' ')[2]) : null,
    rows: lines.filter((l) => !l.startsWith('#')).map((l) => l.split(' ').map((v, k) => (k >= 1 && k <= 6 ? Math.fround(Number(v)) : Number(v)))),
  };
}

function fromStart(rows, t0) {
  if (t0 == null || t0 < 0) return null;
  const out = new Map();
  for (const r of rows) if (r[0] >= t0 && !out.has(r[0] - t0)) out.set(r[0] - t0, r);
  return out;
}

// The player's own trace while riding (after the use press at 20 frames,
// before the second one). A rider's origin and velocity are the seat's, set
// by the server after each physics step, so the client sees them change with
// the snapshots, not at command times: the two builds see the same values
// one snapshot apart. Compare the sequence of distinct seat states instead.
function seatSequence(trace, RIDE) {
  const seq = [];
  for (const [dt, r] of [...trace.entries()].sort((a, b) => a[0] - b[0])) {
    if (dt < RIDE[0] || dt > RIDE[1]) continue;
    const key = r.slice(1, 7).join(' ');
    if (seq[seq.length - 1] !== key) seq.push(key);
  }
  return seq;
}

function compareSeats(n, w, ride) {
  const a = seatSequence(n, ride), b = seatSequence(w, ride);
  // align on the first native state the cart also has
  let off = -1, i0 = 0;
  for (; i0 < Math.min(a.length, 5) && off < 0; i0++) off = b.indexOf(a[i0]);
  i0--;
  if (off < 0) return { common: 0, firstDiff: 'no common seat state', maxErr: NaN };
  let common = 0, firstDiff = null;
  for (let i = i0, j = off; i < a.length && j < b.length; i++, j++) {
    if (a[i] !== b[j]) { if (!firstDiff) firstDiff = `seat state ${i}: native ${a[i]} vs cart ${b[j]}`; } else common++;
  }
  return { common, firstDiff, maxErr: 0, native: a.length, cart: b.length };
}

// the drive checkpoints, g_veh_drive_<vehicle>_t<second>
function checkpoints(v) {
  return Object.keys(v).filter((k) => /^g_veh_drive_\d+_t\d+$/.test(k)).sort((a, b) => {
    const [, va, ta] = a.match(/_(\d+)_t(\d+)$/), [, vb, tb] = b.match(/_(\d+)_t(\d+)$/);
    return va - vb || ta - tb;
  });
}

async function cartDrive(s, script, rule = RULE) {
  await loadScene(s, MAP, { view: `${CLEAN_VIEW};${rule}` });
  // the parked vehicles settle on their suspension and fall asleep
  await s.step(200);
  const before = await s.read('trace_count');
  await s.command(`padscript padscripts/${script}.pad`);
  await s.step(scriptFrames(script) + 40);
  const after = await s.read('trace_count');
  const rows = [];
  const flat = await s.read('trace');
  for (let i = before; i < after && after - before <= TRACE_ROWS; i++) {
    const at = (i % TRACE_ROWS) * TRACE_COLS;
    const r = flat.slice(at, at + TRACE_COLS);
    rows.push([r[8], ...[r[1], r[2], r[3], r[4], r[5], r[6]].map(Math.fround), r[7]]);
  }
  return { values: await readValues(s), trace: fromStart(rows, await s.read('pad_start_time')) };
}

// the drives: the buggy straight ahead of the spawn, the hover craft to its left
const CASES = [
  { script: 'vehicle_drive', control: 'vehicle_drive_control', label: 'buggy', ride: [700, 7500], minDist: 2500, minTop: 700, climb: true },
  { script: 'hover_drive', label: 'hover', ride: [1500, 8500], minDist: 2000, minTop: 700, climb: false },
];

async function driveCase(c, terrain, out, failures, rows) {
  const tag = `vparity-${c.label}`;
  // native
  const home = runNative(tag, MAP, [
    'exec padscripts/padbinds.cfg', 'bot_enable 0', 'wait 200',
    `trace ${tag}.txt`,
    `padscript padscripts/${c.script}.pad "wait 8; trace stop; debugvalues ${tag}_values.txt; quit"`,
  ], { quit: false, set: { g_oaxVehicles: 1 } });
  const vfile = path.join(home, 'baseoa', `${tag}_values.txt`);
  const tfile = path.join(home, 'baseoa', `${tag}.txt`);
  if (!fs.existsSync(vfile) || !fs.existsSync(tfile)) { failures.push(`${c.label}: native wrote no values or trace`); return null; }
  const nv = parseDebugValues(fs.readFileSync(vfile, 'utf8'));
  const nt = nativeRows(tfile);
  const ntrace = fromStart(nt.rows, nt.start);

  // cart
  const s = new Session(tag);
  let cart;
  try {
    cart = await cartDrive(s, c.script);
    await s.screenshot(path.join(out, `${tag}-end.png`));
  } finally {
    await s.shutdown();
  }
  const cv = cart.values;
  fs.writeFileSync(path.join(out, `${tag}.json`), JSON.stringify({ native: nv, cart: cv }, null, 1));

  // identity
  const keys = checkpoints(nv);
  let same = 0;
  for (const k of keys) {
    if (nv[k] === cv[k]) same++;
    else if (!failures.some((f) => f.startsWith(`${c.label}: checkpoint`))) failures.push(`${c.label}: checkpoint ${k}: native "${nv[k]}" vs cart "${cv[k] ?? 'missing'}"`);
  }
  // the running drive hash at the last checkpoint both builds reached
  const lastCommon = keys.filter((k) => cv[k] !== undefined).pop();
  const hashOf = (v) => (v && lastCommon && v[lastCommon] ? v[lastCommon].split(' ').pop() : null);
  rows.push(`${c.label}: drive hash at ${lastCommon}: native ${hashOf(nv)} cart ${hashOf(cv)}; ${same}/${keys.length} once-a-second checkpoints identical`);
  if (hashOf(nv) !== hashOf(cv)) failures.push(`${c.label}: drive hash differs at ${lastCommon}: native ${hashOf(nv)}, cart ${hashOf(cv)}`);
  if (keys.length < 6) failures.push(`${c.label}: only ${keys.length} drive checkpoints`);
  const tr = ntrace && cart.trace ? compareSeats(ntrace, cart.trace, c.ride) : { common: 0, firstDiff: 'no trace' };
  rows.push(`${c.label}: player trace riding the seat (t+${c.ride[0]}..${c.ride[1]} ms): ${tr.common} identical seat states in sequence (native ${tr.native}, cart ${tr.cart} distinct)`);
  if (tr.firstDiff) failures.push(`${c.label}: player trace: ${tr.firstDiff}`);
  if (tr.common < 100) failures.push(`${c.label}: player trace: only ${tr.common} seat states in common`);

  // the drive itself
  const cps = keys.map((k) => cv[k]).filter(Boolean).map((x) => x.split(' ').map(Number));
  const top = Math.max(...cps.map((p) => Math.abs(p[3])));
  const zmax = Math.max(...cps.map((p) => p[2]));
  let minClear = Infinity;
  for (const p of cps) {
    const g = terrain.heightAt(p[0], p[1]);
    if (g !== null) minClear = Math.min(minClear, p[2] - g);
  }
  rows.push(`${c.label}: ${Number(cv.g_veh_distance).toFixed(0)} units with a driver, top checkpoint speed ${top.toFixed(0)} u/s, highest center z ${zmax.toFixed(1)}, ` +
    `lowest center over the terrain ${minClear.toFixed(1)}; enters ${cv.g_veh_enters}, exits ${cv.g_veh_exits}, frames in solid ${cv.g_veh_in_solid}`);
  rows.push(`${c.label}: physics ${cv.g_veh_phys_ticks} ticks, ${cv.g_veh_phys_bodies} bodies (terrain bodies ${cv.g_veh_terrain_bodies}), step ${cv.g_veh_phys_ms} ms (native ${nv.g_veh_phys_ms} ms); late driver commands ${cv.g_veh_late_cmds}/${nv.g_veh_late_cmds}`);
  if (!(Number(cv.g_veh_distance) > c.minDist)) failures.push(`${c.label}: driven only ${cv.g_veh_distance} units`);
  if (!(top > c.minTop)) failures.push(`${c.label}: top speed ${top} u/s`);
  if (c.climb && !(zmax > 100)) failures.push(`${c.label}: the drive never climbed the ramp`);
  if (!(minClear > 0)) failures.push(`${c.label}: the vehicle center went ${minClear} below the terrain`);
  if (Number(cv.g_veh_enters) !== 1 || Number(cv.g_veh_exits) !== 1) failures.push(`${c.label}: enters ${cv.g_veh_enters}, exits ${cv.g_veh_exits} (want 1 and 1)`);
  if (Number(cv.g_veh_in_solid) !== 0) failures.push(`${c.label}: ${cv.g_veh_in_solid} vehicle frames in solid`);
  if (Number(cv.g_veh_late_cmds) !== 0 || Number(nv.g_veh_late_cmds) !== 0) failures.push(`${c.label}: late driver commands (cart ${cv.g_veh_late_cmds}, native ${nv.g_veh_late_cmds})`);
  if (Number(cv.g_veh_terrain_bodies) < 1) failures.push(`${c.label}: no terrain collision in the physics world`);
  return { nv, hashOf, lastCommon };
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const terrain = terrainFromBsp(fs.readFileSync(mapPath(MAP)))[0];
  const results = [];
  for (const c of CASES) results.push(await driveCase(c, terrain, out, failures, rows));

  // controls
  const first = results[0];
  if (first) {
    const c = new Session('vparity-control');
    try {
      const ctl = await cartDrive(c, CASES[0].control);
      rows.push(`control ${CASES[0].control}: drive hash at ${first.lastCommon} ${first.hashOf(ctl.values)} (${first.hashOf(ctl.values) === first.hashOf(first.nv) ? 'SAME' : 'differs'})`);
      if (first.hashOf(ctl.values) === first.hashOf(first.nv)) failures.push('control: another script gave the same drive hash');
    } finally {
      await c.shutdown();
    }
  }
  const off = new Session('vparity-off');
  try {
    const o = await cartDrive(off, CASES[0].script, 'set g_oaxVehicles 0');
    rows.push(`control g_oaxVehicles 0: spawners ${o.values.g_veh_spawners ?? 'none'}, enters ${o.values.g_veh_enters ?? 'none'}`);
    if (o.values.g_veh_spawners !== undefined || o.values.g_veh_enters !== undefined) failures.push('control: vehicles exist with the vehicle rule off');
  } finally {
    await off.shutdown();
  }
  return { ok: failures.length === 0, failures, rows };
}
