// Navigation mesh (phase 7): the Recast/Detour navmesh the server builds for
// maps without AAS (sv_nav_oax.c) must be the same bytes on native and the
// cart, and its path queries must give the same corner points.
//
//   - oax_terrain: navmesh polygon count and hash native == cart; a set of
//     path queries over hills, around the steep bank and the plateau,
//     identical point for point.
//   - oax_outdoor_ctf: every item and both flags have a full (not partial)
//     path from a spawn point (g_oax_stats.c, navmesh routes).
// Controls: a different voxel size (sv_navCellSize 12) must change the
// hash; a goal inside the terrain or below the world must give no path.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';
import { runNative } from '../lib/native.mjs';

export const name = 'nav-mesh';

const MAP = 'oax_terrain';
// [start, goal] pairs (Q3 coordinates; z near the ground)
const QUERIES = [
  [[-1000, -1000, 100], [1000, 1000, 100]],
  [[-1000, -1000, 100], [600, 700, 100]],      // up onto the plateau
  [[0, 0, 60], [-900, 1100, 100]],             // past the steep bank
  [[1000, 1000, 100], [-300, -800, -100]],     // down into the bowl
  [[-1200, 1200, 150], [1200, -1200, 100]],
];
const OFF_MESH = [[0, 0, 60], [0, 0, -300]];     // inside the terrain solid
const BELOW = [[0, 0, 60], [0, 0, -2000]];       // below the world

const cmd = (q) => `nav_path ${q[0].join(' ')} ${q[1].join(' ')}`;

async function cartBoot(s, extra = '') {
  await s.load();
  await s.command(`${extra}${extra ? ';' : ''}bot_enable 0;devmap ${MAP}`);
  await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
  await s.step(10);
}

async function cartPath(s, q) {
  await s.command(cmd(q));
  await s.step(2);
  return (await readValues(s)).nav_path;
}

export async function run({ out }) {
  const failures = [];
  const rows = [];

  // native: the same queries, printed as "nav_path: <count> <flags> x y z ..."
  const home = runNative('nav-mesh', MAP, [
    ...[...QUERIES, OFF_MESH, BELOW].flatMap((q) => [cmd(q), 'wait 2']),
    'debugvalues nav_native.txt',
  ]);
  const log = fs.readFileSync(path.join(home, 'native.log'), 'utf8');
  const nativePaths = [...log.matchAll(/^nav_path: (.*)$/gm)].map((m) => m[1].trim());
  const nv = parseDebugValues(fs.readFileSync(path.join(home, 'baseoa', 'nav_native.txt'), 'utf8'));

  const s = new Session('nav-mesh');
  let cv, cartPaths = [];
  try {
    await cartBoot(s);
    cv = await readValues(s);
    for (const q of [...QUERIES, OFF_MESH, BELOW]) cartPaths.push(await cartPath(s, q));
  } finally {
    await s.shutdown();
  }
  fs.writeFileSync(path.join(out, 'nav-mesh.json'), JSON.stringify({ native: nativePaths, cart: cartPaths, nv: { polys: nv.sv_nav_polys, hash: nv.sv_nav_hash }, cv: { polys: cv.sv_nav_polys, hash: cv.sv_nav_hash } }, null, 1));
  rows.push(`${MAP}: native ${nv.sv_nav_polys} polys hash ${nv.sv_nav_hash} (${nv.sv_nav_tris} triangles in); cart ${cv.sv_nav_polys} polys hash ${cv.sv_nav_hash}`);
  if (!(Number(cv.sv_nav_polys) > 0)) failures.push('the cart built no navmesh');
  if (nv.sv_nav_hash !== cv.sv_nav_hash || nv.sv_nav_polys !== cv.sv_nav_polys) failures.push('navmesh differs between native and the cart');

  let same = 0;
  QUERIES.forEach((q, k) => {
    const n = nativePaths[k], c = cartPaths[k];
    const pts = Number(String(c).split(' ')[0]);
    if (n === c) same++;
    else failures.push(`query ${k}: native "${n}" vs cart "${c}"`);
    if (!(pts >= 2)) failures.push(`query ${k}: no path (${c})`);
    if (Number(String(c).split(' ')[1]) !== 0) failures.push(`query ${k}: partial path (${c})`);
  });
  rows.push(`${QUERIES.length} path queries: ${same} identical native vs cart; corner points per path ${cartPaths.slice(0, QUERIES.length).map((p) => p.split(' ')[0]).join(', ')}`);

  // controls: off the mesh gives nothing
  const off = cartPaths[QUERIES.length], below = cartPaths[QUERIES.length + 1];
  rows.push(`control inside the terrain: "${off}"; below the world: "${below}"`);
  if (Number(off.split(' ')[0]) !== 0) failures.push(`control: a goal inside the terrain got a path: ${off}`);
  if (Number(below.split(' ')[0]) !== 0) failures.push(`control: a goal below the world got a path: ${below}`);
  if (nativePaths[QUERIES.length] !== off || nativePaths[QUERIES.length + 1] !== below) failures.push('control queries differ native vs cart');

  // control: another voxel size must change the navmesh
  const c2 = new Session('nav-mesh-cs12');
  try {
    await cartBoot(c2, 'sv_navCellSize 12');
    const v = await readValues(c2);
    rows.push(`control sv_navCellSize 12: ${v.sv_nav_polys} polys hash ${v.sv_nav_hash}`);
    if (v.sv_nav_hash === cv.sv_nav_hash) failures.push('control: a different voxel size gave the same navmesh hash');
  } finally {
    await c2.shutdown();
  }

  // routes on the CTF map: every item from some spawn
  const r = new Session('nav-mesh-ctf');
  try {
    await r.load();
    await r.command('bot_enable 0;g_gametype 4;g_doWarmup 0;devmap oax_outdoor_ctf');
    await r.stepUntil('conn_state', (v) => v === CA_ACTIVE, 4000, 20);
    await r.step(260);   // routes are computed once level time passes 3 s
    const v = await readValues(r);
    rows.push(`oax_outdoor_ctf: navmesh ${v.sv_nav_polys} polys; items with a full path from a spawn ${v.g_items_routable}/${v.g_items_routed_total} (from ${v.g_route_spawns} spawns, source ${v.g_route_source})`);
    if (v.g_route_source !== 'navmesh') failures.push('routes not computed from the navmesh');
    if (!(Number(v.g_items_routed_total) > 0)) failures.push('control: no items counted');
    if (v.g_items_routable !== v.g_items_routed_total) failures.push(`unroutable items: ${v.g_items_unroutable}`);
  } finally {
    await r.shutdown();
  }
  return { ok: failures.length === 0, failures, rows };
}
