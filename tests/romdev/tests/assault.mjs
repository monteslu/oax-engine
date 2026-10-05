// Assault (g_gametype 14, the game code's g_oax_assault.c) on oax_assault,
// native and cart running the same console script:
//   1. round 1 starts with red attacking: three objectives, the generator
//      open and the others locked; the player (red) sees ATTACK on the HUD;
//   2. control: a rocket owned by nobody (vehrocket) hits the generator and
//      does nothing (only the attackers' damage counts);
//   3. the player's own rockets do damage it; then it falls (assault
//      complete), the keep controls open, and a respawn puts the player on
//      the field in front of the fortress (the attackers' spawns advance);
//   4. holding use beside the controls works them (progress), letting go
//      lets it slip back; they fall, the core falls, and the round ends a
//      success with its time;
//   5. after the map restart round 2 has blue attacking with red's time to
//      beat as its limit, the player now defends (DEFEND on the HUD);
//   6. blue runs out of time: red wins the match (a team point).

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';

export const name = 'assault';

const MAP = 'oax_assault';
const SETUP = ['set g_gametype 14', 'set g_oaxAssaultFirst 0', 'set g_oaxAssaultTime 0', 'cg_draw2D 1'];
// the generator stands at 1640 520 (oax_assault.mjs), the controls at 2500 -560
const FACE_GENERATOR = 'setviewpos 1340 520 64 0 2';

function shots() {
  return [
    { cmd: 'team red', settle: 300, values: 'start' },
    { cmd: 'give all;weapon 5', settle: 20 },
    { cmd: FACE_GENERATOR, settle: 30 },
    { cmd: 'vehrocket 1340 400 64 1640 520 40', settle: 120, values: 'control' },
    { cmd: FACE_GENERATOR, settle: 10 },
    { cmd: '+attack', settle: 60 },
    { cmd: '-attack', settle: 90, values: 'hit' },
    { cmd: 'assault complete generator', settle: 30, values: 'generator' },
    // respawning takes a fire press once 1.7 s have passed
    { cmd: 'kill', settle: 450 },
    { cmd: '+attack', settle: 4 },
    { cmd: '-attack', settle: 60, values: 'respawn' },
    { cmd: 'setviewpos 2440 -560 64 0 10', settle: 30 },
    { cmd: '+button2', settle: 120, values: 'using' },
    { cmd: '-button2', settle: 300, values: 'released' },
    { cmd: 'assault complete controls', settle: 20 },
    { cmd: 'assault complete core', settle: 20, values: 'won' },
    // the result shows 7 s, the map restarts, the clock starts 3 s later
    { cmd: 'wait', settle: 2600, values: 'round2', name: 'round2' },
    { cmd: 'assault limit 1', settle: 1200, values: 'decided' },
  ];
}

const list = (v, k) => String(v?.[k] ?? '').split(' ');
const num = (v, k) => Number(v?.[k]);
// g_as_objN: id type state health progress
const obj = (v, i) => { const [id, type, state, health, progress] = list(v, `g_as_obj${i}`); return { id, type: +type, state: +state, health: +health, progress: +progress }; };
const p0 = (v) => { const [team, x, y, z, health] = list(v, 'g_as_p0').map(Number); return { team, x, y, z, health }; };
const hud = (v) => { const [round, role, left, open] = list(v, 'cg_as_hud').map(Number); return { round, role, left, open }; };

