// Surface world, gameplay (step 7.5): oax_surfworld's look is OAX_SURFACES
// surfaces of zero thickness; collision, Box3D and the navmesh come from a
// separate hull of caulk brushes plus one OAX_COLLISION mesh (the arch).
//
// Checked on the cart and the native build:
//   - movement: two pad scripts (up the ramp onto the platform and off its
//     far edge; into the arch's leg, then sideways under it) give the same
//     server-side player state at every command time on both builds; the
//     player stands on the hull (never below the floor surface), climbs
//     onto the platform, and stops at the arch's collision mesh without
//     entering it;
//   - Box3D: phys_droptest drops boxes on the floor, the platform, the
//     ramp and the arch's top; they come to rest on the hull (or the arch
//     mesh) at the expected heights, asleep, with the same world hash on
//     both builds;
//   - navmesh: built from the hull (the map has no AAS), same polygons and
//     hash on both builds, path queries (across a room around the
//     platform, up the ramp onto it, under the arch) identical and complete.
// Controls, each must fail the corresponding check:
//   - hull removed (oax_surfworld_nohull: same surfaces, hull floor 512
//     lower): the player falls through the surfaces, the boxes fall, the
//     navmesh differs;
//   - surfaces removed (oax_surfworld_nosurf: same hull, no surface world):
//     solid but invisible, so the movement trace is IDENTICAL to the full
//     map's (the positive control: collision never came from the surfaces);
//   - cm_noCollisionMeshes 1: the player walks through the arch's leg and
//     the box on the arch falls to the floor;
//   - a goal below the world has no path.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { loadScene } from '../lib/scenes.mjs';
import { runNative } from '../lib/native.mjs';
import { TRACE_ROWS, TRACE_COLS } from '../lib/movement.mjs';
import { SIM_SCRIPTS } from '../lib/sim-scripts.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';

export const name = 'surface-world-play';

const MAP = 'oax_surfworld';
const FLOOR_ORIGIN_Z = 24;        // player origin standing on z = 0 (mins z -24)
const PLATFORM_ORIGIN_Z = 64 + 24;
const ARCH = { x: 700, y: 300, inner: 72, outer: 104, depth: 32 };
const PLAYER_HALF = 15;

const CASES = [
  { script: 'surf_ramp', x: -150, y: -280, z: 40, yaw: 180 },
  { script: 'surf_arch', x: 560, y: 212, z: 40, yaw: 0 },
];

// boxes (16 units) dropped by phys_droptest: [x, y, z, expected rest z of the center, what]
const DROPS = [
  [-900, -450, 120, 8, 'floor A'],
  [-900, 400, 150, 8, 'floor A'],
  [-700, -280, 200, 72, 'platform'],
  [800, -400, 100, 8, 'floor B'],
  [ARCH.x, ARCH.y, 220, ARCH.outer + 8, 'arch top'],
  [ARCH.x, ARCH.y, 40, 8, 'under the arch'],
  [-430, -280, 200, null, 'ramp'],
];
const DROP_TICKS = 400;

const NAV = [
  [[-900, -400, 30], [-100, 300, 30]],      // across room A, around the platform (the closed door is solid to the navmesh)
  [[-900, 400, 30], [-680, -280, 100]],     // up the ramp onto the platform
  [[560, 300, 30], [850, 300, 30]],         // under the arch
];
const NAV_BELOW = [[-900, -400, 30], [0, 0, -2000]];

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
  let common = 0, firstDiff = null;
  for (const [dt, a] of n) {
    const b = w.get(dt);
    if (!b) continue;
    common++;
    for (let k = 1; k <= 7; k++) if (a[k] !== b[k] && !firstDiff) firstDiff = `t+${dt}ms col ${k}: ${a[k]} vs ${b[k]}`;
  }
  return { common, firstDiff };
}

async function cartRun(map, c, extra = '') {
  const s = new Session(`swplay-${map}-${c.script}`);
  try {
    await loadScene(s, map);
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
  } finally {
    await s.shutdown();
  }
}

