// Navmesh bots (phase 7): on a map without AAS (oax_outdoor_ctf, terrain)
// the game's navmesh bots (g_oax_navbot.c) play a 2v2 CTF match. Asserts:
//   - the bots are driven by the navmesh brain (g_navbots), find paths and
//     reach goals,
//   - every item and both flags were touched, and at least one flag was
//     taken,
//   - nobody fell through the terrain (feet never in solid, nobody below
//     the lowest terrain surface),
//   - the match is the same on native and the cart: with a fixed game seed
//     (sv_gameSeed) the bot-origin hash checkpoints every 10 s of level
//     time are identical.
// Controls: another game seed must change the checkpoints; the hash must
// actually move during the match.
//
// OA_NAVBOT_FRAMES (default 15000 = 4 minutes at 16 ms; 2.5 minutes was too short for a 2v2 to always reach both flags) sets the length.

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';
import { nativeBinary, nativeHome, findBaseoa, execNative } from '../lib/native.mjs';
import { mapPath } from '../lib/scenes.mjs';
import { terrainFromBsp } from '../lib/terrain.mjs';
import { nativeCaptureArgs } from '../lib/capture.mjs';

export const timeoutSec = 2400;
export const name = 'nav-bots';
export const slow = true;

const MAP = 'oax_outdoor_ctf';
const FRAMES = Number(process.env.OA_NAVBOT_FRAMES || 15000);
const BOTS = [['Angelyss', 'red'], ['Major', 'red'], ['Sarge', 'blue'], ['Grism', 'blue']];

// one console line, run the same way by both builds: the map loads and the
// bots are added in the same command buffer, so they join at the same time
function matchLine(seed) {
  return [`set sv_gameSeed ${seed}`, 'bot_enable 1', 'g_gametype 4', 'g_doWarmup 0', 'timelimit 0', 'fraglimit 0', 'capturelimit 0',
    `devmap ${MAP}`, ...BOTS.map(([b, t]) => `addbot ${b} 4 ${t}`)].join(';');
}

function checkpoints(v) {
  return Object.fromEntries(Object.entries(v).filter(([k]) => /^g_navbot_hash_\d+$/.test(k)).map(([k, x]) => [Number(k.split('_').pop()), x]));
}

function runNativeMatch(seed) {
  const home = nativeHome('nav-bots');
  // a console `wait N` lasts about N/2 native frames. The native match draws
  // nothing (r_norefresh, a cheat cvar: set after devmap turns cheats on):
  // the test reads only server state, and drawing 15000 frames of terrain on
  // llvmpipe took ~13 of the 15 minutes native gets.
  fs.writeFileSync(path.join(home, 'baseoa', 'navbots.cfg'), ['fixedtime 16', matchLine(seed), 'r_norefresh 1', `wait ${FRAMES * 2}`, 'debugvalues navbots_native.txt', 'quit'].join('\n') + '\n');
  execNative([
    '+set', 'fs_basepath', path.dirname(findBaseoa()), '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
    ...nativeCaptureArgs({ width: 640, height: 360 }),
    '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0',
    '+set', 'com_introplayed', '1', '+set', 'com_maxfps', '0', '+exec', 'navbots.cfg',
  ], { home, timeout: 900000 });
  return parseDebugValues(fs.readFileSync(path.join(home, 'baseoa', 'navbots_native.txt'), 'utf8'));
}

