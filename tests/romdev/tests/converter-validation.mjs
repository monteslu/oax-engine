// Converter validation: maps converted from another engine's format by an
// external converter must be playable as OpenArena maps on the cart.
//
// For each converted map: it loads; a player placed at every spawn point
// stands on the floor there; the bots' navigation routes every pickup and
// flag from a spawn (botlib travel times, the routing bots play by); a bot
// match (CTF on CTF maps) actually takes both flags; and no bot ever drops
// below the lowest model in the map (out of the world, where no trigger_hurt
// can catch it). Controls: a point below the world must fail the standing
// check, and the item count must be nonzero.
//
// Converted maps are local builds, never committed: OA_CONVERTED_DIR
// (default ~/.openarena/baseoa) holds <map>.pk3 (named by OA_CONVERTED_MAPS), which the
// gate packs into the cart (misc/ci/gate.sh).

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { Session, CA_ACTIVE, defaultCart } from '../lib/romdev.mjs';
import { readValues } from '../lib/values.mjs';
import { bspBuffer, entitiesFromBuffer, worldBounds, lowestModelZ } from '../lib/bsp.mjs';

export const name = 'converter-validation';
export const external = true;

export const convertedDir = process.env.OA_CONVERTED_DIR || path.join(os.homedir(), '.openarena', 'baseoa');
const MAPS = (process.env.OA_CONVERTED_MAPS || '').split(',');
const MATCH_FRAMES = Number(process.env.OA_VALIDATION_FRAMES || 11250);   // 3 minutes at 16 ms
const BOTS = { red: ['Angelyss', 'Arachna', 'Major'], blue: ['Sarge', 'Grism', 'Kyonshi'] };

const SPAWN_CLASSES = new Set(['info_player_deathmatch', 'info_player_start', 'team_CTF_redspawn', 'team_CTF_bluespawn', 'team_CTF_redplayer', 'team_CTF_blueplayer']);

function vec(s) {
  return (s || '0 0 0').split(/\s+/).map(Number);
}

async function boot(s, map, gametype) {
  await s.load();
  await s.command(`bot_enable 1; g_gametype ${gametype}; g_doWarmup 0; timelimit 0; fraglimit 0; capturelimit 0; devmap ${map}`);
  await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 4000, 20);
  await s.step(30);
  const mapname = await s.read('mapname');
  if (mapname !== map) throw new Error(`loaded ${mapname}, not ${map}`);
}

// Place the player at p and let it settle: it must end standing within a
// short distance of the spawn (not falling, not stuck in the air).
// (setviewpos teleports, which kicks the player forward at 400 ups: let the
// kick die out before judging.)
async function standsAt(s, p) {
  await s.command(`setviewpos ${p[0]} ${p[1]} ${p[2] + 8} 0`);
  await s.step(120);
  const o = await s.read('player_origin');
  const ground = await s.read('player_ground_entity');
  const v = await s.read('player_velocity');
  const drop = p[2] - o[2];
  return { ok: ground !== -1 && drop < 48 && Math.hypot(v[0], v[1], v[2]) < 1, drop, ground, speed: Math.hypot(v[0], v[1], v[2]) };
}

async function validate(map, rows, failures) {
  const pk3 = path.join(convertedDir, `${map}.pk3`);
  if (!fs.existsSync(pk3)) { failures.push(`${map}: ${pk3} not found`); return; }
  const buf = bspBuffer({ pk3, member: `maps/${map}.bsp` });
  const ents = entitiesFromBuffer(buf);
  const { mins } = worldBounds(buf);
  const floorZ = lowestModelZ(buf);
  const ctf = ents.some((e) => e.classname === 'team_CTF_redflag') && ents.some((e) => e.classname === 'team_CTF_blueflag');
  const spawns = ents.filter((e) => SPAWN_CLASSES.has(e.classname)).map((e) => vec(e.origin));
  if (!fs.existsSync(path.join(defaultCart, 'assets', 'baseoa', 'maps', `${map}.bsp`))) {
    failures.push(`${map}: not packed into the cart (pack-cart --pk3 ${pk3})`);
    return;
  }

  const s = new Session(`convert-${map}`);
  try {
    await boot(s, map, ctf ? 4 : 0);
    // team games start the local player as a spectator (no collision)
    if (ctf) {
      await s.command('team red');
      await s.step(60);
    }
    const pm = await s.read('player_pm_type');
    if (pm !== 0) failures.push(`${map}: the local player is not a normal player (pm_type ${pm})`);

    // every spawn point holds a standing player
    let bad = 0;
    for (const p of spawns) {
      const r = await standsAt(s, p);
      if (!r.ok) { bad++; if (bad <= 3) failures.push(`${map}: spawn ${p.join(' ')} does not hold a player (dropped ${r.drop.toFixed(1)}, ground ${r.ground}, speed ${r.speed.toFixed(1)})`); }
    }
    // control: the same check must fail in the void below the world
    const voidCheck = await standsAt(s, [spawns[0][0], spawns[0][1], mins[2] - 512]);
    if (voidCheck.ok) failures.push(`${map}: control: a point below the world passed the spawn check`);

    // bot match; the local player watches as a spectator
    await s.command('team spectator');
    await s.step(10);
    for (const t of ['red', 'blue']) {
      for (const b of BOTS[t]) {
        await s.command(ctf ? `addbot ${b} 4 ${t}` : `addbot ${b} 4`);
        await s.step(5);
      }
    }
    for (let done = 0; done < MATCH_FRAMES; done += 750) await s.step(750);
    const v = await readValues(s);
    const total = Number(v.g_items_total), reached = Number(v.g_items_reached);
    const minZ = v.g_min_z === 'none' ? null : Number(v.g_min_z);
    rows.push(`${map}: ${ctf ? 'CTF' : 'DM'}, ${spawns.length} spawns (${bad} bad), items routable ${v.g_items_routable}/${v.g_items_routed_total} from ${v.g_route_spawns} spawns, bots touched ${reached}/${total}, flags ${v.g_flags_reached}/${v.g_flags_total}, lowest bot z ${minZ} (lowest model ${floorZ.toFixed(0)}), ${Number(v.g_level_time) / 1000}s of play`);
    if (bad) failures.push(`${map}: ${bad} of ${spawns.length} spawn points do not hold a player`);
    if (!(total > 0)) failures.push(`${map}: control: no items counted (stats not running?)`);
    if (!(Number(v.g_route_spawns) > 0)) failures.push(`${map}: no spawn point has a navigation area (AAS missing?)`);
    if (Number(v.g_items_routable) !== total) failures.push(`${map}: ${total - Number(v.g_items_routable)} items have no route from any spawn: ${v.g_items_unroutable}`);
    if (!(reached > 0)) failures.push(`${map}: bots never picked anything up`);
    if (ctf && !(Number(v.g_flags_total) === 2 && Number(v.g_flags_reached) === 2)) failures.push(`${map}: flags reached ${v.g_flags_reached}/${v.g_flags_total}`);
    if (minZ === null) failures.push(`${map}: no living bot recorded`);
    else if (minZ < floorZ) failures.push(`${map}: a bot fell to z ${minZ}, below every model (${floorZ.toFixed(0)})`);
  } finally {
    await s.shutdown();
  }
}

export async function run() {
  const failures = [];
  const rows = [];
  for (const map of MAPS) await validate(map, rows, failures);
  return { ok: failures.length === 0, failures, rows };
}
