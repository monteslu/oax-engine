// A mover at rest is navmesh floor (docs/navigation.md, "Movers as floor"):
// a brush entity with navfloor 1 joins the mesh at its spawn pose as solid
// floor (G_OAX_NAV_ADDMODEL before the commit) and leaves it when it moves
// off that pose (G_OAX_NAV_SETMODEL, the tiles it reaches rebuilt).
//
// oax_navfloor: two ledges bridged by a func_oax_mover with navfloor 1.
//   - at rest: a path from the west ledge to the east one exists and runs
//     over the bridge (complete, its last point on the east ledge);
//   - the player walks into the trigger that sends the bridge into the
//     floor: the same path is now partial (the mesh has no bridge), and
//     g_nav_floors reads 0 where it read 1.
// Control: the first path is the one that must be complete; a mesh that
// never had the bridge fails it, a mesh that never rebuilt fails the second.

import fs from 'node:fs';
import path from 'node:path';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues } from '../lib/values.mjs';
import { WEST, EAST, TRIGGER } from '../../maps/src/oax_navfloor.mjs';

export const name = 'nav-floor';

const MAP = 'oax_navfloor';
const v3 = (p) => p.join(' ');
const PATH = `nav_path ${v3(WEST)} ${v3(EAST)}`;

function parsePaths(log) {
  // "nav_path: <n> <flags> x y z ..." lines in order
  return log.split('\n').filter((l) => l.startsWith('nav_path: ')).map((l) => {
    const n = l.slice(10).trim().split(/\s+/).map(Number);
    const pts = [];
    for (let i = 2; i + 2 < n.length; i += 3) pts.push([n[i], n[i + 1], n[i + 2]]);
    return { count: n[0], flags: n[1], pts };
  });
}

export async function run() {
  const failures = [];
  const rows = [];
  const inside = [(TRIGGER.lo[0] + TRIGGER.hi[0]) / 2, (TRIGGER.lo[1] + TRIGGER.hi[1]) / 2, TRIGGER.lo[2] + 30];
  const home = runNative('nav-floor', MAP, [
    'wait 100',
    PATH, 'debugvalues nav_floor_0.txt',
    `setviewpos ${v3(inside)} 0`,
    'wait 200',                       // the bridge takes 1 s to go; its tiles rebuild as it leaves
    PATH, 'debugvalues nav_floor_1.txt',
  ]);
  const log = fs.readFileSync(path.join(home, 'native.log'), 'utf8');
  const paths = parsePaths(log);
  const values = [0, 1].map((i) => {
    const f = path.join(home, 'baseoa', `nav_floor_${i}.txt`);
    return fs.existsSync(f) ? parseDebugValues(fs.readFileSync(f, 'utf8')) : {};
  });
  rows.push(`navmesh: ${values[0].sv_nav_polys ?? '?'} polygons, ${values[0].sv_nav_models ?? '?'} floor models; g_nav_floor_count ${values[0].g_nav_floor_count ?? '?'}, g_nav_floors ${values[0].g_nav_floors ?? '?'} then ${values[1].g_nav_floors ?? '?'}`);
  if (paths.length < 2) {
    failures.push(`expected two nav_path results, got ${paths.length} (see ${home}/native.log)`);
    return { ok: false, failures, rows };
  }
  const [a, b] = paths;
  const endA = a.pts[a.pts.length - 1] || [0, 0, 0];
  const reachA = a.flags === 0 && a.count >= 2 && Math.abs(endA[0] - EAST[0]) < 48 && Math.abs(endA[1] - EAST[1]) < 48;
  rows.push(`at rest: ${a.count} points, flags ${a.flags}, ends at ${endA.join(' ')}; after the bridge left: ${b.count} points, flags ${b.flags}`);
  if (values[0].sv_nav_models !== '1') failures.push(`the build did not include the floor mover (sv_nav_models ${values[0].sv_nav_models})`);
  if (values[0].g_nav_floors !== '1') failures.push(`the game did not register the bridge as floor (g_nav_floors ${values[0].g_nav_floors})`);
  if (!reachA) failures.push('at rest, no complete path crosses the bridge to the east ledge');
  if (values[1].g_nav_floors !== '0') failures.push(`the bridge left but the game kept it as floor (g_nav_floors ${values[1].g_nav_floors})`);
  if (b.flags === 0 && b.count >= 2) failures.push('the bridge left but the path to the east ledge is still complete (the tiles did not rebuild)');
  return { ok: failures.length === 0, failures, rows };
}
