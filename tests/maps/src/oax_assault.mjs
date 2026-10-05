// oax_assault: an outdoor Assault map (g_gametype 14, GT_ASSAULT). A
// heightmap valley climbs from the attackers' landing in the west to a
// walled fortress in the east. Three objectives, in order:
//
//   1. the gate generator (destroy), in front of the outer wall: it opens
//      the gate;
//   2. the keep controls (use: hold the use button for 5 seconds), in the
//      courtyard: they open the keep;
//   3. the golden cow (destroy, final), inside the keep: the round.
//
// Spawns move with the fight: the attackers land in the west, then spawn on
// the field in front of the fortress once the generator is down, then in
// the gateway once the controls are taken. The defenders start in front of
// the wall and on its walk, fall back to the courtyard, then to the keep.
// Defenders reach the wall walk by two ramps in the courtyard and can drop
// down outside; the wall is too high to climb from outside.
//
// The battlefield: two defender turrets on the wall walk (misc_oax_turret),
// artillery on the field in front of the wall until the generator falls
// (trigger_oax_artillery), a shake and explosions when the generator and
// the cow go (target_oax_shake, target_oax_explosion). Once the generator
// is down an attackers-only teleporter at the landing sends them straight
// to the field (trigger_teleport with role / after). Defending bots hold
// posts by the objectives (info_oax_assault_defend).
//
// With the vehicle rule (g_oaxVehicles 1) a buggy and a hover craft wait at
// the landing. No AAS: bots path on the navmesh. Our own geometry with
// OpenArena textures; the cow is Quaternius's CC0 one (its source and build
// in assets/assault/BUILD.md), gilded with oaxMetal: it reflects the keep
// through a misc_cubemap probe.

import { MapFile, Brush, Face, box } from '../mapwriter.mjs';
import { Terrain } from '../lib/terrain.mjs';
import { foliageFiles } from '../lib/foliage-art.mjs';
import { vehicleFiles, addVehicle } from '../lib/vehicles.mjs';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// the map's art (the golden cow; licences in assets/assault/licenses)
const ASSETS = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'assault');
function assetFiles() {
  const out = {};
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      const rel = path.relative(ASSETS, p);
      if (e.isDirectory()) walk(p);
      else if (rel !== 'BUILD.md') out[rel.startsWith('licenses') ? rel.replace(/^licenses/, 'credits/oax_assault') : rel] = fs.readFileSync(p);
    }
  };
  walk(ASSETS);
  return out;
}

export const FIELD = { x0: -4096, x1: 4096, y0: -2048, y1: 2048 };
export const CELL = 64;

// the fortress (all on flat ground at z 0)
export const WALL_X = 1800;           // the outer wall's outside face
const WALL_T = 64, WALL_H = 240, WALL_Y = 1000, BACK_X = 3800;
const GATE_W = 160, GATE_H = 192;
const WALK_Z = 200, WALK_W = 96;
export const KEEP = { x0: 3100, x1: 3800, y: 520, h: 320, doorW: 128, doorH: 160 };
export const GENERATOR = [1640, 520];
export const CONTROLS = [2500, -560];
export const CORE = [3500, 0];

const smooth = (e0, e1, x) => { const t = Math.min(1, Math.max(0, (x - e0) / (e1 - e0))); return t * t * (3 - 2 * t); };
const bump = (x, y, cx, cy, r) => Math.exp(-((x - cx) ** 2 + (y - cy) ** 2) / (2 * r * r));

export function fieldHeight(x, y) {
  let z = -140 * (1 - smooth(-3800, 1200, x));                // the valley climbs to the fortress
  z += 80 * Math.sin(x / 560) * Math.cos(y / 470) + 40 * Math.sin((x - y) / 330);
  z += 280 * bump(x, y, -1500, 1150, 190);                   // rock outcrops: cover and flanks
  z += 260 * bump(x, y, -300, -1250, 200);
  z += 240 * bump(x, y, 700, 1300, 180);
  z += 180 * (smooth(1300, 1900, Math.abs(y)) * (1 - smooth(1200, 1600, x)));   // the valley's sides
  const flat = smooth(1000, 1450, x);                        // level ground under the fortress
  const landing = bump(x, y, -3600, 0, 420);                 // and at the landing
  return z * (1 - flat) * (1 - 0.9 * landing);
}