async function runCartMatch(name, seed, frames) {
  const s = new Session(name);
  try {
    await s.load();
    await s.command(matchLine(seed));
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 4000, 20);
    for (let done = 0; done < frames; done += 750) await s.step(Math.min(750, frames - done));
    return await readValues(s);
  } finally {
    await s.shutdown();
  }
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const terrain = terrainFromBsp(fs.readFileSync(mapPath(MAP)))[0];
  let minSurface = Infinity;
  for (let j = 0; j < terrain.samplesY; j++) for (let i = 0; i < terrain.samplesX; i++) minSurface = Math.min(minSurface, terrain.sampleZ(i, j));

  const v = await runCartMatch('nav-bots', 7, FRAMES);
  const nv = runNativeMatch(7);
  fs.writeFileSync(path.join(out, 'nav-bots.json'), JSON.stringify({ cart: v, native: nv }, null, 1));

  rows.push(`${MAP} 2v2 CTF, ${Number(v.g_level_time) / 1000}s: navbots ${v.g_navbots}, paths ${v.g_navbot_paths} (no path ${v.g_navbot_nopath}), goals reached ${v.g_navbot_goals}, stuck ${v.g_navbot_stuck}`);
  rows.push(`items touched ${v.g_items_reached}/${v.g_items_total}, flags ${v.g_flags_reached}/${v.g_flags_total}, flags taken red ${v.g_ctf_red_taken} blue ${v.g_ctf_blue_taken}, captures red ${v.g_ctf_red_caps} blue ${v.g_ctf_blue_caps}`);
  rows.push(`feet in solid ${v.g_bots_in_solid} of ${v.g_bot_frames} bot frames, lowest bot z ${v.g_min_z} (lowest terrain surface ${minSurface.toFixed(1)})`);
  if (Number(v.g_navbots) !== BOTS.length) failures.push(`${v.g_navbots} navmesh bots, expected ${BOTS.length}`);
  if (!(Number(v.g_navbot_goals) > 0)) failures.push('bots reached no goal');
  if (!(Number(v.g_items_total) > 0)) failures.push('control: no items counted');
  if (v.g_items_reached !== v.g_items_total) failures.push(`items never touched: ${v.g_items_missing}`);
  if (!(Number(v.g_flags_total) === 2 && Number(v.g_flags_reached) === 2)) failures.push(`flags touched ${v.g_flags_reached}/${v.g_flags_total}`);
  if (!(Number(v.g_ctf_red_taken) + Number(v.g_ctf_blue_taken) > 0)) failures.push('no flag was taken');
  if (Number(v.g_bots_in_solid) !== 0) failures.push(`${v.g_bots_in_solid} bot frames with feet in solid`);
  if (v.g_min_z === 'none' || Number(v.g_min_z) < minSurface + 24 - 0.5) failures.push(`a bot went below the terrain (lowest z ${v.g_min_z})`);

  // identity
  const cc = checkpoints(v), nc = checkpoints(nv);
  const common = Object.keys(cc).filter((t) => nc[t] !== undefined).map(Number).sort((a, b) => a - b);
  const diff = common.filter((t) => cc[t] !== nc[t]);
  rows.push(`native vs cart: ${common.length} hash checkpoints in common (cart ${Object.keys(cc).length}, native ${Object.keys(nc).length}), ${diff.length} differ${diff.length ? ` (first at ${diff[0]}s: cart ${cc[diff[0]]} native ${nc[diff[0]]})` : ''}; native items ${nv.g_items_reached}/${nv.g_items_total}`);
  if (common.length < 5) failures.push(`only ${common.length} checkpoints in common`);
  if (diff.length) failures.push(`bot matches differ native vs cart from ${diff[0]}s`);
  const moved = new Set(common.map((t) => cc[t])).size;
  if (moved < common.length - 1) failures.push(`control: the bot hash did not move (${moved} distinct of ${common.length})`);

  // control: another seed must play differently
  const w = await runCartMatch('nav-bots-seed', 8, 1250);
  const wc = checkpoints(w);
  const same = Object.keys(wc).filter((t) => cc[t] !== undefined && Number(t) > 0 && wc[t] === cc[t]).length;
  rows.push(`control sv_gameSeed 8: ${same} of ${Object.keys(wc).length - 1} checkpoints after 0s match seed 7`);
  if (same > 0) failures.push('control: another game seed gave the same bot match');
  return { ok: failures.length === 0, failures, rows };
}