function nativeRun(c) {
  const tag = `swplay_${c.script}`;
  const home = runNative(`swplay-${c.script}`, MAP, [
    'exec padscripts/padbinds.cfg', 'bot_enable 0', 'wait 120',
    `setviewpos ${c.x} ${c.y} ${c.z} ${c.yaw}`, 'wait 200',
    `trace ${tag}.txt`,
    `padscript padscripts/${c.script}.pad "wait 8; trace stop; quit"`,
  ], { quit: false });
  const f = path.join(home, 'baseoa', `${tag}.txt`);
  if (!fs.existsSync(f)) return null;
  const nr = nativeRows(f);
  return fromStart(nr.rows, nr.start);
}

const dropCmd = (tag) => `phys_droptest ${tag} ${DROP_TICKS} ${DROPS.map((d) => d.slice(0, 3).join(' ')).join(' ')}`;
function drops(v, tag) {
  return {
    hash: v[`phys_drop_${tag}_hash`], asleep: Number(v[`phys_drop_${tag}_asleep`]),
    bodies: DROPS.map((_, i) => (v[`phys_drop_${tag}_${i}`] || 'none').split(' ').map(Number)),
  };
}

async function cartValues(map, cmds, extra = '') {
  const s = new Session(`swplay-${map}-values`);
  try {
    await s.load();
    await s.command(`bot_enable 0;g_doWarmup 0;devmap ${map}`);
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
    await s.step(10);
    if (extra) { await s.command(extra); await s.step(2); }
    const navPaths = [];
    for (const c of cmds) {
      await s.command(c);
      await s.step(3);
      if (c.startsWith('nav_path')) navPaths.push((await readValues(s)).nav_path);
    }
    return { v: await readValues(s), navPaths };
  } finally {
    await s.shutdown();
  }
}

const navCmd = (q) => `nav_path ${q[0].join(' ')} ${q[1].join(' ')}`;

