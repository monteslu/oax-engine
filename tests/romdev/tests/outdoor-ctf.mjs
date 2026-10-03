// Outdoor CTF with bots (the phase 7 exit): oax_outdoor_ctf on the cart, a
// 3v3 bot CTF match on heightmap terrain with no AAS, so every bot
// navigates on the Recast/Detour navmesh. Asserts:
//   - every item and both flags were touched by a bot (g_oax_stats.c),
//   - both flags were taken at least once (captures reported),
//   - nobody fell through the terrain: no bot frame with its feet inside
//     solid, and no bot lower than the lowest terrain surface,
//   - frame time with the whole field in view (terrain, foliage, sun
//     shadows, six bots), measured by the host around frame steps.
// Controls: bots that are not allowed the navmesh (bot_oaxNav 0: straight
// steering, no stuck recovery) must reach fewer items than the full match
// and take no flag, and the feet-in-solid counter must see a player pushed
// under the terrain.
//
// OA_CTF_FRAMES (default 15000 = 4 minutes at 16 ms) sets the match length.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { readValues } from '../lib/values.mjs';
import { mapPath, CLEAN_VIEW } from '../lib/scenes.mjs';
import { terrainFromBsp } from '../lib/terrain.mjs';

export const timeoutSec = 2400;
export const name = 'outdoor-ctf';

const MAP = 'oax_outdoor_ctf';
const MATCH_FRAMES = Number(process.env.OA_CTF_FRAMES || 15000);
const CONTROL_FRAMES = 3000;
const BATCH = 750;
const BOTS = { red: ['Angelyss', 'Arachna', 'Major'], blue: ['Sarge', 'Grism', 'Kyonshi'] };
// a view over the field from above the blue fort: terrain, groves, both forts
const CAMERA = '-3000 -1300 520 14 25 0';

async function boot(s, extra = '') {
  await s.load();
  await s.command(`${CLEAN_VIEW};bot_enable 1;g_gametype 4;g_doWarmup 0;timelimit 0;fraglimit 0;capturelimit 0;${extra}${extra ? ';' : ''}devmap ${MAP}`);
  await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 4000, 20);
  await s.command(CLEAN_VIEW);
  await s.step(30);
  const mapname = await s.read('mapname');
  if (mapname !== MAP) throw new Error(`loaded ${mapname}, not ${MAP}`);
}

async function addBots(s) {
  await s.command('team spectator');
  await s.step(10);
  for (const t of ['red', 'blue']) {
    for (const b of BOTS[t]) {
      await s.command(`addbot ${b} 4 ${t}`);
      await s.step(5);
    }
  }
}

