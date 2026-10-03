// Vehicle CTF with bots (the phase 8 exit: "the vehicle mode plays locally
// with bots"). oax_outdoor_vctf on the cart in the vehicle mode (g_gametype
// 4 CTF + the rule g_oaxVehicles 1): two buggies and a hover craft per team
// spawn in front of the forts; a 3v3 bot match runs for several minutes on
// the navmesh. The server simulates every vehicle in its Box3D world
// (raycast wheels and hover thrusters over the BSP and the terrain height
// field). Asserts:
//   - bots get into vehicles and drive them (distance driven by bots),
//   - flags are taken and captured,
//   - vehicles are destroyed and their spawners bring new ones,
//   - nobody falls through the world: no bot frame with its feet in solid
//     or under the lowest terrain surface, no vehicle frame with its center
//     in solid, none below the kill height;
//   - frame time and the physics step time over the match.
// Published: vehicle counts, distance driven, physics ticks and step time.
// Controls: with the rule off the same map has no vehicles and no bot
// drives; with the terrain left out of the physics world
// (g_oaxVehNoTerrain 1) the in-solid and fell counters do count.
//
// OA_VCTF_FRAMES (default 15000 = 4 minutes at 16 ms) sets the match length.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { readValues } from '../lib/values.mjs';
import { mapPath, CLEAN_VIEW } from '../lib/scenes.mjs';
import { terrainFromBsp } from '../lib/terrain.mjs';

export const timeoutSec = 2400;
export const name = 'vehicle-ctf';

const MAP = 'oax_outdoor_vctf';
const MATCH_FRAMES = Number(process.env.OA_VCTF_FRAMES || 15000);
const CONTROL_FRAMES = 2250;
const BATCH = 750;
const BOTS = { red: ['Angelyss', 'Arachna', 'Major'], blue: ['Sarge', 'Grism', 'Kyonshi'] };
// above the red fort looking down the field
const CAMERA = '2900 -900 420 18 160 0';

