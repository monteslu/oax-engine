// Own-vehicle prediction and command-time controls (vehicle fixes after
// phase 8). The server applies a driver's commands by their command time
// with no added delay: its vehicle clock (16 ms ticks) never runs past the
// command time of a human in the game, and humans move it from their own
// ClientThink, so every tick sees the same inputs whenever packets arrive.
// The driving client runs the same vehicle in a Box3D world of its own
// (cg_oax_vehicle.c), restarts from every authoritative state a snapshot
// brings, replays its newer commands, and draws at its newest command time
// with corrections decaying away (cg_oaxVehErrorDecay), never snapping.
//
// On oax_vehicle_test the vehicle_drive pad script (get in, drive over the
// terrain and the ramp, turn, slide, brake, back up, get out) runs on the
// native client and on the cart, with prediction on and off. Asserted:
//   - the server's drive (hash and once-a-second checkpoints) is identical
//     native vs cart, and identical with prediction on and off on each
//     build (prediction cannot touch the server);
//   - no driver command arrives after the tick that uses it (late 0);
//   - with prediction on, on both builds: the predicted vehicle at each new
//     server state's time is within ERR_BOUND units of it, what was drawn
//     at each command time is within TRACK_BOUND units of the server's
//     vehicle at that time, corrections stay under CORR_BOUND, zero snaps;
// Control (must fail the bound): prediction off (cg_oaxVehPredict 0) draws
// the interpolated vehicle, which trails the driver's commands by the
// input lag: its tracking error at speed is many times the bound.
// Disturbance (cart): an impulse the client cannot foresee (vehkick, queued
// like an explosion's) knocks the driven buggy sideways mid-drive: the
// prediction is corrected (a correction over a unit), smoothly (no snap),
// and what is drawn comes back within the bound once the error decays.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { runNative } from '../lib/native.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';

export const name = 'vehicle-predict';

const MAP = 'oax_vehicle_test';
const RULE = 'set g_oaxVehicles 1';
const SCRIPT = 'vehicle_drive';
const ERR_BOUND = 2;		// units: prediction vs the server at the same tick
const TRACK_BOUND = 4;		// units: drawn vs the server at the same command time
const CORR_BOUND = 8;		// units: one correction
const LAG_MIN = 30;			// units: the control's tracking error must exceed this
const KICK_FRAME = 200;		// frames into the drive (full throttle by then)
const KICK = '0 300000 0';	// kg u/s sideways: about 330 u/s on the 900 kg buggy
const KICK_TRACK_BOUND = 48;	// units: drawn vs server around the kick

function scriptFrames(script) {
  const text = fs.readFileSync(path.join(path.dirname(new URL(import.meta.url).pathname), '..', 'data', 'padscripts', `${script}.pad`), 'utf8');
  return text.split('\n').filter((l) => l.trim() && !l.startsWith('#')).reduce((n, l) => n + Number(l.split(/\s+/)[0]), 0);
}

const checkpoints = (v) => Object.keys(v).filter((k) => /^g_veh_drive_\d+_t\d+$/.test(k)).sort((a, b) => Number(a.match(/_t(\d+)$/)[1]) - Number(b.match(/_t(\d+)$/)[1]));

async function cartDrive(tag, predict, kick) {
  const s = new Session(tag);
  try {
    await loadScene(s, MAP, { view: `${CLEAN_VIEW};${RULE}` });
    await s.command(`cg_oaxVehPredict ${predict}`);
    await s.step(200);
    await s.command(`padscript padscripts/${SCRIPT}.pad`);
    if (kick) {
      await s.step(KICK_FRAME);
      await s.command(`vehkick ${kick}`);
      await s.step(scriptFrames(SCRIPT) + 40 - KICK_FRAME);
    } else {
      await s.step(scriptFrames(SCRIPT) + 40);
    }
    return await readValues(s);
  } finally {
    await s.shutdown();
  }
}

function nativeDrive(tag, predict) {
  const home = runNative(tag, MAP, [
    'exec padscripts/padbinds.cfg', 'bot_enable 0', `cg_oaxVehPredict ${predict}`, 'wait 200',
    `padscript padscripts/${SCRIPT}.pad "wait 8; debugvalues ${tag}_values.txt; quit"`,
  ], { quit: false, set: { g_oaxVehicles: 1 } });
  const f = path.join(home, 'baseoa', `${tag}_values.txt`);
  return fs.existsSync(f) ? parseDebugValues(fs.readFileSync(f, 'utf8')) : null;
}

