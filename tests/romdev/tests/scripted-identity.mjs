// Phase 4 exit: a scripted map with movers, triggers and a GUI plays
// identically on the native build and the cart.
//
// oax_scripted (tests/maps/src/oax_scripted.mjs): the player stands at the
// door panel aimed at LOCK and fires (a pad script, so both builds get the
// same input on the same command clock). The click is a GUI event; the
// GUI runs the map script oax_gui_lock(), which drives a keyframed mover up
// and down and fires a counter trigger twice, whose count calls
// count_done() to hide a marker. Compared, native vs cart:
//   - the GUI log (focus, click position, commands, state sets),
//   - the script event log, hashed with times relative to its first row
//     (the click lands on a server frame whose absolute time depends on the
//     build's frame phase; everything after it must match),
//   - the oax mover trace hash (each mover timed from its own first move),
//   - the end state: GUI state, counter, lift position.
// Controls: aiming at OPEN instead must give a different GUI log and no
// script at all; and the script must really have run (counter reached 2,
// lift back down, both legs logged).

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues, readValues } from '../lib/values.mjs';

export const name = 'scripted-identity';

const MAP = 'oax_scripted';
const AIM_LOCK = '-460 28 24 180';
const AIM_OPEN = '-460 -28 24 180';
const SETTLE_FRAMES = 120;
// the script takes about 3 s, but the mover trace hash samples each mover
// until 21 s after its first move: read after that window has closed
const AFTER_FRAMES = 1500;

function guiLog(v) {
  const n = Number(v.g_guilog_n || 0);
  return Array.from({ length: n }, (_, i) => v[`g_guilog_${i}`]).join(' | ');
}

function summary(v) {
  return {
    gui: guiLog(v),
    state: v.g_gui0_state,
    scriptRows: v.g_script_log_count,
    scriptRel: v.g_script_log_rhash,
    scriptLast: v.g_script_log_last,
    mover: v.g_mover_hash,
    moverFrames: v.g_mover_frames,
    errors: v.g_script_errors,
  };
}

async function cartRun(aim) {
  const s = new Session(`scripted-${aim === AIM_LOCK ? 'lock' : 'open'}`);
  try {
    await s.load();
    await s.command(`exec padscripts/padbinds.cfg; bot_enable 0; g_doWarmup 0; devmap ${MAP}`);
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
    await s.step(30);
    await s.command(`setviewpos ${aim}`);
    await s.step(SETTLE_FRAMES);
    await s.command('padscript padscripts/fire.pad');
    await s.step(AFTER_FRAMES);
    return await readValues(s);
  } finally {
    await s.shutdown();
  }
}

function nativeRun(aim) {
  const home = runNative(`scripted-${aim === AIM_LOCK ? 'lock' : 'open'}`, MAP, [
    'exec padscripts/padbinds.cfg', 'bot_enable 0', 'g_doWarmup 0', 'wait 60',
    `setviewpos ${aim}`, 'wait 240',
    // nothing may follow the padscript in the buffer (cl_testscript.c)
    `padscript padscripts/fire.pad "wait ${AFTER_FRAMES * 2}; debugvalues values.txt; quit"`,
  ], { quit: false });
  const f = path.join(home, 'baseoa', 'values.txt');
  return fs.existsSync(f) ? parseDebugValues(fs.readFileSync(f, 'utf8')) : {};
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const cart = summary(await cartRun(AIM_LOCK));
  const native = summary(nativeRun(AIM_LOCK));
  const control = summary(await cartRun(AIM_OPEN));
  fs.writeFileSync(path.join(out, 'scripted-identity.json'), JSON.stringify({ cart, native, control }, null, 2));

  for (const [label, r] of [['cart', cart], ['native', native]]) {
    rows.push(`${label}: gui "${r.gui}"`);
    rows.push(`${label}: state ${r.state}, script ${r.scriptRows} rows rel-hash ${r.scriptRel} last "${r.scriptLast}", movers ${r.mover} over ${r.moverFrames} frames, errors ${r.errors}`);
  }
  rows.push(`control (OPEN): gui "${control.gui}", script rows ${control.scriptRows ?? 0}`);

  // the script really ran: both lift legs and both counter fires
  if (cart.state !== '2') failures.push(`cart: GUI state ${cart.state}, LOCK was not pressed`);
  if (!(Number(cart.scriptRows) >= 6)) failures.push(`cart: only ${cart.scriptRows} script log rows`);
  if (cart.errors && cart.errors !== '0') failures.push(`cart: script errors ${cart.errors}`);
  if (!(Number(cart.moverFrames) > 0)) failures.push('cart: the lift never moved');
  // identity
  for (const k of ['gui', 'state', 'scriptRows', 'scriptRel', 'mover', 'moverFrames']) {
    if (cart[k] !== native[k]) failures.push(`native and cart differ in ${k}: "${native[k]}" vs "${cart[k]}"`);
  }
  // control: a different press is a different game
  if (control.gui === cart.gui) failures.push('control: pressing OPEN gave the same GUI log');
  if (Number(control.scriptRows || 0) > 0) failures.push('control: OPEN ran the map script');
  return { ok: failures.length === 0, failures, rows };
}