export async function run({ out }) {
  const failures = [];
  const rows = [];

  // ---- movement --------------------------------------------------------------
  const cartTraces = {};
  for (const c of CASES) {
    const label = `${MAP}/${c.script}`;
    const n = nativeRun(c);
    const w = await cartRun(MAP, c);
    if (!n || !w) { failures.push(`${label}: no movement in ${!n ? 'native' : 'cart'} trace`); continue; }
    cartTraces[c.script] = w;
    fs.writeFileSync(path.join(out, `swplay_${c.script}.cart.json`), JSON.stringify([...w.entries()]));
    const { common, firstDiff } = compare(n, w);
    let zMin = Infinity, zMax = -Infinity, grounded = 0, airborne = 0, dist = 0, prev = null;
    for (const r of w.values()) {
      zMin = Math.min(zMin, r[3]); zMax = Math.max(zMax, r[3]);
      if (r[7] === -1) airborne++; else grounded++;
      if (prev) dist += Math.hypot(r[1] - prev[1], r[2] - prev[2]);
      prev = r;
    }
    rows.push(`${label}: ${common} common command times (native ${n.size}, cart ${w.size}), ${firstDiff ? 'FIRST DIFFERENCE ' + firstDiff : 'identical'}; ` +
      `${dist.toFixed(0)} units, z ${zMin.toFixed(2)}..${zMax.toFixed(2)}, ${grounded} grounded / ${airborne} airborne rows`);
    if (common < n.size || common < 100) failures.push(`${label}: only ${common} of native's ${n.size} command times on the cart`);
    if (firstDiff) failures.push(`${label}: native and cart differ at ${firstDiff}`);
    if (zMin < FLOOR_ORIGIN_Z - 0.01) failures.push(`${label}: the player went below the floor (z ${zMin})`);
    if (!(grounded > 50)) failures.push(`${label}: the player never stood on the hull`);
    if (c.script === 'surf_ramp') {
      const onPlatform = [...w.values()].filter((r) => r[7] !== -1 && Math.abs(r[3] - PLATFORM_ORIGIN_Z) < 0.25 && r[1] < -560).length;
      rows.push(`${label}: ${onPlatform} grounded rows on the platform (origin z ${PLATFORM_ORIGIN_Z})`);
      if (!(onPlatform > 5)) failures.push(`${label}: the player never stood on the platform`);
      if (!(airborne > 5)) failures.push(`${label}: no jump or fall exercised`);
    } else {
      // stopped by the arch's leg: the player box never overlaps it
      const legY = [ARCH.y - ARCH.outer - PLAYER_HALF, ARCH.y - ARCH.inner + PLAYER_HALF];
      const x0 = ARCH.x - ARCH.depth / 2;
      let deepest = -Infinity, passed = false, reached = Infinity;
      for (const r of w.values()) {
        if (r[2] > legY[0] + 1 && r[2] < legY[1] - 1 && r[3] < 104) {
          deepest = Math.max(deepest, r[1] + PLAYER_HALF - x0);
          reached = Math.min(reached, x0 - (r[1] + PLAYER_HALF));
        }
        if (r[1] > ARCH.x + ARCH.depth / 2 + PLAYER_HALF) passed = true;
      }
      rows.push(`${label}: deepest box overlap with the arch's leg ${deepest.toFixed(3)} (0 = touching), went under the arch: ${passed}`);
      if (deepest > 0.01) failures.push(`${label}: the player entered the arch's leg by ${deepest.toFixed(3)}`);
      if (deepest < -2) failures.push(`${label}: the player never reached the arch's leg (${deepest.toFixed(1)})`);
      if (!passed) failures.push(`${label}: the player never passed under the arch`);
    }
  }

  // controls on the cart
  if (cartTraces.surf_ramp) {
    const c = CASES[0];
    const nohull = await cartRun('oax_surfworld_nohull', c);
    const lowest = nohull ? Math.min(...[...nohull.values()].map((r) => r[3])) : NaN;
    const d = nohull ? compare(cartTraces.surf_ramp, nohull) : { firstDiff: 'no movement' };
    rows.push(`control hull removed (oax_surfworld_nohull): lowest z ${lowest.toFixed(1)}, ${d.firstDiff ? 'differs at ' + d.firstDiff : 'IDENTICAL'}`);
    if (!(lowest < -400)) failures.push('control: without the hull the player did not fall through the surfaces');
    if (!d.firstDiff) failures.push('control: the hull-less trace matched the full map');

    const nosurf = await cartRun('oax_surfworld_nosurf', c);
    const e = nosurf ? compare(cartTraces.surf_ramp, nosurf) : { firstDiff: 'no movement', common: 0 };
    rows.push(`control surfaces removed (oax_surfworld_nosurf): ${e.common} common command times, ${e.firstDiff ? 'DIFFERS at ' + e.firstDiff : 'identical (solid but invisible)'}`);
    if (e.firstDiff || !(e.common > 100)) failures.push('control: without its surfaces the hull did not collide exactly the same');
  }
  if (cartTraces.surf_arch) {
    const w = await cartRun(MAP, CASES[1], 'cm_noCollisionMeshes 1');
    const x0 = ARCH.x - ARCH.depth / 2;
    let deepest = -Infinity;
    for (const r of (w || new Map()).values()) {
      if (r[2] > ARCH.y - ARCH.outer && r[2] < ARCH.y - ARCH.inner && r[3] < 104) deepest = Math.max(deepest, r[1] + PLAYER_HALF - x0);
    }
    rows.push(`control cm_noCollisionMeshes 1: deepest overlap with the arch's leg ${deepest.toFixed(1)}`);
    if (!(deepest > 8)) failures.push('control: without collision meshes the player still stopped at the arch');
  }

  // ---- Box3D and the navmesh ------------------------------------------------------
  const cmds = [dropCmd('full'), ...NAV.map(navCmd), navCmd(NAV_BELOW)];
  const home = runNative('swplay-values', MAP, ['bot_enable 0', 'wait 20', ...cmds.flatMap((c) => [c, 'wait 4']), 'debugvalues values.txt']);
  const vfile = path.join(home, 'baseoa', 'values.txt');
  const nv = fs.existsSync(vfile) ? parseDebugValues(fs.readFileSync(vfile, 'utf8')) : {};
  const nlog = fs.existsSync(path.join(home, 'native.log')) ? fs.readFileSync(path.join(home, 'native.log'), 'utf8') : '';
  const nativePaths = [...nlog.matchAll(/^nav_path: (.*)$/gm)].map((m) => m[1].trim());
  const cart = await cartValues(MAP, cmds);
  const cv = cart.v;

  const nd = drops(nv, 'full'), cd = drops(cv, 'full');
  rows.push(`Box3D: ${DROPS.length} boxes, ${DROP_TICKS} ticks: cart hash ${cd.hash} (${cd.asleep} asleep), native hash ${nd.hash} (${nd.asleep} asleep)`);
  if (!cd.hash || cd.hash !== nd.hash) failures.push(`Box3D: world hash native ${nd.hash} vs cart ${cd.hash}`);
  DROPS.forEach((d, i) => {
    const b = cd.bodies[i];
    rows.push(`  box on ${d[4]}: rest at ${b.slice(0, 3).map((x) => x.toFixed(2)).join(' ')}${b[3] ? ', asleep' : ', AWAKE'}${d[3] !== null ? ` (expected z ${d[3]})` : ''}`);
    if (b.length < 4 || b.some((x) => !Number.isFinite(x))) { failures.push(`Box3D: box ${i} (${d[4]}) has no state`); return; }
    if (!b[3]) failures.push(`Box3D: box ${i} (${d[4]}) never came to rest`);
    if (d[3] !== null && Math.abs(b[2] - d[3]) > 1.5) failures.push(`Box3D: box ${i} (${d[4]}) rests at z ${b[2].toFixed(2)}, expected ${d[3]}`);
    if (d[3] === null && !(b[2] > 8 && b[2] < 72)) failures.push(`Box3D: box ${i} (${d[4]}) not on the ramp (z ${b[2].toFixed(2)})`);
  });

  rows.push(`navmesh: cart ${cv.sv_nav_polys} polys hash ${cv.sv_nav_hash}; native ${nv.sv_nav_polys} polys hash ${nv.sv_nav_hash}`);
  if (!(Number(cv.sv_nav_polys) > 0)) failures.push('navmesh: the cart built none');
  if (cv.sv_nav_hash !== nv.sv_nav_hash) failures.push('navmesh: native and cart differ');
  NAV.forEach((q, k) => {
    const c = cart.navPaths[k], n = nativePaths[k];
    rows.push(`  path ${k}: cart "${c}"`);
    if (c !== n) failures.push(`navmesh path ${k}: native "${n}" vs cart "${c}"`);
    const [count, partial] = String(c).split(' ').map(Number);
    if (!(count >= 2) || partial !== 0) failures.push(`navmesh path ${k}: not a full path (${c})`);
  });
  const below = cart.navPaths[NAV.length];
  rows.push(`control: goal below the world: "${below}"`);
  if (Number(String(below).split(' ')[0]) !== 0) failures.push(`control: a goal below the world got a path (${below})`);

  // controls: no hull, no collision meshes
  const ctl = await cartValues('oax_surfworld_nohull', [dropCmd('nohull')]);
  const hd = drops(ctl.v, 'nohull');
  const fell = DROPS.map((d, i) => [d, hd.bodies[i]]).filter(([d, b]) => d[4] !== 'arch top' && b[2] < -400).length;
  rows.push(`control hull removed: ${fell}/${DROPS.length - 1} boxes fell below -400; navmesh ${ctl.v.sv_nav_polys} polys hash ${ctl.v.sv_nav_hash}`);
  if (fell !== DROPS.length - 1) failures.push(`control: without the hull only ${fell} boxes fell`);
  if (ctl.v.sv_nav_hash === cv.sv_nav_hash) failures.push('control: the hull-less map built the same navmesh');
  const nc = await cartValues(MAP, [dropCmd('nocoll')], 'cm_noCollisionMeshes 1');
  const ncd = drops(nc.v, 'nocoll');
  const archBox = DROPS.findIndex((d) => d[4] === 'arch top');
  rows.push(`control cm_noCollisionMeshes 1: the arch box rests at z ${ncd.bodies[archBox][2]}`);
  if (!(ncd.bodies[archBox][2] < 20)) failures.push('control: without collision meshes the box still rested on the arch');

  return { ok: failures.length === 0, failures, rows };
}