export function makeTerrain() {
  const t = new Terrain({
    name: 'oax_assault', origin: [FIELD.x0, FIELD.y0, -512], cellSize: CELL,
    samplesX: (FIELD.x1 - FIELD.x0) / CELL + 1, samplesY: (FIELD.y1 - FIELD.y0) / CELL + 1, heightScale: 1 / 32,
  });
  t.shape((x, y) => fieldHeight(x, y));
  t.paint((x, y, z, slope) => {
    const fort = x > 1350;
    const route = Math.abs(y) < 500 && x < 1300;
    return {
      splat: [slope > 0.88 ? 1 : 0.1, fort ? 1.3 : route ? 1.0 : slope > 0.8 ? 0.35 : 0.5, slope < 0.8 ? 1.6 : 0, z < -100 ? 0.8 : 0],
      density: [
        slope > 0.9 && !fort ? 1 : 0,
        slope > 0.93 && !fort && !route && Math.abs(y) > 650 && x > -3100 ? 0.7 : 0,
        0, 0],
    };
  });
  return t;
}

// a wedge rising along +x (dir 1) or -x (dir -1) from z0 to z1 over [xa, xb]
function ramp(xa, xb, y0, y1, z0, z1, dir, tex) {
  const L = xb - xa, H = z1 - z0;
  const lowX = dir > 0 ? xa : xb;
  const n = [-dir * H, 0, L];
  const d = n[0] * lowX + n[2] * z0;
  return new Brush([
    new Face([0, 0, -1], -(z0 - 32), tex),
    new Face(n, d, tex),
    new Face([dir, 0, 0], dir > 0 ? xb : -xa, tex),
    new Face([0, 1, 0], y1, tex),
    new Face([0, -1, 0], -y0, tex),
  ]).check();
}

const STONE = 'gothic_block/blocks18c', WALL = 'gothic_wall/streetbricks10', FLOOR = 'gothic_floor/xstepborder5';
const METAL = 'base_floor/metaltechfloor01final', DOOR = 'base_door/shinymetaldoor';

