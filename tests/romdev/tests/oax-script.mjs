// Map scripting (the id Tech 4 script VM in the engine, pumped by the oax
// game module), on the cart and the native build, with the oax_script map:
//
// - The script event log (time, event, self, thread rows; hashed by the
//   engine) after every thread finished is identical run to run on the
//   cart and identical native vs cart.
// - Control: a run where the player walks into the trigger that calls
//   player_entered() must log differently.
// - The deliberate runaway thread is killed at the same instruction on
//   both builds (g_script_runaway).
// - The script did what it says: the counter fired once (3 of 3), the
//   pillar was hidden, waitFor resumed, no compile or runtime errors.
// - Per-frame script cost is published (g_script_instr, g_script_us and
//   their worst-frame maxima). With 50 threads that each call an engine
//   and a game event every frame (oax_script_perf), the cart's frame time
//   grows by less than OA_SCRIPT_BUDGET_MS (0.5) against the same map with
//   g_oaxScripts 0, and native RUN stays under 500 us per frame.
// - A script that does not compile (oax_script_err) is reported with file
//   and line, and the map plays on without scripting.
// - Control: a map without a script starts no VM.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues, readValues } from '../lib/values.mjs';
import { PLAYER_TRIGGER } from '../../maps/src/oax_script.mjs';

export const name = 'oax-script';

const END_TIME = 12500;
const BUDGET_MS = Number(process.env.OA_SCRIPT_BUDGET_MS || 0.5);	// every thread is done by then (the last event is at ~11.2 s)

async function cartRun(label, { map = 'oax_script', touch = false } = {}) {
  const s = new Session(`oax-script-${label}`);
  try {
    await s.load();
    await s.command(`bot_enable 0; devmap ${map}`);
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
    if (map !== 'oax_script') {
      await s.step(60);
      return await readValues(s);
    }
    if (touch) {
      await s.stepUntil('debug_values', (t) => Number(parseDebugValues(t).g_script_time) >= 1000, 2000, 10);
      const c = PLAYER_TRIGGER.center;
      await s.command(`setviewpos ${c[0]} ${c[1]} ${c[2] - 24} 0`);
    }
    await s.stepUntil('debug_values', (t) => Number(parseDebugValues(t).g_script_time) >= END_TIME, 3000, 25);
    return await readValues(s);
  } finally {
    await s.shutdown();
  }
}

async function cartCost(scripts) {
  const s = new Session(`oax-script-cost-${scripts}`);
  try {
    await s.load();
    await s.command(`bot_enable 0; set g_oaxScripts ${scripts}; cg_drawFPS 0; devmap oax_script_perf`);
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
    await s.step(120);
    const per = [];
    for (let i = 0; i < 10; i++) {
      const t = process.hrtime.bigint();
      await s.step(60);
      per.push(Number(process.hrtime.bigint() - t) / 1e6 / 60);
    }
    per.sort((x, y) => x - y);
    // the middle six chunks: host hiccups land in the tails
    const mid = per.slice(2, 8);
    return { ms: mid.reduce((x, y) => x + y, 0) / mid.length, values: await readValues(s) };
  } finally {
    await s.shutdown();
  }
}

function nativeRun() {
  const home = runNative('oax-script', 'oax_script', ['bot_enable 0', 'wait 1800', 'debugvalues values.txt', 'scriptlog scriptlog.txt']);
  const dir = path.join(home, 'baseoa');
  const read = (f) => (fs.existsSync(path.join(dir, f)) ? fs.readFileSync(path.join(dir, f), 'utf8') : '');
  return { values: parseDebugValues(read('values.txt')), log: read('scriptlog.txt') };
}

function nativeOther(map, lines) {
  const home = runNative(`oax-script-${map}`, map, ['bot_enable 0', ...lines, 'debugvalues values.txt']);
  const f = path.join(home, 'baseoa', 'values.txt');
  return parseDebugValues(fs.existsSync(f) ? fs.readFileSync(f, 'utf8') : '');
}

const row = (v) => `log ${v.g_script_log_count} rows hash ${v.g_script_log_hash}, last "${v.g_script_log_last}"`;