function p95(xs) {
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor(s.length * 0.95))];
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const terrain = terrainFromBsp(fs.readFileSync(mapPath(MAP)))[0];
  let minSurface = Infinity;
  for (let j = 0; j < terrain.samplesY; j++) for (let i = 0; i < terrain.samplesX; i++) minSurface = Math.min(minSurface, terrain.sampleZ(i, j));

  let mainReached = Infinity;
  const s = new Session('outdoor-ctf');
  try {
    await boot(s);
    await addBots(s);
    const v0 = await readValues(s);
    rows.push(`navmesh: ${v0.sv_nav_polys ?? '?'} polys from ${v0.sv_nav_tris ?? '?'} triangles, hash ${v0.sv_nav_hash ?? '?'}; navbots ${v0.g_navbots ?? '?'}`);
    await s.command(`cl_overrideView "${CAMERA}"`);
    // frame time: host time per stepped frame, batches ending in a screenshot
    // (a pixel read waits for the GPU); a screenshot's own cost subtracted
    const shot = path.join(out, 'outdoor-ctf.png');
    const shotCosts = [];
    for (let i = 0; i < 5; i++) { const t0 = performance.now(); await s.screenshot(shot); shotCosts.push(performance.now() - t0); }
    shotCosts.sort((a, b) => a - b);
    const times = [];
    for (let done = 0; done < MATCH_FRAMES; done += BATCH) {
      const t0 = performance.now();
      await s.step(BATCH);
      await s.screenshot(shot);
      times.push((performance.now() - t0 - shotCosts[2]) / BATCH);
    }
    const v = await readValues(s);
    const total = Number(v.g_items_total), reached = Number(v.g_items_reached);
    mainReached = reached;
    const minZ = v.g_min_z === 'none' ? null : Number(v.g_min_z);
    const mean = times.reduce((a, b) => a + b, 0) / times.length;
    rows.push(`${MATCH_FRAMES} frames (${Number(v.g_level_time) / 1000}s of play), 3v3 bots: items touched ${reached}/${total}, flags touched ${v.g_flags_reached}/${v.g_flags_total}`);
    rows.push(`flags taken: red ${v.g_ctf_red_taken}, blue ${v.g_ctf_blue_taken}; captures: red ${v.g_ctf_red_caps}, blue ${v.g_ctf_blue_caps}`);
    rows.push(`navbot paths ${v.g_navbot_paths}, goals reached ${v.g_navbot_goals}, stuck detections ${v.g_nav_stuck}`);
    rows.push(`bot frames ${v.g_bot_frames}, feet in solid ${v.g_bots_in_solid}, lowest bot z ${minZ} (lowest terrain surface ${minSurface.toFixed(1)}, bottom ${terrain.bottom})`);
    rows.push(`frame time over the field with 6 bots: mean ${mean.toFixed(2)} ms, p95 ${p95(times).toFixed(2)} ms per frame (${times.length} batches of ${BATCH})`);
    if (v.g_items_missing) rows.push(`items never touched: ${v.g_items_missing || 'none'}`);
    if (!(total > 0)) failures.push('control: no items counted (stats not running?)');
    if (reached !== total) failures.push(`bots touched ${reached} of ${total} items: ${v.g_items_missing}`);
    if (!(Number(v.g_flags_total) === 2 && Number(v.g_flags_reached) === 2)) failures.push(`flags touched ${v.g_flags_reached}/${v.g_flags_total}`);
    if (!(Number(v.g_ctf_red_taken) > 0 && Number(v.g_ctf_blue_taken) > 0)) failures.push(`both flags must be taken (red ${v.g_ctf_red_taken}, blue ${v.g_ctf_blue_taken})`);
    if (Number(v.g_bots_in_solid) !== 0) failures.push(`${v.g_bots_in_solid} bot frames had feet inside solid`);
    if (!(Number(v.g_bot_frames) > MATCH_FRAMES)) failures.push(`only ${v.g_bot_frames} living bot frames`);
    if (minZ === null) failures.push('no living bot recorded');
    else if (minZ < minSurface + 24 - 0.5) failures.push(`a bot reached z ${minZ}, below the lowest terrain surface ${minSurface.toFixed(1)} + 24`);
    fs.writeFileSync(path.join(out, 'outdoor-ctf.json'), JSON.stringify({ values: v, times }, null, 1));
  } finally {
    await s.shutdown();
  }

  // control 1: bots without the navmesh do not get around
  const c = new Session('outdoor-ctf-nonav');
  try {
    await boot(c, 'set bot_oaxNav 0');
    await addBots(c);
    for (let done = 0; done < CONTROL_FRAMES; done += BATCH) await c.step(BATCH);
    const v = await readValues(c);
    rows.push(`control bot_oaxNav 0, ${CONTROL_FRAMES} frames: items touched ${v.g_items_reached}/${v.g_items_total}, flags taken red ${v.g_ctf_red_taken} blue ${v.g_ctf_blue_taken}`);
    if (!(Number(v.g_items_reached) < mainReached) || Number(v.g_ctf_red_taken) + Number(v.g_ctf_blue_taken) > 0) {
      failures.push(`control: bots without the navmesh reached ${v.g_items_reached} items (full match ${mainReached}) or took a flag`);
    }
    // control 2: the feet-in-solid counter must count: noclip a bot-free
    // spectator is not counted, so push the terrain off for collision and
    // let the bots fall into it
    await c.command('set bot_oaxNav 1; cm_noTerrain 1');
    await c.step(120);
    await c.command('cm_noTerrain 0');
    await c.step(60);
    const w = await readValues(c);
    rows.push(`control cm_noTerrain 1 for 120 frames: feet in solid ${w.g_bots_in_solid}, lowest bot z ${w.g_min_z}`);
    if (!(Number(w.g_bots_in_solid) > 0) && !(Number(w.g_min_z) < minSurface)) failures.push('control: bots fell through a disabled terrain but nothing counted it');
  } finally {
    await c.shutdown();
  }
  return { ok: failures.length === 0, failures, rows };
}
