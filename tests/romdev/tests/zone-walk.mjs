// func_oax_zone current_walk: a zone's current drags walking players too
// (UE1's ZoneVelocity on a train roof), against the ground friction.
//   - standing in the current_walk hall, the player drifts +X on the ground;
//   - control: the same current without current_walk leaves a standing
//     player where it is (airborne and swimming only, as before);
//   - the cart and the native build drift alike: the same steady speed (the
//     current against the ground friction) and drift rate (their traces
//     start at different moments of a drift that begins on placement, so
//     the positions themselves are not compared).

import { Session } from '../lib/romdev.mjs';
import { cartRun, nativeRun } from '../lib/sim.mjs';
import { WALK, STILL } from '../../maps/src/oax_zone_walk.mjs';

export const name = 'zone-walk';

const MAP = 'oax_zone_walk';
const grounded = (rows) => rows.filter((r) => r[7] !== -1);
const drift = (rows) => rows[rows.length - 1][1] - rows[0][1];
// the steady state: the median x velocity over the trace's second half, and
// the drift per second over it
const median = (a) => { const b = [...a].sort((x, y) => x - y); return b[b.length >> 1]; };
const steady = (rows) => {
  const h = rows.slice(rows.length >> 1);
  return { vx: median(h.map((r) => r[4])), rate: (h[h.length - 1][1] - h[0][1]) / ((h[h.length - 1][0] - h[0][0]) / 1000) };
};

export async function run() {
  const failures = [], rows = [];
  const fail = (m) => failures.push(m);
  const s = new Session('zone-walk');
  let cart;
  try {
    cart = { walk: await cartRun(s, MAP, WALK, 'zstand'), still: await cartRun(s, MAP, STILL, 'zstand') };
  } finally {
    await s.shutdown();
  }
  const nat = { walk: nativeRun('zone-walk-walk', MAP, WALK, 'zstand'), still: nativeRun('zone-walk-still', MAP, STILL, 'zstand') };
  for (const [b, r] of [['cart', cart], ['native', nat]]) {
    const w = r.walk.rows, st = r.still.rows;
    const vx = Math.max(...w.map((x) => x[4]));
    rows.push(`${b}: current_walk: ${grounded(w).length}/${w.length} rows on the ground, drift ${drift(w).toFixed(2)} units, top vx ${vx.toFixed(2)}; without: drift ${drift(st).toFixed(3)}`);
    if (grounded(w).length < w.length * 0.9) fail(`${b}: the player left the ground in the current_walk hall`);
    if (!(drift(w) > 40)) fail(`${b}: the walking current did not drag the standing player (${drift(w).toFixed(2)})`);
    if (Math.abs(drift(st)) > 0.5) fail(`${b}: control: a current without current_walk moved a standing player (${drift(st).toFixed(3)})`);
  }
  const sc = steady(cart.walk.rows), sn = steady(nat.walk.rows);
  rows.push(`steady drift: cart ${sc.vx.toFixed(2)} ups (${sc.rate.toFixed(2)} units/s), native ${sn.vx.toFixed(2)} ups (${sn.rate.toFixed(2)} units/s)`);
  if (Math.abs(sc.vx - sn.vx) > 0.5 || Math.abs(sc.rate - sn.rate) > sn.rate * 0.02) fail('cart and native drift differently');
  return { ok: failures.length === 0, failures, rows };
}
