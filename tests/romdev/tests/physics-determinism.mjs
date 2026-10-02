// Phase 6 exit: Box3D physics is deterministic for any worker count and
// identical on the native build and the cart.
//
// The game module's `physscene <tag> <workers> <ticks> [perturb]`
// (oa-gamecode g_oax_phys.c) builds a heavy scripted scene in its own world
// on oax_phys: static collision from the map's brushes and patch plus a
// height field, four 36-box stacks, 320 debris bodies (boxes, spheres,
// capsules, convex hulls, compounds), 16 ragdolls (11 capsules each on ball
// and hinge joints), a hinged bridge, a spherical chain, a cart on wheel
// joints with motors, a prismatic piston, distance ropes, a soft weld, a
// kinematic sweeper, three explosions and ray batches; then steps it tick
// by tick and records the world hash (every body's position, rotation,
// velocities, sleep state; plus the ray hits every 30 ticks) per tick.
//
// Checked, for 600 ticks (9.6 s):
//   - per-tick hashes identical for 1, 4 and 8 workers on the cart, which
//     runs Box3D's workers on WASI threads (phys_threads must be 7);
//   - the same on the native build (Box3D's own thread pool);
//   - native == cart, tick for tick;
//   - control: the same scene with one debris body moved by 0.001 units
//     must differ (and the test reports the first differing tick);
//   - the scene really ran: hundreds of bodies, joints, contacts, ray hits.
// Rows publish the physics step time per run and the body counts.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues, readValues } from '../lib/values.mjs';

export const name = 'physics-determinism';

const MAP = 'oax_phys';
const TICKS = 600;
const RUNS = [
  { tag: 'w1', workers: 1, perturb: '' },
  { tag: 'w4', workers: 4, perturb: '' },
  { tag: 'w8', workers: 8, perturb: '' },
  { tag: 'p1', workers: 1, perturb: '0.001' },
];

function scene(v, tag) {
  const hashes = [];
  for (let i = 0; v[`phys_scene_${tag}_h${i}`] !== undefined; i++) {
    hashes.push(...v[`phys_scene_${tag}_h${i}`].trim().split(/\s+/).filter(Boolean));
  }
  const n = (k) => Number(v[`phys_scene_${tag}_${k}`]);
  return {
    done: v[`phys_scene_${tag}_done`] === '1',
    chain: v[`phys_scene_${tag}_chain`],
    ticks: n('ticks'), bodies: n('bodies'), awake: n('awake'), joints: n('joints'),
    contacts: n('contacts'), workers: n('workers'), rayhits: n('rayhits'), stepMs: n('step_ms'),
    hashes,
  };
}

function firstDiff(a, b) {
  const n = Math.max(a.length, b.length);
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) return i;
  return -1;
}

async function cartRuns() {
  const s = new Session('physics-determinism');
  const res = {};
  let threads;
  let onWorkers = 0;
  try {
    await s.load();
    await s.command(`bot_enable 0; g_doWarmup 0; devmap ${MAP}`);
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
    await s.step(10);
    for (const r of RUNS) {
      const t0 = process.hrtime.bigint();
      await s.command(`physscene ${r.tag} ${r.workers} ${TICKS} ${r.perturb}`);
      let v = {};
      for (let i = 0; i < 400; i++) {
        await s.step(10);
        v = await readValues(s);
        if (v[`phys_scene_${r.tag}_done`] === '1') break;
      }
      const ow = Number(v.phys_tasks_on_workers || 0);
      res[r.tag] = { ...scene(v, r.tag), wallMs: Number(process.hrtime.bigint() - t0) / 1e6, workerTasks: ow - onWorkers };
      onWorkers = ow;
      threads = v.phys_threads;
    }
  } finally {
    await s.shutdown();
  }
  return { res, threads };
}