function fortress(map) {
  const x0 = WALL_X, x1 = WALL_X + WALL_T;
  // the outer wall either side of the gate, and the lintel over it
  map.brush(box([x0, -WALL_Y, -32], [x1, -GATE_W, WALL_H], WALL));
  map.brush(box([x0, GATE_W, -32], [x1, WALL_Y, WALL_H], WALL));
  map.brush(box([x0, -GATE_W, GATE_H], [x1, GATE_W, WALL_H], WALL));
  // side and back walls
  for (const s of [1, -1]) {
    map.brush(box([x0, s > 0 ? WALL_Y : -WALL_Y - WALL_T, -32], [BACK_X + WALL_T, s > 0 ? WALL_Y + WALL_T : -WALL_Y, WALL_H], WALL));
  }
  map.brush(box([BACK_X, -WALL_Y, -32], [BACK_X + WALL_T, WALL_Y, WALL_H], WALL));
  // the courtyard floor (a little over the terrain, which is flat there)
  map.brush(box([x1, -WALL_Y, -32], [BACK_X, WALL_Y, 4], { top: FLOOR, sides: STONE, bottom: 'common/caulk' }));
  // the wall walk inside the outer wall, across the gate, and its two ramps
  map.brush(box([x1, -WALL_Y, WALK_Z - 16], [x1 + WALK_W, WALL_Y, WALK_Z], { top: FLOOR, sides: STONE, bottom: STONE }));
  for (const s of [1, -1]) {
    const y0 = s > 0 ? 820 : -WALL_Y, y1 = s > 0 ? WALL_Y : -820;
    map.brush(ramp(x1 + WALK_W, x1 + WALK_W + 560, y0, y1, 0, WALK_Z, -1, STONE));
  }
  // the gate: slides sideways into the wall when the generator falls
  map.entity('func_door', { targetname: 'genfall', angle: 90, lip: 8, speed: 140, wait: -1, sounds: 1 },
    [box([x0 + 16, -GATE_W, 4], [x0 + 48, GATE_W, GATE_H], DOOR)]);

  // the keep: four walls with a door in the west one, a roof
  const k = KEEP;
  map.brush(box([k.x0, -k.y, 0], [k.x0 + 48, -k.doorW, k.h], WALL));
  map.brush(box([k.x0, k.doorW, 0], [k.x0 + 48, k.y, k.h], WALL));
  map.brush(box([k.x0, -k.doorW, k.doorH], [k.x0 + 48, k.doorW, k.h], WALL));
  map.brush(box([k.x0, -k.y - 48, 0], [k.x1, -k.y, k.h], WALL));
  map.brush(box([k.x0, k.y, 0], [k.x1, k.y + 48, k.h], WALL));
  map.brush(box([k.x0, -k.y - 48, k.h], [k.x1, k.y + 48, k.h + 24], { bottom: METAL, top: STONE, sides: STONE }));
  map.entity('func_door', { targetname: 'keepdoor', angle: 90, lip: 8, speed: 120, wait: -1, sounds: 1 },
    [box([k.x0 + 8, -k.doorW, 4], [k.x0 + 40, k.doorW, k.doorH], DOOR)]);
  // cover in the courtyard: low blocks and a bunker
  for (const [x, y, w, d, h] of [[2250, 250, 96, 160, 64], [2700, -100, 160, 96, 72], [2900, 640, 128, 128, 96], [2250, -900, 128, 64, 56]]) {
    map.brush(box([x - w / 2, y - d / 2, 4], [x + w / 2, y + d / 2, 4 + h], STONE));
  }
  // a platform the controls stand on
  map.brush(box([CONTROLS[0] - 120, CONTROLS[1] - 100, 4], [CONTROLS[0] + 120, CONTROLS[1] + 100, 28], { top: METAL, sides: STONE }));

  // lights: the courtyard, the keep, the gateway
  for (const [x, y] of [[2300, -600], [2300, 600], [2800, 0], [2100, 0]]) {
    map.entity('light', { origin: [x, y, 360], light: 700, _color: [1, 0.92, 0.8] });
  }
  map.entity('light', { origin: [3450, 0, k.h - 40], light: 700, _color: [1, 0.55, 0.45] });
  map.entity('light', { origin: [WALL_X - 80, 0, 260], light: 450, _color: [1, 0.85, 0.6] });
}