const n = (v) => Number(v ?? NaN);

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const runs = {};
  runs.cartOn = await cartDrive('vpredict-cart-on', 1);
  runs.cartOff = await cartDrive('vpredict-cart-off', 0);
  runs.nativeOn = nativeDrive('vpredict-native-on', 1);
  runs.nativeOff = nativeDrive('vpredict-native-off', 0);
  runs.cartKick = await cartDrive('vpredict-cart-kick', 1, KICK);
  fs.writeFileSync(path.join(out, 'vehicle-predict.json'), JSON.stringify(runs, null, 1));
  for (const [k, v] of Object.entries(runs)) if (!v) failures.push(`${k}: no debug values`);
  if (failures.length) return { ok: false, failures, rows };

  // the server's drive: native == cart, prediction on == off
  const keys = checkpoints(runs.nativeOn);
  if (keys.length < 6) failures.push(`only ${keys.length} drive checkpoints`);
  const last = keys.filter((k) => ['nativeOn', 'cartOn', 'cartOff', 'nativeOff'].every((r) => runs[r][k] !== undefined)).pop();
  const hash = (v) => (last && v[last] ? v[last].split(' ').pop() : null);
  for (const [a, b] of [['nativeOn', 'cartOn'], ['cartOn', 'cartOff'], ['nativeOn', 'nativeOff']]) {
    let same = 0;
    for (const k of keys) if (runs[a][k] === runs[b][k]) same++;
    rows.push(`server drive ${a} vs ${b}: hash at ${last} ${hash(runs[a])} / ${hash(runs[b])}, ${same}/${keys.length} checkpoints identical`);
    if (same !== keys.length || hash(runs[a]) !== hash(runs[b])) failures.push(`server drive differs between ${a} and ${b}`);
  }
  for (const [k, v] of Object.entries(runs)) {
    if (n(v.g_veh_late_cmds) !== 0) failures.push(`${k}: ${v.g_veh_late_cmds} late driver commands`);
  }
  rows.push(`driver commands: late ${['nativeOn', 'cartOn'].map((k) => runs[k].g_veh_late_cmds).join('/')}, ahead of the server ${['nativeOn', 'cartOn'].map((k) => runs[k].g_veh_ahead_cmds).join('/')}, ` +
    `ticks held for a command ${['nativeOn', 'cartOn'].map((k) => runs[k].g_veh_held_ticks).join('/')}, max command lag ${['nativeOn', 'cartOn'].map((k) => runs[k].g_veh_cmd_lag_max).join('/')} ms (native/cart)`);

  // the prediction
  for (const k of ['nativeOn', 'cartOn']) {
    const v = runs[k];
    rows.push(`${k}: prediction error at ${v.cg_vehpred_err_n} server states max ${v.cg_vehpred_err_max} mean ${v.cg_vehpred_err_mean}; ` +
      `corrections ${v.cg_vehpred_corr_n} (${v.cg_vehpred_corr_per_s}/s, max ${v.cg_vehpred_corr_max}, mean ${v.cg_vehpred_corr_mean}), snaps ${v.cg_vehpred_snaps}; ` +
      `drawn vs server at ${v.cg_vehpred_track_n} command times max ${v.cg_vehpred_track_max} mean ${v.cg_vehpred_track_mean}`);
    if (!(n(v.cg_vehpred_err_n) > 100)) failures.push(`${k}: only ${v.cg_vehpred_err_n} server states compared`);
    if (!(n(v.cg_vehpred_err_max) <= ERR_BOUND)) failures.push(`${k}: prediction error ${v.cg_vehpred_err_max} over ${ERR_BOUND}`);
    if (!(n(v.cg_vehpred_track_max) <= TRACK_BOUND)) failures.push(`${k}: drawn vehicle ${v.cg_vehpred_track_max} from the server's, over ${TRACK_BOUND}`);
    if (!(n(v.cg_vehpred_corr_max) <= CORR_BOUND)) failures.push(`${k}: a correction of ${v.cg_vehpred_corr_max} units`);
    if (n(v.cg_vehpred_snaps) !== 0) failures.push(`${k}: ${v.cg_vehpred_snaps} snaps`);
  }
  // control: interpolation trails the commands
  for (const k of ['nativeOff', 'cartOff']) {
    const v = runs[k];
    rows.push(`control ${k} (cg_oaxVehPredict 0): drawn vs server at ${v.cg_vehpred_track_n} command times max ${v.cg_vehpred_track_max} mean ${v.cg_vehpred_track_mean} ` +
      `(${n(v.cg_vehpred_track_max) > TRACK_BOUND ? 'fails the bound, as it must' : 'PASSES the bound'})`);
    if (!(n(v.cg_vehpred_track_max) > LAG_MIN)) failures.push(`control ${k}: interpolated vehicle only ${v.cg_vehpred_track_max} from the server's (want > ${LAG_MIN})`);
    if (!(n(v.cg_vehpred_track_n) > 100)) failures.push(`control ${k}: only ${v.cg_vehpred_track_n} command times compared`);
  }
  // the disturbance
  const k = runs.cartKick;
  rows.push(`cartKick (vehkick ${KICK} at frame ${KICK_FRAME}): impulses ${k.g_veh_impulses}, server drive hash ${hash(k)} (${hash(k) === hash(runs.cartOn) ? 'SAME as undisturbed' : 'differs from undisturbed'}); ` +
    `corrections ${k.cg_vehpred_corr_n} (max ${k.cg_vehpred_corr_max}, mean ${k.cg_vehpred_corr_mean}), snaps ${k.cg_vehpred_snaps}; prediction error max ${k.cg_vehpred_err_max}; ` +
    `drawn vs server max ${k.cg_vehpred_track_max} mean ${k.cg_vehpred_track_mean}`);
  if (n(k.g_veh_impulses) !== 1 || hash(k) === hash(runs.cartOn)) failures.push('disturbance: the kick did not reach the server\'s vehicle');
  if (!(n(k.cg_vehpred_corr_max) > 1)) failures.push(`disturbance: no real correction (max ${k.cg_vehpred_corr_max})`);
  if (n(k.cg_vehpred_snaps) !== 0) failures.push(`disturbance: ${k.cg_vehpred_snaps} snaps`);
  if (!(n(k.cg_vehpred_track_max) <= KICK_TRACK_BOUND)) failures.push(`disturbance: drawn vehicle ${k.cg_vehpred_track_max} from the server's`);
  if (!(n(k.cg_vehpred_track_mean) <= TRACK_BOUND)) failures.push(`disturbance: drawn vehicle ${k.cg_vehpred_track_mean} from the server's on average`);
  return { ok: failures.length === 0, failures, rows };
}