function check(build, at, rows, failures) {
  const f = (m) => failures.push(`${build}: ${m}`);
  const st = at.start;
  rows.push(`${build}: start: round ${num(st, 'g_as_round')}, attackers ${num(st, 'g_as_attackers')}, objectives ${num(st, 'g_as_objectives')}, ` +
    `states ${[0, 1, 2].map((i) => obj(st, i).state).join(' ')}, player team ${p0(st).team}, HUD role ${hud(st).role}, limit ${num(st, 'g_as_limit')}`);
  if (!(num(st, 'g_as_round') === 1 && num(st, 'g_as_attackers') === 1 && num(st, 'g_as_objectives') === 3)) f('round 1 did not start with red attacking three objectives');
  if (!(obj(st, 0).state === 1 && obj(st, 1).state === 0 && obj(st, 2).state === 0)) f('the objectives did not start generator open, the rest locked');
  if (!(p0(st).team === 1 && hud(st).role === 0)) f(`the player is not an attacker on the HUD (team ${p0(st).team}, role ${hud(st).role})`);

  rows.push(`${build}: generator health: after a rocket nobody owns ${obj(at.control, 0).health}%, after the player's rockets ${obj(at.hit, 0).health}%`);
  if (obj(at.control, 0).health !== 100) f(`a rocket nobody owns damaged the generator (${obj(at.control, 0).health}%)`);
  if (!(obj(at.hit, 0).health < 100)) f("the player's rockets did not damage the generator");

  const g = at.generator, r = p0(at.respawn);
  rows.push(`${build}: generator done: states ${[0, 1, 2].map((i) => obj(g, i).state).join(' ')}; respawned at ${r.x} ${r.y} ${r.z}`);
  if (!(obj(g, 0).state === 2 && obj(g, 1).state === 1 && obj(g, 2).state === 0)) f('the controls did not open after the generator');
  if (!(r.x > 550 && r.x < 900 && r.health > 0)) f(`the attacker respawned at x ${r.x}, not at the field spawns (650..800)`);

  const u = obj(at.using, 1), rel = obj(at.released, 1);
  rows.push(`${build}: controls: progress ${u.progress}% while holding use, ${rel.progress}% after letting go`);
  if (!(u.progress > 5)) f(`holding use beside the controls made ${u.progress}% progress`);
  if (!(rel.progress < u.progress)) f('the progress did not slip back after letting go');

  const w = at.won;
  rows.push(`${build}: round 1: phase ${num(w, 'g_as_phase')}, outcome ${num(w, 'g_as_outcome')}, time ${num(w, 'g_as_r1time')} ms`);
  if (!(num(w, 'g_as_phase') === 2 && num(w, 'g_as_outcome') === 1 && num(w, 'g_as_r1time') > 0)) f('the core did not end round 1 a success');

  const r2 = at.round2;
  rows.push(`${build}: round 2: round ${num(r2, 'g_as_round')}, attackers ${num(r2, 'g_as_attackers')}, limit ${num(r2, 'g_as_limit')}, ` +
    `objectives done ${num(r2, 'g_as_done')}`);
  if (!(num(r2, 'g_as_round') === 2 && num(r2, 'g_as_attackers') === 2)) f('round 2 did not start with blue attacking');
  if (num(r2, 'g_as_limit') !== num(w, 'g_as_r1time')) f(`round 2's limit ${num(r2, 'g_as_limit')} is not round 1's time ${num(w, 'g_as_r1time')}`);
  if (num(r2, 'g_as_done') !== 0) f('the objectives were not reset for round 2');

  const d = at.decided;
  rows.push(`${build}: decided: outcome ${num(d, 'g_as_outcome')}, winner ${num(d, 'g_as_winner')}; the player (team ${p0(d).team}) on the HUD: round ${hud(d).round}, role ${hud(d).role}`);
  // the HUD's numbers come from its last drawn frame: read in round 2's own time
  if (!(p0(d).team === 1 && hud(d).round === 2 && hud(d).role === 1)) f('the player is not a defender on the HUD in round 2');
  if (!(num(d, 'g_as_outcome') === 2 && num(d, 'g_as_winner') === 1)) f('blue running out of time did not give red the match');
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const cart = await cartShots('assault', MAP, shots(), { setup: SETUP, out });
  const native = nativeShots('assault', MAP, shots(), {
    setup: SETUP.filter((c) => !c.startsWith('set g_gametype')), startArgs: ['+set', 'g_gametype', '14'],
  });
  for (const [b, r] of [['cart', cart], ['native', native]]) {
    for (const k of ['start', 'control', 'hit', 'generator', 'respawn', 'using', 'released', 'won', 'round2', 'decided']) {
      if (!r.valuesAt[k] || !Object.keys(r.valuesAt[k]).length) failures.push(`${b}: no values at ${k}`);
    }
  }
  if (failures.length) return { ok: false, failures, rows };
  check('cart', cart.valuesAt, rows, failures);
  check('native', native.valuesAt, rows, failures);
  return { ok: failures.length === 0, failures, rows };
}