function objectives(map) {
  // 1. the gate generator: a machine against the wall, glowing on top
  const [gx, gy] = GENERATOR;
  map.entity('func_oax_objective', {
    type: 'destroy', id: 'generator', name: 'the gate generator', order: 1, health: 900, target: 'genfall',
    message: 'The gate generator is down: the gate is open',
  }, [
    box([gx - 56, gy - 56, -16], [gx + 56, gy + 56, 96], { sides: 'base_support/cable', top: 'base_light/proto_lightblue', bottom: 'common/caulk' }),
  ]);
  map.entity('light', { origin: [gx - 90, gy, 140], light: 300, _color: [0.4, 0.6, 1] });
  // 2. the keep controls: a console on its platform
  const [cx, cy] = CONTROLS;
  map.entity('func_oax_objective', {
    type: 'use', id: 'controls', name: 'the keep controls', order: 2, usetime: 5, radius: 80, target: 'keepdoor',
    message: 'The keep controls are taken: the keep is open',
  }, [
    box([cx - 24, cy - 40, 28], [cx + 24, cy + 40, 76], { sides: METAL, top: 'base_light/ceil1_38', bottom: 'common/caulk' }),
  ]);
  map.entity('light', { origin: [cx, cy, 140], light: 220, _color: [0.6, 1, 0.6] });
  // 3. the golden cow, on a plinth, facing the keep door: the model drawn
  // at the origin, an invisible hitbox around it (brushes of an entity with
  // an origin are relative to it: q3map2 keeps them as written, the game
  // adds the origin)
  const [rx, ry] = CORE;
  map.brush(box([rx - 140, ry - 60, 0], [rx + 140, ry + 60, 16], { sides: METAL, top: 'base_floor/clang_floor', bottom: 'common/caulk' }));
  map.entity('func_oax_objective', {
    type: 'destroy', id: 'cow', name: 'the golden cow', order: 3, final: 1, health: 1500, target: 'coreboom',
    message: 'The golden cow is dead', origin: [rx, ry, 16], model2: 'models/oax/cow/goldcow.md3',
  }, [
    box([-120, -32, 0], [120, 32, 130], 'common/weapclip'),
  ]);
  map.entity('light', { origin: [rx - 160, ry, 220], light: 400, _color: [1, 0.85, 0.6] });
  // bright fixtures around it for the gold to mirror: ceiling panels and
  // blue strips on the side walls
  for (const [x, y] of [[rx - 180, ry - 220], [rx - 180, ry + 220], [rx + 180, ry - 220], [rx + 180, ry + 220]]) {
    map.brush(box([x - 32, y - 32, KEEP.h - 8], [x + 32, y + 32, KEEP.h], { bottom: 'base_light/ceil1_38', sides: METAL, top: 'common/caulk' }));
  }
  for (const y of [-KEEP.y, KEEP.y]) {
    const iy = y < 0 ? y : y - 8;
    map.brush(box([rx - 200, iy, 120], [rx + 200, iy + 8, 136], { [y < 0 ? 'py' : 'ny']: 'base_light/proto_lightblue', sides: METAL }));
  }
  // the probe the gold reflects (misc_cubemap: oaxMetal, docs/materials.md)
  map.entity('misc_cubemap', { origin: [rx, ry, 90], radius: 420, name: 'cowprobe' });
  // what their fall looks like
  map.entity('target_oax_shake', { targetname: 'genfall', origin: [gx, gy, 64], intensity: 2.5, duration: 1.2, radius: 2500 });
  map.entity('target_oax_explosion', { targetname: 'genfall', origin: [gx, gy, 48], count: 3, delay: 0.3, spread: 48 });
  map.entity('target_oax_shake', { targetname: 'coreboom', origin: [rx, ry, 90], intensity: 5, duration: 2.5 });
  map.entity('target_oax_explosion', { targetname: 'coreboom', origin: [rx, ry, 90], count: 6, delay: 0.25, spread: 80 });
}

function battlefield(map, ground) {
  // defender turrets on the wall walk, covering the approach
  for (const y of [-760, 760]) {
    map.entity('misc_oax_turret', { origin: [WALL_X + 32, y, WALL_H], angle: 180, role: 'defend', arc: 70, range: 1100, health: 250 });
  }
  // artillery on the field before the wall, until the generator falls
  map.entity('trigger_oax_artillery', { role: 'attack', until: 'generator', interval: 4, dmg: 70, radius: 180, spread: 200 },
    [box([-600, -1300, -400], [WALL_X - 40, 1300, 700], 'common/trigger')]);
  // an attackers-only teleporter at the landing, once the generator is down
  const [tx, ty] = [-3800, 900];
  map.entity('trigger_teleport', { target: 'tp_field', role: 'attack', after: 'generator' },
    [box([tx - 48, ty - 48, ground(tx, ty) - 8], [tx + 48, ty + 48, ground(tx, ty) + 64], 'common/trigger')]);
  map.brush(box([tx - 56, ty - 56, ground(tx, ty) - 24], [tx + 56, ty + 56, ground(tx, ty) + 2], { top: 'base_light/proto_lightblue', sides: METAL, bottom: 'common/caulk' }));
  map.entity('misc_teleporter_dest', { targetname: 'tp_field', origin: [400, 0, ground(400, 0) + 40], angle: 0 });
  // defending bots' posts: the generator's front, the walk, the controls, the core
  const post = (x, y, z, objective, priority) => map.entity('info_oax_assault_defend', { origin: [x, y, z], objective, priority });
  for (const [x, y] of [[1500, 300], [1500, 750], [1400, -200], [1650, -600]]) post(x, y, ground(x, y) + 32, 'generator', 0);
  post(WALL_X + 112, -400, WALK_Z + 32, 'generator', 1);
  post(WALL_X + 112, 400, WALK_Z + 32, 'generator', 1);
  for (const [x, y] of [[2350, -350], [2650, -760], [2700, -300], [2300, -800]]) post(x, y, 60, 'controls', 2);
  for (const [x, y] of [[3350, -250], [3350, 250], [3600, -350], [3600, 350]]) post(x, y, 40, 'cow', 3);
}