export async function run({ out }) {
  const failures = [];
  const rows = [];

  const a = await cartRun('a');
  const b = await cartRun('b');
  const touched = await cartRun('touch', { touch: true });
  const box = await cartRun('box', { map: 'oax_box' });
  const native = nativeRun();
  const err = await cartRun('err', { map: 'oax_script_err' });
  const nativeErr = nativeOther('oax_script_err', ['wait 100']);
  const nativePerf = nativeOther('oax_script_perf', ['wait 600']);
  const costOn = await cartCost(1);
  const costOff = await cartCost(0);
  const n = native.values;
  fs.writeFileSync(path.join(out, 'oax_script.native.log'), native.log);

  rows.push(`cart run 1: ${row(a)}`);
  rows.push(`cart run 2: ${row(b)}`);
  rows.push(`native:     ${row(n)} (at ${n.g_script_time} ms)`);
  rows.push(`touch:      ${row(touched)}`);
  rows.push(`runaway: cart "${a.g_script_runaway}" native "${n.g_script_runaway}"`);
  rows.push(`setkeyval "${a.g_script_setkey}", shaderparm0 "${a.g_shaderparm_0}"`);
  rows.push(`counter ${a.g_count_counter}; errors cart ${a.g_script_errors} native ${n.g_script_errors}; entities bound ${a.g_script_bound}`);
  rows.push(`script cost at the end: ${a.g_script_instr} instructions, ${a.g_script_us} us (cart), ${n.g_script_us} us (native)`);
  rows.push(`compile error: cart "${err.g_script_error}" (active ${err.g_script_active}); native "${nativeErr.g_script_error}"`);
  rows.push(`50 threads: cart ${costOn.ms.toFixed(3)} ms/frame vs ${costOff.ms.toFixed(3)} without scripts (+${(costOn.ms - costOff.ms).toFixed(3)}, budget ${BUDGET_MS}); ${costOn.values.g_script_threads} threads, worst frame ${costOn.values.g_script_instr_max} instructions ${costOn.values.g_script_calls_max} game calls`);
  rows.push(`50 threads native: worst frame ${nativePerf.g_script_us_max} us, ${nativePerf.g_script_instr_max} instructions, ${nativePerf.g_script_calls_max} game calls, ${nativePerf.g_script_threads} threads`);
  rows.push(`oax_box (no script): active ${box.g_script_active}, log ${box.g_script_log_count ?? 'none'}`);

  for (const [label, v] of [['cart', a], ['native', n]]) {
    if (v.g_script_active !== '1') failures.push(`${label}: scripts not active (${v.g_script_active})`);
    if (v.g_script_errors !== '0') failures.push(`${label}: script errors: ${v.g_script_error}`);
    if (Number(v.g_script_time) < END_TIME) failures.push(`${label}: stopped at ${v.g_script_time} ms`);
    if (!(Number(v.g_script_log_count) > 20)) failures.push(`${label}: only ${v.g_script_log_count} log rows`);
    if (v.g_script_instr === undefined || v.g_script_us === undefined) failures.push(`${label}: per-frame script cost not published`);
  }
  if (a.g_script_log_hash !== b.g_script_log_hash || a.g_script_log_count !== b.g_script_log_count) failures.push('cart: two runs logged differently');
  if (a.g_script_log_hash !== n.g_script_log_hash || a.g_script_log_count !== n.g_script_log_count) failures.push('native and cart logged differently');
  if (touched.g_script_log_hash === a.g_script_log_hash) failures.push('control: walking into the trigger did not change the log');
  if (!/player_entered|rotateOnce/.test(touched.g_script_log_last || '') && Number(touched.g_script_log_count) <= Number(a.g_script_log_count)) failures.push('control: the touch run logged no extra events');
  if (!a.g_script_runaway || a.g_script_runaway !== n.g_script_runaway) failures.push(`runaway thread: cart "${a.g_script_runaway}" vs native "${n.g_script_runaway}"`);
  if (!/after 100001$/.test(a.g_script_runaway || '')) failures.push(`runaway thread not killed at the limit: "${a.g_script_runaway}"`);
  if (a.g_count_counter !== '3 -1') failures.push(`counter: ${a.g_count_counter} (want "3 -1")`);
  if (!/^\d+ origin 0 -128 128$/.test(a.g_script_setkey || '')) failures.push(`target_oax_setkeyval did not set the key: "${a.g_script_setkey}"`);
  if (!/^\d+ 0\.5000$/.test(a.g_shaderparm_0 || '')) failures.push(`target_oax_shaderparm did not set parm 0: "${a.g_shaderparm_0}"`);
  if (!/ trigger /.test(` ${a.g_script_log_last} `)) failures.push(`the last event should be count_done's trigger: "${a.g_script_log_last}"`);
  if (box.g_script_active === '1' || box.g_script_log_count !== undefined) failures.push('control: a map without a script started the VM');
  for (const [label, v] of [['cart', err], ['native', nativeErr]]) {
    if (v.g_script_active !== '0' || !/maps\/oax_script_err\.script, line 6/.test(v.g_script_error || '')) failures.push(`${label}: the compile error was not reported as expected: "${v.g_script_error}" (active ${v.g_script_active})`);
  }
  if (costOn.values.g_script_threads !== '50' || Number(costOn.values.g_script_calls_max) < 50) failures.push(`cost: expected 50 busy threads, got ${costOn.values.g_script_threads} threads, ${costOn.values.g_script_calls_max} calls`);
  if (costOff.values.g_script_active !== '0') failures.push('cost control: g_oaxScripts 0 still started the VM');
  if (costOn.ms - costOff.ms > BUDGET_MS) failures.push(`cost: 50 threads add ${(costOn.ms - costOff.ms).toFixed(3)} ms per frame on the cart (budget ${BUDGET_MS})`);
  if (!(Number(nativePerf.g_script_us_max) < 500) || nativePerf.g_script_threads !== '50') failures.push(`native cost: worst frame ${nativePerf.g_script_us_max} us with ${nativePerf.g_script_threads} threads`);

  // the native log, row by row, for the record
  const lines = native.log.trim().split('\n');
  if (!lines.some((l) => / <runaway> /.test(l))) failures.push('native log has no runaway row');
  if (!lines.some((l) => /<script_setcallback>/.test(l))) failures.push('native log has no waitFor callback');
  return { ok: failures.length === 0, failures, rows };
}