function nativeRuns() {
  const lines = ['bot_enable 0', 'g_doWarmup 0', 'wait 20'];
  for (const r of RUNS) lines.push(`physscene ${r.tag} ${r.workers} ${TICKS} ${r.perturb}`.trim(), 'wait 400');
  lines.push('debugvalues values.txt');
  const home = runNative('physics-determinism', MAP, lines);
  const f = path.join(home, 'baseoa', 'values.txt');
  const v = fs.existsSync(f) ? parseDebugValues(fs.readFileSync(f, 'utf8')) : {};
  const res = {};
  for (const r of RUNS) res[r.tag] = scene(v, r.tag);
  return { res };
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const cart = await cartRuns();
  const native = nativeRuns();
  fs.writeFileSync(path.join(out, 'physics-determinism.json'), JSON.stringify({ cart, native }, null, 2));

  for (const [label, side] of [['cart', cart.res], ['native', native.res]]) {
    for (const r of RUNS) {
      const x = side[r.tag];
      rows.push(`${label} ${r.tag}${r.perturb ? ' (perturbed)' : ''}: ${x.workers} workers, ${x.ticks} ticks, chain ${x.chain}, ` +
        `${x.bodies} bodies (${x.awake} awake), ${x.joints} joints, ${x.contacts} contacts, ${x.rayhits} ray hits, ` +
        `physics step ${Number(x.stepMs).toFixed(1)} ms total${x.wallMs ? `, wall ${x.wallMs.toFixed(0)} ms` : ''}`);
      if (!x.done) failures.push(`${label} ${r.tag}: the scene never finished`);
      if (x.hashes.length !== TICKS) failures.push(`${label} ${r.tag}: ${x.hashes.length} tick hashes, wanted ${TICKS}`);
    }
    const base = side.w1;
    // the scene is real: hundreds of bodies, joints and contacts, rays that hit
    if (!(base.bodies >= 600)) failures.push(`${label}: only ${base.bodies} bodies`);
    if (!(base.joints >= 150)) failures.push(`${label}: only ${base.joints} joints`);
    if (!(base.contacts >= 200)) failures.push(`${label}: only ${base.contacts} contacts`);
    if (!(base.rayhits > 0)) failures.push(`${label}: no ray hits`);
    if (new Set(base.hashes).size < TICKS / 2) failures.push(`${label}: the world hash barely changes (${new Set(base.hashes).size} distinct)`);
    for (const t of ['w4', 'w8']) {
      if (side[t].workers !== Number(t.slice(1))) failures.push(`${label} ${t}: the world ran ${side[t].workers} workers`);
      const d = firstDiff(base.hashes, side[t].hashes);
      rows.push(`${label}: 1 vs ${t.slice(1)} workers: ${d < 0 ? `identical over ${TICKS} ticks` : `DIFFER from tick ${d}`}`);
      if (d >= 0) failures.push(`${label}: ${t} differs from 1 worker from tick ${d}`);
    }
    // control: a perturbed start must change the hashes
    const dp = firstDiff(base.hashes, side.p1.hashes);
    rows.push(`${label} control: perturbed run ${dp < 0 ? 'IDENTICAL (control failed)' : `differs from tick ${dp}`}`);
    if (dp < 0) failures.push(`${label}: the perturbed control gave the same hashes`);
  }
  // the cart really ran threads
  rows.push(`cart worker threads spawned: ${cart.threads}`);
  if (Number(cart.threads) !== 7) failures.push(`cart: ${cart.threads} worker threads, wanted 7`);
  // and the threads did the work: Box3D tasks ran on them only when asked for
  for (const r of RUNS) {
    const n = cart.res[r.tag].workerTasks;
    rows.push(`cart ${r.tag}: ${n} Box3D tasks ran on worker threads`);
    if (r.workers > 1 && !(n > 0)) failures.push(`cart ${r.tag}: no task ran on a worker thread`);
    if (r.workers === 1 && n !== 0) failures.push(`cart ${r.tag}: ${n} tasks ran on worker threads with 1 worker`);
  }
  // native == cart, tick for tick, for every run
  for (const r of RUNS) {
    const d = firstDiff(native.res[r.tag].hashes, cart.res[r.tag].hashes);
    rows.push(`native vs cart ${r.tag}: ${d < 0 ? `identical over ${TICKS} ticks` : `DIFFER from tick ${d}`}`);
    if (d >= 0) failures.push(`native and cart differ in ${r.tag} from tick ${d}`);
  }
  return { ok: failures.length === 0, failures, rows };
}