function spawns(map, ground) {
  const spot = (role, x, y, z, angle, keys = {}) => map.entity('info_oax_assault_spawn', { origin: [x, y, z], angle, role, ...keys });
  // attackers: the landing, then the field before the wall, then the gateway
  for (const [x, y] of [[-3700, -260], [-3700, 0], [-3700, 260], [-3550, -130], [-3550, 130], [-3850, 0]]) {
    spot('attack', x, y, ground(x, y) + 32, 0, { until: 'generator' });
  }
  for (const [x, y] of [[300, -420], [300, 0], [300, 420], [150, -210], [150, 210]]) {
    spot('attack', x, y, ground(x, y) + 32, 0, { after: 'generator', until: 'controls' });
  }
  for (const [x, y] of [[2000, -300], [2000, 300], [1500, -200], [1500, 200], [1500, 0]]) {
    spot('attack', x, y, ground(x, y) + 32, 0, { after: 'controls' });
  }
  // defenders: in front of the wall and on the walk, then the courtyard, then the keep
  for (const [x, y, z] of [[1600, -760, null], [1600, 760, null], [1400, -300, null], [WALL_X + 112, -500, WALK_Z + 32], [WALL_X + 112, 500, WALK_Z + 32]]) {
    spot('defend', x, y, z ?? ground(x, y) + 32, 180, { until: 'generator' });
  }
  for (const [x, y] of [[2700, 600], [2700, -800], [2950, 200], [2950, -400], [2400, 750]]) {
    spot('defend', x, y, 40, 180, { after: 'generator', until: 'controls' });
  }
  for (const [x, y] of [[3650, -350], [3650, 350], [3650, 0], [3300, -400], [3300, 400]]) {
    spot('defend', x, y, 40, 180, { after: 'controls' });
  }
}

// a metal (oaxMetal: reflectance, roughness: it mirrors the keep) over a
// dark base, with the classic chrome trick on top: an additive sphere map of
// a studio's hot spots (goldshine.png), so it shines like gold in any light
const gold = (name, metal, base) => `models/oax/cow/gold_${name}
{
	oaxMetal ${metal}
	{
		map $whiteimage
		rgbGen const ( ${base} )
	}
	{
		map models/oax/cow/goldshine.png
		tcGen environment
		blendFunc add
		rgbGen identityLighting
	}
}
`;

const shaders = `// oax_assault map shaders (tests/maps/src/oax_assault.mjs)
// the golden cow: one polished gold all over (its spots too), the nose a
// touch rosier
${gold('white', '1 0.78 0.34 0.35', '0.12 0.08 0.02')}${gold('black', '1 0.78 0.34 0.35', '0.12 0.08 0.02')}${gold('pink', '1 0.7 0.45 0.35', '0.12 0.07 0.03')}
textures/oax_assault/sky
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	q3map_sun 1 0.9 0.78 230 200 55
	q3gl2_sun 1 0.9 0.78 250 200 55 0.35
	skyparms env/sky1/sky001 - -
}
`;

const arena = `{
map "oax_assault"
longname "oax Assault"
type "assault"
}
`;

