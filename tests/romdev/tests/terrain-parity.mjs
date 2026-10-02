// Terrain movement parity (phase 7): the native client and the cart play
// the same pad scripts over heightmap terrain (oax_terrain: hills, a slope
// too steep to climb, a cliff, tree trunks) and their server-side player
// states must be identical at every command time both recorded. Terrain
// collision runs in cm_trace for pmove (server and client prediction), so
// any float difference in it would show here.
//
// Also: the player never sinks below the terrain surface (heights from the
// map's own baked lump, triangles as the engine splits them), and actually
// walks on terrain (ground under it, z well above the floor below).
// Controls: with cm_noTerrain 1 the same script on the cart must differ
// (the player falls to the floor under the terrain), and a different
// script must not match native.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, mapPath } from '../lib/scenes.mjs';
import { runNative } from '../lib/native.mjs';
import { TRACE_ROWS, TRACE_COLS } from '../lib/movement.mjs';
import { SIM_SCRIPTS } from '../lib/sim-scripts.mjs';
import { terrainFromBsp, allFoliage } from '../lib/terrain.mjs';

export const name = 'terrain-parity';

const MAP = 'oax_terrain';
// start points chosen on the terrain (z is set from the surface)
const CASES = [
  { script: 'terrain_run', x: -1000, y: -1000, yaw: 45 },
  { script: 'terrain_bank', x: -200, y: 700, yaw: 180 },
  { script: 'terrain_cliff', x: 450, y: 650, yaw: 0 },
  // into a solid tree trunk (foliage placement is deterministic: lib/terrain.mjs ports it)
  { script: 'terrain_cliff', x: -1240, y: -960, yaw: 0, trunk: true },
];

function nativeRows(file) {
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  const start = lines.find((l) => l.startsWith('# start '));
  return {
    start: start ? Number(start.split(' ')[2]) : null,
    rows: lines.filter((l) => !l.startsWith('#')).map((l) => l.split(' ').map((v, k) => (k >= 1 && k <= 6 ? Math.fround(Number(v)) : Number(v)))),
  };
}

function fromStart(rows, t0) {
  if (t0 == null || t0 < 0 || !rows.some((r) => Math.hypot(r[4], r[5]) > 0.5)) return null;
  const out = new Map();
  for (const r of rows) if (r[0] >= t0 && !out.has(r[0] - t0)) out.set(r[0] - t0, r);
  return out;
}

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

