// oax_outdoor_ctf: the phase 7 exit map. An outdoor CTF field: two stone
// forts at the ends of a 6144 x 3072 heightmap valley (misc_oax_terrain:
// rolling hills, two rock outcrops too steep to climb, a central rise),
// grass and solid-trunked trees placed from a density map, sun with
// cascaded shadows (q3gl2_sun). Our own geometry with OpenArena textures.
//
// Bases: a raised platform with two ramps up from the field, back and side
// walls and a roof over the flag. Every item stands on the terrain or a
// platform a player can walk to. No AAS: bots navigate on the navmesh built
// from the brushes and the terrain.

import { MapFile, Brush, Face, box } from '../mapwriter.mjs';
import { Terrain } from '../lib/terrain.mjs';
import { foliageFiles } from '../lib/foliage-art.mjs';

export const FIELD = { x0: -3072, x1: 3072, y0: -1536, y1: 1536 };
export const CELL = 64;
export const BASE_X = 2600;          // flag x for red (+) and blue (-)
const PLATFORM_Z = 64;

const smooth = (e0, e1, x) => { const t = Math.min(1, Math.max(0, (x - e0) / (e1 - e0))); return t * t * (3 - 2 * t); };
const bump = (x, y, cx, cy, r) => Math.exp(-((x - cx) ** 2 + (y - cy) ** 2) / (2 * r * r));

export function fieldHeight(x, y) {
  let z = 0;
  z += 110 * Math.sin(x / 520) * Math.cos(y / 430);
  z += 60 * Math.sin((x + y) / 300);
  z += 180 * bump(x, y, 0, 0, 420);                     // central rise
  z += 260 * bump(x, y, -900, 800, 170);                // rock outcrops (steep)
  z += 260 * bump(x, y, 900, -800, 170);
  z -= 90 * bump(x, y, 1300, 700, 380);                 // hollows
  z -= 90 * bump(x, y, -1300, -700, 380);
  // flat ground around the forts
  const flat = smooth(1900, 2250, Math.abs(x));
  return z * (1 - flat);
}

// clear: [x, y] points (items, spawns) that must stay free of tree trunks
export function makeTerrain(clear = ITEM_SPOTS) {
  const t = new Terrain({
    name: 'oax_outdoor_ctf', origin: [FIELD.x0, FIELD.y0, -512], cellSize: CELL,
    samplesX: (FIELD.x1 - FIELD.x0) / CELL + 1, samplesY: (FIELD.y1 - FIELD.y0) / CELL + 1, heightScale: 1 / 32,
  });
  t.shape((x, y) => fieldHeight(x, y));
  t.paint((x, y, z, slope) => {
    const nearBase = Math.abs(x) > 2150 && Math.abs(y) < 700;
    const edge = Math.min(x - FIELD.x0, FIELD.x1 - x, y - FIELD.y0, FIELD.y1 - y);
    const nearItem = clear.some(([cx, cy]) => Math.hypot(x - cx, y - cy) < 192);
    return {
      splat: [slope > 0.88 ? 1 : 0.1, nearBase ? 1.2 : slope > 0.8 ? 0.35 : 0.6, slope < 0.8 ? 1.6 : 0, z < -60 ? 1 : 0],
      // grass on gentle open ground; trees in groves, never on the forts' approaches
      density: [
        slope > 0.9 && !nearBase ? 1 : 0,
        slope > 0.93 && !nearBase && !nearItem && edge > 200 && Math.abs(y) > 450 && Math.abs(x) < 1900 ? 0.8 : 0,
        0, 0],
    };
  });
  return t;
}

// items on the field (mirrored for both teams where noted), shared with the tests
export const FIELD_ITEMS = [
  ...[1, -1].flatMap((s) => [
    ['weapon_shotgun', s * 2050, -800], ['ammo_shells', s * 2050, 800], ['item_health', s * 1700, 0], ['ammo_rockets', s * 1500, -1100],
    ['weapon_plasmagun', s * 1200, 1150], ['ammo_cells', s * 1000, 1250], ['item_armor_shard', s * 600, -350], ['item_health', s * 600, 350],
  ]),
  ['weapon_rocketlauncher', 0, 0], ['weapon_railgun', 0, 1250], ['ammo_slugs', 0, -1250], ['item_armor_body', 0, -600],
];
const SPAWN_SPOTS = [1, -1].flatMap((s) => [[2100, -500], [2100, 500], [2000, 0]].map(([x, y]) => [s * x, y]));
const ITEM_SPOTS = [...FIELD_ITEMS.map(([, x, y]) => [x, y]), ...SPAWN_SPOTS];

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

