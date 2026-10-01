// QVM interpreter performance: a bot match on the cart, every QVM on the
// interpreter (a cart can't generate code), timed per frame from the test
// side. The match must hold the frame budget, or the roadmap's build-time
// QVM translation becomes necessary. Frame time is published as a row so a
// slowdown shows up in the gate log even while it is under budget.

import { Session, CA_ACTIVE } from '../lib/romdev.mjs';

export const name = 'qvm-perf';

const BOTS = ['Angelyss', 'Arachna', 'Major', 'Sarge', 'Grism'];
const BUDGET_MS = Number(process.env.OA_FRAME_BUDGET_MS || 16.7);
const WARMUP = 300;     // bots join, pick goals, start fighting
const MEASURE = 600;    // ten seconds of match at 16 ms frames
const CHUNK = 60;

async function timeFrames(s, frames) {
  const per = [];
  for (let done = 0; done < frames; done += CHUNK) {
    const t = process.hrtime.bigint();
    await s.step(CHUNK);
    per.push(Number(process.hrtime.bigint() - t) / 1e6 / CHUNK);
  }
  per.sort((a, b) => a - b);
  return { mean: per.reduce((a, b) => a + b, 0) / per.length, worst: per[per.length - 1] };
}

async function scene(s, bots) {
  await s.load();
  await s.command(`bot_enable ${bots ? 1 : 0};g_doWarmup 0;cg_drawFPS 0;devmap oa_dm1`);
  await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
  for (const b of bots ? BOTS : []) {
    await s.command(`addbot ${b} 3`);
    await s.step(5);
  }
  await s.step(WARMUP);
}

export async function run() {
  const failures = [];
  const rows = [];
  const results = {};
  for (const bots of [false, true]) {
    const s = new Session(`qvm-perf-${bots ? 'bots' : 'empty'}`);
    try {
      await scene(s, bots);
      const ents = await s.read('server_entities');
      const r = await timeFrames(s, MEASURE);
      results[bots ? 'bots' : 'empty'] = r;
      rows.push(`${bots ? `${BOTS.length} bots` : 'no bots'}: ${r.mean.toFixed(2)} ms/frame mean, worst ${CHUNK}-frame chunk ${r.worst.toFixed(2)} ms/frame, ${ents} server entities`);
      // the bots must actually be in the game, or this times an empty map
      if (bots && ents < BOTS.length + 1) failures.push(`only ${ents} server entities; the bots did not join`);
    } finally {
      await s.shutdown();
    }
  }
  const { bots, empty } = results;
  if (bots && empty) {
    rows.push(`bot and QVM cost: ${(bots.mean - empty.mean).toFixed(2)} ms/frame`);
    if (bots.mean > BUDGET_MS) failures.push(`bot match runs ${bots.mean.toFixed(2)} ms/frame, over the ${BUDGET_MS} ms budget`);
  }
  return { ok: failures.length === 0, failures, rows };
}