export function build() {
  const t = makeTerrain();
  const map = new MapFile({ message: 'oax Assault', _ambient: 45 });
  const w = 32, sky = 'oax_assault/sky', cliff = 'gothic_block/blocks18c';
  const { x0, x1, y0, y1 } = FIELD;
  const zFloor = -700, zTop = 1800;
  map.brush(
    box([x0 - w, y0 - w, zFloor - w], [x1 + w, y1 + w, zFloor], 'common/caulk'),
    box([x0 - w, y0 - w, zTop], [x1 + w, y1 + w, zTop + w], sky),
    box([x0 - w, y0 - w, zFloor], [x0, y1 + w, zTop], cliff),
    box([x1, y0 - w, zFloor], [x1 + w, y1 + w, zTop], cliff),
    box([x0, y0 - w, zFloor], [x1, y0, zTop], cliff),
    box([x0, y1, zFloor], [x1, y1 + w, zTop], cliff),
  );
  map.entity('misc_oax_terrain', t.entityKeys({
    seed: 2026,
    bottom: -640,
    layers: [
      { shader: 'textures/acc_dm3/grass', scale: 384 },
      { shader: 'textures/base_floor/dirt', scale: 256 },
      { shader: 'textures/cosmo_block/rock01', scale: 320 },
      { shader: 'textures/cosmo_floor/sand01', scale: 256 },
    ],
    foliage: [
      'grass textures/oax_terrain/grassblades 0 5 18 30 1000 1500',
      'tree textures/oax_terrain/tree 1 0.07 240 330 3500 4200 10 120 0.9',
    ],
  }));
  fortress(map);
  objectives(map);
  const ground = (x, y) => Math.ceil(t.groundZ(x, y));
  spawns(map, ground);
  battlefield(map, ground);
  map.entity('info_oax_assault', {
    time: 300,
    message: 'Destroy the gate generator, take the keep controls, kill the golden cow',
  });

  // items: the attackers' landing and the field; the defenders' walk and keep
  const item = (cls, x, y, z) => map.entity(cls, { origin: [x, y, z ?? ground(x, y) + 24] });
  for (const [cls, x, y] of [
    ['weapon_rocketlauncher', -3450, -420], ['weapon_shotgun', -3450, 420], ['ammo_rockets', -3300, 0], ['item_armor_combat', -3300, -250],
    ['weapon_plasmagun', -1800, 300], ['ammo_cells', -1700, 450], ['item_health_large', -900, -250], ['weapon_grenadelauncher', -600, 450],
    ['ammo_grenades', -500, 600], ['item_armor_shard', 200, -200], ['item_armor_shard', 200, 200], ['item_health', 600, 0],
    ['ammo_rockets', 1100, -600], ['item_health', 1100, 600],
  ]) item(cls, x, y);
  for (const [cls, x, y, z] of [
    ['weapon_railgun', WALL_X + 112, 0, WALK_Z + 24], ['ammo_slugs', WALL_X + 112, 300, WALK_Z + 24], ['ammo_slugs', WALL_X + 112, -300, WALK_Z + 24],
    ['weapon_lightning', 2600, 300, 28], ['ammo_lightning', 2650, 380, 28], ['item_armor_body', 2950, -700, 28], ['item_health_large', 2400, 0, 28],
    ['item_health_mega', 3500, -380, 28], ['ammo_rockets', 3500, 380, 28],
  ]) item(cls, x, y, z);
  // vehicles at the landing (the vehicle rule)
  addVehicle(map, 'buggy', [-3350, -700, ground(-3350, -700) + 60], 0, 20);
  addVehicle(map, 'hover', [-3350, 700, ground(-3350, 700) + 60], 0, 20);

  map.entity('info_player_intermission', { origin: [400, -1700, 900], angles: [24, 30, 0] });
  // a few deathmatch spots (other game types and spectators fall back to them)
  for (const [x, y] of [[-3700, 0], [2400, 0], [-500, 0]]) {
    map.entity('info_player_deathmatch', { origin: [x, y, (x > 1400 ? 4 : ground(x, y)) + 32], angle: x > 0 ? 180 : 0 });
  }
  map.entity('light', { origin: [0, 0, 1400], light: 2500 });
  return {
    map, aas: false,
    manifest: { features: ['terrain', 'nav', 'vehicles', 'assault'] },
    files: {
      'scripts/oax_assault.shader': shaders, 'scripts/oax_assault.arena': arena,
      ...t.files(), ...foliageFiles(), ...vehicleFiles(), ...assetFiles(),
    },
  };
}