// one fort; s = +1 red (x > 0), -1 blue
function fort(map, s) {
  const stone = 'gothic_block/blocks18c', top = 'gothic_floor/xstepborder5', wall = 'gothic_wall/streetbricks10', trim = 'gothic_trim/wood2';
  const X = (a, b) => (s > 0 ? [a, b] : [-b, -a]);
  const [px0, px1] = X(2400, 2900);
  // platform, sunk into the flat ground
  map.brush(box([px0, -320, -32], [px1, 320, PLATFORM_Z], { top, sides: stone, bottom: 'common/caulk' }));
  // two ramps up from the field side, 160 wide
  const [ra, rb] = X(2208, 2400);
  for (const [y0, y1] of [[-300, -140], [140, 300]]) map.brush(ramp(ra, rb, y0, y1, 0, PLATFORM_Z, s, stone));
  // walls: back and sides on the platform
  const [bx0, bx1] = X(2868, 2900);
  map.brush(box([bx0, -320, PLATFORM_Z], [bx1, 320, PLATFORM_Z + 224], wall));
  for (const [y0, y1] of [[-320, -288], [288, 320]]) {
    const [sx0, sx1] = X(2560, 2868);
    map.brush(box([sx0, y0, PLATFORM_Z], [sx1, y1, PLATFORM_Z + 224], wall));
  }
  // a roof over the flag, on the walls
  const [fx0, fx1] = X(2560, 2900);
  map.brush(box([fx0, -320, PLATFORM_Z + 224], [fx1, 320, PLATFORM_Z + 240], { bottom: trim, top: stone, sides: stone }));
  // a low parapet in front of the flag (cover; a jump or the gaps get past it)
  const [cx0, cx1] = X(2440, 2456);
  map.brush(box([cx0, -100, PLATFORM_Z], [cx1, 100, PLATFORM_Z + 40], stone));
  // a light under the roof
  map.entity('light', { origin: [s * 2730, 0, PLATFORM_Z + 200], light: 500, _color: s > 0 ? [1, 0.5, 0.45] : [0.45, 0.55, 1] });
}

const shaders = `// oax_outdoor_ctf map shaders (tests/maps/src/oax_outdoor_ctf.mjs)
textures/oax_outdoor_ctf/sky
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	q3map_sun 1 0.95 0.85 200 40 50
	q3gl2_sun 1 0.95 0.85 220 40 50 0.35
	skyparms env/sky1/sky001 - -
}
`;

export function build() {
  const t = makeTerrain();
  const map = new MapFile({ message: 'oax outdoor CTF', _ambient: 25 });
  const w = 32, sky = 'oax_outdoor_ctf/sky', cliff = 'gothic_block/blocks18c';
  const { x0, x1, y0, y1 } = FIELD;
  const zFloor = -700, zTop = 1600;
  // sealed: floor far below the terrain, sky above, rock walls round the field
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
  fort(map, 1);
  fort(map, -1);

  const ground = (x, y) => Math.ceil(t.groundZ(x, y));
  const item = (cls, x, y, extra = {}) => map.entity(cls, { origin: [x, y, ground(x, y) + 24], ...extra });
  for (const s of [1, -1]) {
    const team = s > 0 ? 'red' : 'blue';
    map.entity(`team_CTF_${team}flag`, { origin: [s * 2760, 0, PLATFORM_Z + 24] });
    // initial and respawn points on and in front of the fort
    for (const [x, y] of [[2520, -200], [2520, 200], [2680, -180], [2680, 180]]) {
      map.entity(`team_CTF_${team}player`, { origin: [s * x, y, PLATFORM_Z + 32], angle: s > 0 ? 180 : 0 });
      map.entity(`team_CTF_${team}spawn`, { origin: [s * x, y, PLATFORM_Z + 32], angle: s > 0 ? 180 : 0 });
    }
    for (const [x, y] of [[2100, -500], [2100, 500], [2000, 0]]) {
      map.entity(`team_CTF_${team}spawn`, { origin: [s * x, y, ground(s * x, y) + 32], angle: s > 0 ? 180 : 0 });
    }
    // base items: on the platform and in front of it
    map.entity('item_armor_combat', { origin: [s * 2800, -200, PLATFORM_Z + 24] });
    map.entity('item_health_large', { origin: [s * 2800, 200, PLATFORM_Z + 24] });
  }
  for (const [cls, x, y] of FIELD_ITEMS) item(cls, x, y);
  // spectators and intermission: above mid-field, looking at the red fort
  map.entity('info_player_intermission', { origin: [0, -1200, 700], angles: [20, 60, 0] });
  // sunlight comes from the sky shader; a little fill
  map.entity('light', { origin: [0, 0, 1200], light: 2500 });
  return {
    map, aas: false,
    manifest: { features: ['terrain', 'nav'] },
    files: { 'scripts/oax_outdoor_ctf.shader': shaders, ...t.files(), ...foliageFiles() },
  };
}