async function cartRun(s, c, extra = '') {
  await loadScene(s, MAP);
  if (extra) { await s.command(extra); await s.step(2); }
  await s.command(`setviewpos ${c.x} ${c.y} ${c.z} ${c.yaw}`);
  await s.step(60);
  const before = await s.read('trace_count');
  await s.command(`padscript padscripts/${c.script}.pad`);
  await s.step(SIM_SCRIPTS[c.script].reduce((n, st) => n + st.frames, 0) + 30);
  const after = await s.read('trace_count');
  const n = after - before;
  if (n <= 0 || n > TRACE_ROWS) throw new Error(`${c.script}: ${n} trace rows`);
  const flat = await s.read('trace');
  const rows = [];
  for (let i = before; i < after; i++) {
    const at = (i % TRACE_ROWS) * TRACE_COLS;
    const r = flat.slice(at, at + TRACE_COLS);
    rows.push([r[8], ...[r[1], r[2], r[3], r[4], r[5], r[6]].map(Math.fround), r[7]]);
  }
  return fromStart(rows, await s.read('pad_start_time'));
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const terrain = terrainFromBsp(fs.readFileSync(mapPath(MAP)))[0];
  if (!terrain) return { ok: false, failures: [`${MAP} has no OAX_TERRAIN lump`], rows };
  const PLAYER_MINS_Z = -24;
  let firstCart = null, firstNative = null;

  for (const c0 of CASES) {
    const c = { ...c0, z: Math.ceil(terrain.groundZ(c0.x, c0.y)) + 40 };
    const label = `${MAP}/${c.script}${c.trunk ? '@trunk' : ''}`;
    const home = runNative(`tparity-${c.script}${c.trunk ? "-trunk" : ""}`, MAP, [
      'exec padscripts/padbinds.cfg', 'bot_enable 0', 'wait 120',
      `setviewpos ${c.x} ${c.y} ${c.z} ${c.yaw}`, 'wait 200',
      `trace tparity_${c.script}${c.trunk ? "_trunk" : ""}.txt`,
      `padscript padscripts/${c.script}.pad "wait 8; trace stop; quit"`,
    ], { quit: false });
    const nfile = path.join(home, 'baseoa', `tparity_${c.script}${c.trunk ? "_trunk" : ""}.txt`);
    if (!fs.existsSync(nfile)) { failures.push(`${label}: native wrote no trace`); continue; }
    const nr = nativeRows(nfile);
    const n = fromStart(nr.rows, nr.start);

    const s = new Session(`tparity-${c.script}${c.trunk ? "-trunk" : ""}`);
    let w;
    try {
      w = await cartRun(s, c);
    } finally {
      await s.shutdown();
    }
    if (!n || !w) { failures.push(`${label}: no movement in ${!n ? 'native' : 'cart'} trace`); continue; }
    fs.writeFileSync(path.join(out, `tparity_${c.script}${c.trunk ? "_trunk" : ""}.cart.json`), JSON.stringify([...w.entries()]));
    if (!firstCart) { firstCart = { c, w }; firstNative = n; }

    const { common, firstDiff, maxErr } = compare(n, w);
    // on the terrain, never below it
    let minClear = Infinity, onGround = 0, airborne = 0, zMin = Infinity, zMax = -Infinity, dist = 0, prev = null;
    for (const r of w.values()) {
      const surf = terrain.heightAt(r[1], r[2]);
      if (surf !== null) minClear = Math.min(minClear, r[3] + PLAYER_MINS_Z - surf);
      if (r[7] === -1) airborne++; else onGround++;
      zMin = Math.min(zMin, r[3]); zMax = Math.max(zMax, r[3]);
      if (prev) dist += Math.hypot(r[1] - prev[1], r[2] - prev[2]);
      prev = r;
    }
    rows.push(`${label}: ${common} common command times (native ${n.size}, cart ${w.size}), max difference ${maxErr}; ` +
      `${dist.toFixed(0)} units travelled, z ${zMin.toFixed(1)}..${zMax.toFixed(1)}, ${onGround} grounded / ${airborne} airborne rows, ` +
      `lowest feet clearance over the surface ${minClear.toFixed(3)}`);
    if (common < n.size) failures.push(`${label}: only ${common} of native's ${n.size} command times recorded by the cart`);
    if (common < 100) failures.push(`${label}: only ${common} command times in common`);
    if (firstDiff) failures.push(`${label}: first difference at ${firstDiff}`);
    if (c.trunk) {
      // the nearest trunk ahead: the player must reach it and not pass through it
      const fol = terrain.foliage.findIndex((f) => f.collideRadius > 0);
      const trees = allFoliage(terrain, fol).filter((p) => p.x > c.x && Math.abs(p.y - c.y) < 40);
      const tree = trees.sort((a, b) => a.x - b.x)[0];
      const r = terrain.foliage[fol].collideRadius + 15;
      let closest = Infinity, past = false;
      for (const p of w.values()) {
        closest = Math.min(closest, Math.max(Math.abs(p[1] - tree.x), Math.abs(p[2] - tree.y)));
        if (p[1] > tree.x + r && Math.abs(p[2] - tree.y) < r - 1) past = true;
      }
      rows.push(`${label}: trunk at ${tree.x.toFixed(1)} ${tree.y.toFixed(1)}: closest box distance ${closest.toFixed(3)} (touching at ${r})`);
      if (closest < r - 0.01) failures.push(`${label}: the player box entered the trunk (${closest.toFixed(3)} < ${r})`);
      if (closest > r + 2) failures.push(`${label}: the player never reached the trunk (${closest.toFixed(1)})`);
      if (past) failures.push(`${label}: the player passed through the trunk`);
    }
    if (minClear < -0.01) failures.push(`${label}: the player sank ${(-minClear).toFixed(3)} units into the terrain`);
    if (!(onGround > 50)) failures.push(`${label}: the player never stood on the terrain`);
    if (!(airborne > 5)) failures.push(`${label}: the script never left the ground (no jump/fall exercised)`);
  }

  // controls on the cart
  if (firstCart) {
    const s = new Session('tparity-noterrain');
    try {
      const w = await cartRun(s, firstCart.c, 'cm_noTerrain 1');
      const r = w ? compare(firstNative, w) : { firstDiff: 'no movement' };
      const lowest = w ? Math.min(...[...w.values()].map((x) => x[3])) : NaN;
      rows.push(`control cm_noTerrain 1: ${r.firstDiff ? 'differs at ' + r.firstDiff : 'IDENTICAL'}, lowest z ${lowest.toFixed(1)} (terrain bottom ${terrain.bottom})`);
      if (!r.firstDiff) failures.push('control: the trace without terrain collision matched native');
      if (!(lowest < terrain.bottom)) failures.push('control: without terrain collision the player did not fall below the terrain');
    } finally {
      await s.shutdown();
    }
    const s2 = new Session('tparity-otherscript');
    try {
      const w = await cartRun(s2, { ...firstCart.c, script: 'terrain_cliff' });
      const r = compare(firstNative, w || new Map());
      rows.push(`control other script: ${r.firstDiff ? 'differs at ' + r.firstDiff : 'IDENTICAL'}`);
      if (!r.firstDiff) failures.push('control: a different script matched native');
    } finally {
      await s2.shutdown();
    }
  }
  return { ok: failures.length === 0, failures, rows };
}