async function boot(s, rule) {
  await s.load();
  // one console line holds 256 bytes: the rules first, then the map
  await s.command(`${CLEAN_VIEW};bot_enable 1;set g_gametype 4;g_doWarmup 0;timelimit 0;fraglimit 0;capturelimit 0`);
  await s.step(1);
  await s.command(`set sv_gameSeed 8;${rule};devmap ${MAP}`);
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

const n = (x) => Number(x ?? 0);

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const terrain = terrainFromBsp(fs.readFileSync(mapPath(MAP)))[0];
  let minSurface = Infinity;
  for (let j = 0; j < terrain.samplesY; j++) for (let i = 0; i < terrain.samplesX; i++) minSurface = Math.min(minSurface, terrain.sampleZ(i, j));

  const s = new Session('vehicle-ctf');
  try {
    await boot(s, 'set g_oaxVehicles 1');
    await addBots(s);
    const v0 = await readValues(s);
    rows.push(`vehicle mode: ${v0.g_veh_spawners} spawners, ${v0.g_veh_count} vehicles, physics bodies ${v0.g_veh_phys_bodies} (terrain ${v0.g_veh_terrain_bodies}); navmesh ${v0.sv_nav_polys ?? '?'} polys; navbots ${v0.g_navbots ?? '?'}`);
    if (n(v0.g_veh_spawners) !== 6 || n(v0.g_veh_count) !== 6) failures.push(`expected 6 spawners and 6 vehicles, got ${v0.g_veh_spawners} and ${v0.g_veh_count}`);
    await s.command(`cl_overrideView "${CAMERA}"`);
    const shot = path.join(out, 'vehicle-ctf.png');
    const shotCosts = [];
    for (let i = 0; i < 5; i++) { const t0 = performance.now(); await s.screenshot(shot); shotCosts.push(performance.now() - t0); }
    shotCosts.sort((a, b) => a - b);
    const times = [], physMs = [];
    let shots = 0;
    for (let done = 0; done < MATCH_FRAMES; done += BATCH) {
      const t0 = performance.now();
      await s.step(BATCH);
      await s.screenshot(shot);
      times.push((performance.now() - t0 - shotCosts[2]) / BATCH);
      const vv = await readValues(s);
      physMs.push(n(vv.g_veh_phys_ms));
      // a few views of the match while vehicles are out
      if (shots < 3 && n(vv.g_veh_driven) > 0) fs.copyFileSync(shot, path.join(out, `vehicle-ctf-${shots++}.png`));
    }
    const v = await readValues(s);
    const minZ = v.g_min_z === 'none' ? null : n(v.g_min_z);
    const mean = times.reduce((a, b) => a + b, 0) / times.length;
    rows.push(`${MATCH_FRAMES} frames (${n(v.g_level_time) / 1000}s of play), 3v3 bots in vehicle CTF`);
    rows.push(`vehicles: spawned ${v.g_veh_spawned} (${v.g_veh_spawners} spawners), destroyed ${v.g_veh_destroyed}, sent home ${v.g_veh_resets}, fell ${v.g_veh_fell}; ` +
      `bot drives ${v.g_vehbot_drives} (enters ${v.g_veh_bot_enters}, with a flag ${v.g_vehbot_flag_drives}), exits at goal ${v.g_vehbot_exit_goal} / wrecked ${v.g_vehbot_exit_wreck} / stuck ${v.g_vehbot_exit_stuck}; gunner frames ${v.g_veh_gunner_frames}; roadkills ${v.g_veh_roadkills}`);
    rows.push(`distance driven by bots ${n(v.g_veh_bot_distance).toFixed(0)} units over ${v.g_veh_bot_driven_frames} driven vehicle frames; vehicle frames ${v.g_veh_frames}`);
    rows.push(`flags taken: red ${v.g_ctf_red_taken}, blue ${v.g_ctf_blue_taken}; captures: red ${v.g_ctf_red_caps}, blue ${v.g_ctf_blue_caps}`);
    rows.push(`bot frames ${v.g_bot_frames}, feet in solid ${v.g_bots_in_solid}, lowest bot z ${minZ} (lowest terrain surface ${minSurface.toFixed(1)}); vehicle frames in solid ${v.g_veh_in_solid}, lowest vehicle center z ${v.g_veh_min_z}`);
    rows.push(`physics: ${v.g_veh_phys_ticks} ticks, ${v.g_veh_phys_bodies} bodies, step ${Math.max(...physMs).toFixed(3)} ms max of the samples (engine phys_game_step_ms ${v.phys_game_step_ms}); late driver commands ${v.g_veh_late_cmds}`);
    rows.push(`frame time over the field: mean ${mean.toFixed(2)} ms, p95 ${p95(times).toFixed(2)} ms per frame (${times.length} batches of ${BATCH})`);
    fs.writeFileSync(path.join(out, 'vehicle-ctf.json'), JSON.stringify({ values: v, times, physMs }, null, 1));

    if (!(n(v.g_veh_bot_enters) >= 3)) failures.push(`bots got into vehicles only ${v.g_veh_bot_enters} times`);
    if (!(n(v.g_veh_bot_distance) > 20000)) failures.push(`bots drove only ${v.g_veh_bot_distance} units`);
    if (!(n(v.g_ctf_red_taken) > 0 && n(v.g_ctf_blue_taken) > 0)) failures.push(`both flags must be taken (red ${v.g_ctf_red_taken}, blue ${v.g_ctf_blue_taken})`);
    if (!(n(v.g_ctf_red_caps) + n(v.g_ctf_blue_caps) > 0)) failures.push('no flag was captured');
    if (!(n(v.g_veh_destroyed) > 0)) failures.push('no vehicle was destroyed');
    if (!(n(v.g_veh_spawned) > n(v.g_veh_spawners))) failures.push(`no vehicle respawned (spawned ${v.g_veh_spawned} from ${v.g_veh_spawners} spawners)`);
    if (n(v.g_bots_in_solid) !== 0) failures.push(`${v.g_bots_in_solid} bot frames had feet inside solid`);
    if (minZ === null) failures.push('no living bot recorded');
    else if (minZ < minSurface + 24 - 0.5) failures.push(`a bot reached z ${minZ}, below the lowest terrain surface ${minSurface.toFixed(1)} + 24`);
    if (n(v.g_veh_in_solid) !== 0) failures.push(`${v.g_veh_in_solid} vehicle frames had the center inside solid`);
    if (n(v.g_veh_fell) !== 0) failures.push(`${v.g_veh_fell} vehicles fell out of the world`);
    if (n(v.g_veh_min_z) < minSurface) failures.push(`a vehicle center went down to ${v.g_veh_min_z}, under the lowest terrain surface`);
    if (n(v.g_veh_late_cmds) !== 0) failures.push(`${v.g_veh_late_cmds} driver commands arrived after the frame that should use them`);
  } finally {
    await s.shutdown();
  }

  // control 1: classic play on the same map (the rule off): no vehicles at all
  const c = new Session('vehicle-ctf-classic');
  try {
    await boot(c, 'set g_oaxVehicles 0');
    await addBots(c);
    for (let done = 0; done < CONTROL_FRAMES; done += BATCH) await c.step(BATCH);
    const v = await readValues(c);
    rows.push(`control g_oaxVehicles 0, ${CONTROL_FRAMES} frames: vehicles ${v.g_veh_count ?? 'none'}, bot vehicle distance ${v.g_veh_bot_distance ?? 'none'}; navbots ${v.g_navbots}, flags taken red ${v.g_ctf_red_taken} blue ${v.g_ctf_blue_taken}`);
    if (v.g_veh_count !== undefined || v.g_veh_bot_distance !== undefined) failures.push('control: vehicles with the vehicle rule off');
    if (!(n(v.g_navbots) === 6)) failures.push(`control: ${v.g_navbots} navbots in the classic match`);
  } finally {
    await c.shutdown();
  }
  // control 2: without the terrain in the physics world the vehicles sink
  // through it, and the counters must see that
  const d = new Session('vehicle-ctf-noterrain');
  try {
    await boot(d, 'set g_oaxVehicles 1;set g_oaxVehNoTerrain 1');
    await d.step(300);
    const v = await readValues(d);
    rows.push(`control g_oaxVehNoTerrain 1: vehicle frames in solid ${v.g_veh_in_solid}, fell ${v.g_veh_fell}, lowest center z ${v.g_veh_min_z}`);
    if (!(n(v.g_veh_in_solid) > 0 || n(v.g_veh_fell) > 0 || n(v.g_veh_min_z) < minSurface)) failures.push('control: vehicles without terrain collision but nothing counted it');
  } finally {
    await d.shutdown();
  }
  return { ok: failures.length === 0, failures, rows };
}
