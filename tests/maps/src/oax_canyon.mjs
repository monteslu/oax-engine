// oax_canyon: a large vehicle CTF map in a winding canyon. The play space is
// one heightmap terrain (misc_oax_terrain, 16384 x 10240, 32-unit cells):
// a meandering valley floor of cracked earth, sand and grass, enclosed by
// irregular rock walls that rise out of the terrain itself (ledges,
// uneven ridge lines, no straight edges), two side gullies, two mesas with
// one walkable flank, scattered boulders. The sealing box around it is sky,
// hidden behind the ridges. Two forts at the canyon ends; buggies and hover
// craft under the vehicle rule (g_oaxVehicles 1).
//
// The layout is point-symmetric about the map centre (h(x, y) = h(-x, -y)),
// so both teams get the same canyon. No AAS: bots use the navmesh.
//
// A playable map, not a test fixture: no test reads its numbers.

import { MapFile, Brush, Face, box } from '../mapwriter.mjs';
import { Terrain } from '../lib/terrain.mjs';
import { foliageFiles } from '../lib/foliage-art.mjs';
import { vehicleFiles, addVehicle } from '../lib/vehicles.mjs';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// the map's art (tree models, grass cards; licences in assets/canyon/licenses)
const ASSETS = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'assets', 'canyon');
function assetFiles() {
  const out = {};
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      const rel = path.relative(ASSETS, p);
      // licences ship in the package as credits (CC-BY-SA art needs them)
      if (e.isDirectory()) walk(p); else out[rel.startsWith('licenses') ? rel.replace(/^licenses/, 'credits/oax_canyon') : rel] = fs.readFileSync(p);
    }
  };
  walk(ASSETS);
  return out;
}

export const FIELD = { x0: -8192, x1: 8192, y0: -5120, y1: 5120 };
export const CELL = 32;
const BASE_X = 6400;                 // fort centre x for red (+) and blue (-)
const PLATFORM_Z = 64;

// ---- deterministic value noise -------------------------------------------
function hash2(i, j) {
  let h = (i * 374761393 + j * 668265263) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}
function vnoise(x, y) {
  const i = Math.floor(x), j = Math.floor(y), u = x - i, v = y - j;
  const s = (t) => t * t * (3 - 2 * t);
  const a = hash2(i, j), b = hash2(i + 1, j), c = hash2(i, j + 1), d = hash2(i + 1, j + 1);
  return a + (b - a) * s(u) + (c - a) * s(v) + (a - b - c + d) * s(u) * s(v);
}
// fractal noise in [-1, 1]
function fbm(x, y, oct = 5) {
  let sum = 0, amp = 1, norm = 0, f = 1;
  for (let o = 0; o < oct; o++) { sum += amp * (vnoise(x * f + o * 17.3, y * f - o * 9.1) * 2 - 1); norm += amp; amp *= 0.5; f *= 2.03; }
  return sum / norm;
}
const smooth = (e0, e1, x) => { const t = Math.min(1, Math.max(0, (x - e0) / (e1 - e0))); return t * t * (3 - 2 * t); };
const bump = (x, y, cx, cy, r) => Math.exp(-((x - cx) ** 2 + (y - cy) ** 2) / (2 * r * r));

// ---- the canyon ------------------------------------------------------------
// centre line of the main valley (an odd function: point symmetry)
export const center = (x) => 1500 * Math.sin(x / 2900) + 420 * Math.sin(x / 1250);
// half width of the valley floor
const halfWidth = (x) => 820 + 260 * Math.cos(x / 1700) + 650 * bump(x, 0, 0, 0, 1300) + 700 * smooth(4800, 5800, Math.abs(x));

// side gullies: a short branch valley from the main one, mirrored
const GULLY = [[3600, 0], [4300, -3600]];      // from (x, center(x) + 0) to the second point, red side
function segDist(px, py, ax, ay, bx, by) {
  const vx = bx - ax, vy = by - ay;
  const t = Math.max(0, Math.min(1, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)));
  return Math.hypot(px - ax - t * vx, py - ay - t * vy);
}

// mesas: a plateau with steep sides, one flank a long walkable slope
const MESAS = [[2700, 1, 380, 520]];           // x, side of the valley (+1 = +y), height, radius; red side

// shallow pools: a level apron at z 0, a basin under it with a ragged
// shore, water 14 under the apron. Both pools (red and its blue mirror) share
// one water plane, so the renderer reflects in both at once.
export const POOL = { x: 2150, frac: -0.42, rx: 560, ry: 380, depth: 72, apron: 460, waterZ: -14 };
export const poolCenter = () => [POOL.x, center(POOL.x) + POOL.frac * halfWidth(POOL.x)];
// distance from the pool centre in units of its radius, with a ragged shore
function poolR(x, y) {
  const [px, py] = poolCenter();
  const dx = (x - px) / POOL.rx, dy = (y - py) / POOL.ry;
  return Math.hypot(dx, dy) * (1 + 0.12 * fbm(x / 160 + 9, y / 160 - 4, 2));
}

function heightCanon(x, y) {
  // floor: gentle undulation
  let floor = 70 * fbm(x / 900, y / 900, 4) + 25 * fbm(x / 260, y / 260, 3);
  // boulders: sparse bumps on the floor
  const bn = fbm(x / 300 + 40, y / 300 - 11, 3);
  floor += 260 * Math.max(0, bn - 0.42);

  // distance out of the valley floor: main valley or a gully, whichever is nearer
  const cy = center(x);
  let e = Math.abs(y - cy) - halfWidth(x);
  {
    const [[ax], [bx, by]] = GULLY;
    e = Math.min(e, segDist(x, y, ax, center(ax), bx, by) - (520 + 120 * Math.sin(y / 500)));
  }
  e -= 280 * fbm(x / 1100 + 5, y / 1100 + 3, 4);          // ragged wall line

  // walls: rise fast, then keep climbing more slowly; ledges from terracing
  const H = 1500 + 650 * fbm(x / 1800 - 7, y / 1800 + 2, 4);
  let wall = 0;
  if (e > 0) {
    const run = 420 + 180 * fbm(x / 700, y / 700, 3);
    wall = H * (1 - Math.exp(-e / run));
    const step = 210 + 60 * fbm(x / 1300, y / 1300, 2);
    const terraced = Math.floor(wall / step) * step + step * smooth(0.65, 1, (wall % step) / step);
    wall += 0.45 * (terraced - wall);
    wall += 70 * fbm(x / 120, y / 120, 3) * smooth(0, 300, e);   // rock roughness on the faces
  }
  let z = floor + wall;

  // mesas
  for (const [mx, side, mh, mr] of MESAS) {
    const py = center(mx) + side * (halfWidth(mx) * 0.45);
    let dx = x - mx;
    const dy = y - py;
    // the walkable flank faces the centre of the map: stretched distance there
    if (dx < 0) dx /= 3.2;
    const d = Math.hypot(dx, dy);
    z = Math.max(z, floor + mh * smooth(mr + 140, mr - 30, d) + 20 * fbm(x / 150, y / 150, 2));
  }

  // the pool: a level apron, then the basin
  {
    const r = poolR(x, y);
    const apron = smooth(1 + POOL.apron / POOL.rx, 1, r);
    z = z * (1 - apron);
    z -= POOL.depth * smooth(1.0, 0.55, r);
  }
  // flat ground round the fort
  {
    const f = smooth(1500, 900, Math.hypot(x - BASE_X, (y - center(BASE_X)) * 1.2));
    z = z * (1 - f);
  }
  // the outer rim, after the flattening: the terrain rises well above any
  // line of sight at the map edge, behind the forts too
  const edge = Math.min(x - FIELD.x0, FIELD.x1 - x, y - FIELD.y0, FIELD.y1 - y);
  const rim = (1 - smooth(0, 1400, edge)) * (2300 + 300 * fbm(x / 900, y / 900, 3));
  return rim > 0 ? Math.max(z, floor + rim) : z;
}
// point symmetry: the blue half is the red half turned 180 degrees. Across
// the centre line the two are blended, so the ground has no seam:
// h(x, y) = w(x) c(x, y) + (1 - w(x)) c(-x, -y) with w(-x) = 1 - w(x).
const BLEND = 900;
export function canyonHeight(x, y) {
  if (x >= BLEND) return heightCanon(x, y);
  if (x <= -BLEND) return heightCanon(-x, -y);
  const w = smooth(-BLEND, BLEND, x);
  return w * heightCanon(x, y) + (1 - w) * heightCanon(-x, -y);
}

export function makeTerrain(clear) {
  const t = new Terrain({
    name: 'oax_canyon', origin: [FIELD.x0, FIELD.y0, -1024], cellSize: CELL,
    samplesX: (FIELD.x1 - FIELD.x0) / CELL + 1, samplesY: (FIELD.y1 - FIELD.y0) / CELL + 1, heightScale: 1 / 16,
  });
  t.shape(canyonHeight);
  t.paint((x, y, z, slope) => {
    const nearBase = [1, -1].some((s) => Math.hypot(x - s * BASE_X, y - center(s * BASE_X)) < 1100);
    const nearItem = clear.some(([cx, cy]) => Math.hypot(x - cx, y - cy) < 220);
    const sx = x >= 0 ? x : -x, sy = x >= 0 ? y : -y;
    const n = fbm(sx / 600 + 3, sy / 600 - 8, 3);
    const steep = slope < 0.82, ledge = z > 250;
    const wet = z < POOL.waterZ + 12;                // in or at the edge of a pool: no foliage
    return {
      // layers: 0 grass, 1 cracked earth, 2 rock, 3 sand
      splat: [
        !steep && !ledge && n > 0.1 ? 1.4 : 0,
        !steep ? 1 + (nearBase ? 0.6 : 0) : 0,
        steep ? 3 : ledge ? 0.9 : 0,
        !steep && n < -0.15 ? 1.3 : 0,
      ],
      density: [
        !steep && !ledge && !wet && n > 0.05 ? 0.9 : 0,
        slope > 0.92 && !nearBase && !nearItem && !wet && n > 0.25 && z < 400 ? 0.55 : 0,
        0, 0],
    };
  });
  return t;
}

// ---- forts -----------------------------------------------------------------
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

// one fort at the canyon end; s = +1 red (x > 0), -1 blue. Local coordinates:
// u along x towards the back wall (u = 0 at the ramps' foot), v across.
function fort(map, s, cx, cy) {
  // weathered light stone: the gothic set averages ~12% brightness and reads
  // black in a canyon wall's shadow
  const stone = 'acc_dm5/wall_dirty', top = 'acc_dm5/stntiles_dirty', wall = 'acc_dm5/stnlarge_dirty', trim = 'acc_dm5/stonesteps_dirty';
  const X = (a, b) => (s > 0 ? [cx + a, cx + b] : [cx - b, cx - a]);
  const Y = (a, b) => [cy + a, cy + b];
  const W = 420;
  map.brush(box([X(-200, 400)[0], cy - W, -32], [X(-200, 400)[1], cy + W, PLATFORM_Z], { top, sides: stone, bottom: 'common/caulk' }));
  const [ra, rb] = X(-392, -200);
  for (const [y0, y1] of [[-380, -200], [200, 380]]) map.brush(ramp(ra, rb, ...Y(y0, y1), 0, PLATFORM_Z, s, stone));
  const [bx0, bx1] = X(368, 400);
  map.brush(box([bx0, cy - W, PLATFORM_Z], [bx1, cy + W, PLATFORM_Z + 256], wall));
  for (const [y0, y1] of [[-W, -W + 32], [W - 32, W]]) {
    const [sx0, sx1] = X(40, 368);
    map.brush(box([sx0, cy + y0, PLATFORM_Z], [sx1, cy + y1, PLATFORM_Z + 256], wall));
  }
  const [fx0, fx1] = X(40, 400);
  map.brush(box([fx0, cy - W, PLATFORM_Z + 256], [fx1, cy + W, PLATFORM_Z + 272], { bottom: trim, top: stone, sides: stone }));
  const [px0, px1] = X(-140, -124);
  map.brush(box([px0, cy - 120, PLATFORM_Z], [px1, cy + 120, PLATFORM_Z + 44], stone));
  map.entity('light', { origin: [s * (Math.abs(cx) + 230), cy, PLATFORM_Z + 220], light: 600, _color: s > 0 ? [1, 0.5, 0.45] : [0.45, 0.55, 1] });
  return { flag: [s * (Math.abs(cx) + 260), cy, PLATFORM_Z + 24] };
}

// ---- items -----------------------------------------------------------------
// red-side items as [classname, x, fraction across the valley (-1..1)]; blue
// gets the point-mirrored copy. Centre items are listed once.
const SIDE_ITEMS = [
  ['weapon_shotgun', 4700, -0.5], ['ammo_shells', 4700, 0.5], ['item_health', 5650, 0], ['ammo_rockets', 4500, 0.6],
  ['weapon_plasmagun', 4300, -0.55], ['ammo_cells', 4100, -0.4], ['item_armor_shard', 3300, 0.35], ['item_health', 3300, -0.35],
  ['weapon_lightning', 1800, 0.55], ['ammo_lightning', 1700, 0.5], ['item_health_large', 1400, -0.5],
];
const CENTER_ITEMS = [['weapon_rocketlauncher', 0, 0], ['item_armor_body', 0, 0.55], ['item_quad', 0, -0.55]];

const valleyPoint = (x, frac) => [x, center(x) + frac * halfWidth(x) * 0.62];

function itemSpots() {
  const spots = [];
  for (const [cls, x, f] of SIDE_ITEMS) {
    const [px, py] = valleyPoint(x, f);
    spots.push([cls, px, py], [cls, -px, -py]);
  }
  for (const [cls, x, f] of CENTER_ITEMS) spots.push([cls, ...valleyPoint(x, f)]);
  // a railgun on each mesa top
  for (const [mx, side, , ] of MESAS) for (const s of [1, -1]) {
    const px = s * mx;
    spots.push(['weapon_railgun', px, center(px) + s * side * (halfWidth(px) * 0.45)]);
  }
  // health and a hover craft spot in each gully's dead end
  for (const s of [1, -1]) {
    const [, [gx, gy]] = GULLY;
    spots.push(['item_health_mega', s * (gx - 150), s * (gy + 700)]);
  }
  return spots;
}

const shaders = `// oax_canyon map shaders (tests/maps/src/oax_canyon.mjs)
textures/oax_canyon/sky
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	q3map_sun 1 0.92 0.8 220 35 40
	q3gl2_sun 1 0.92 0.8 240 35 40 0.4
	skyparms env/sky1/sky001 - -
}

// the grass blades clamped: with the default repeat, the bottom (root) row
// bleeds into the top edge of every blade quad as floating green speckles
textures/oax_canyon/grass_tuft
{
	{
		clampmap textures/oax_canyon/grass_04.png
		alphaFunc GE128
	}
}

textures/oax_canyon/grass_blades
{
	{
		clampmap textures/oax_canyon/grass_05.png
		alphaFunc GE128
	}
}

// the water volume's hidden faces: water contents, nothing drawn
textures/oax_canyon/water_hidden
{
	qer_editorimage textures/liquids/vorwater.tga
	surfaceparm nodraw
	surfaceparm nonsolid
	surfaceparm trans
	surfaceparm water
}

textures/oax_canyon/water
{
	qer_editorimage textures/liquids/vorwater.tga
	surfaceparm nomarks
	surfaceparm trans
	surfaceparm nonsolid
	surfaceparm water
	surfaceparm nolightmap
	cull disable
	oaxWater
	// a still pond: strong reflection, gentle ripples
	oaxWaterParm tint 0.09 0.16 0.13
	oaxWaterParm density 0.015
	oaxWaterParm scale 0.005
	oaxWaterParm speed 0.02 0.015
	oaxWaterParm distortion 0.008
	oaxWaterParm reflectivity 1.0
	oaxWaterParm fresnel 0.3
	oaxWaterParm waves 0.15
	oaxWaterParm foam 16 0.6
	oaxWaterParm caustics 0.9 0.006
	{
		map textures/liquids/vorwater.tga
		blendfunc filter
		tcmod scroll 0.01 0.008
	}
}
`;

export function build() {
  const items = itemSpots();
  const spawnXY = [];
  for (const s of [1, -1]) for (const [u, v] of [[-650, -600], [-650, 600], [-800, 0], [-450, -350], [-450, 350]]) {
    const x = s * (BASE_X + u);
    spawnXY.push([x, center(x) + s * v]);
  }
  const t = makeTerrain([...items.map(([, x, y]) => [x, y]), ...spawnXY]);
  const map = new MapFile({
    message: 'oax canyon', _ambient: 30,
    // the outdoor environment (renderergl2 tr_oax_env.c)
    oax_wind: '0.25 0.3 30', oax_foliageaa: 1, oax_groundfx: 1, oax_underwaterfog: 1,
    oax_atmosphere: '1.35 1.45 1.65 0.00025 0.0008 -100 1.2',
    oax_grade: '1.08 1.06 1.03 1.0 0.96 0.35',
    oax_clouds: '1800 50 20 0.45 0.55',
  });
  const w = 32, sky = 'oax_canyon/sky';
  const { x0, x1, y0, y1 } = FIELD;
  const zFloor = -1100, zTop = 3600;
  // sealed: floor far below, sky above and on every side (hidden by the rim)
  map.brush(
    box([x0 - w, y0 - w, zFloor - w], [x1 + w, y1 + w, zFloor], 'common/caulk'),
    box([x0 - w, y0 - w, zTop], [x1 + w, y1 + w, zTop + w], sky),
    box([x0 - w, y0 - w, zFloor], [x0, y1 + w, zTop], sky),
    box([x1, y0 - w, zFloor], [x1 + w, y1 + w, zTop], sky),
    box([x0, y0 - w, zFloor], [x1, y0, zTop], sky),
    box([x0, y1, zFloor], [x1, y1 + w, zTop], sky),
  );
  map.entity('misc_oax_terrain', t.entityKeys({
    seed: 4242,
    bottom: -1060,
    triplanar: true, macro: true, detail: true,
    layers: [
      { shader: 'textures/acc_dm3/grass', scale: 384 },
      { shader: 'textures/acc_dm3/sp_ground', scale: 448 },
      { shader: 'textures/cosmo_block/rock05', scale: 512 },
      { shader: 'textures/acc_dm5/sand', scale: 384 },
    ],
    foliage: [
      // green grass: tall blades and lush tufts (Yughues, CC0)
      'grass textures/oax_canyon/grass_blades 0 3.2 26 42 1100 1600',
      'grass textures/oax_canyon/grass_tuft 0 1.6 20 32 1100 1600',
      // ez-tree conifers (ponderosa x2, pinyon, juniper) and dead snags; models
      // under models/oax/foliage (misc/tools/glb-to-md3.mjs; licences in their folder)
      'model models/oax/foliage/conifer.md3 1 0.05 260 520 5000 5800 10 140 0.9 4',
      'model models/oax/foliage/snag.md3 1 0.012 280 440 5000 5800 8 140 0.9 1',
    ],
  }));

  const ground = (x, y) => Math.ceil(t.groundZ(x, y));
  for (const s of [1, -1]) {
    const team = s > 0 ? 'red' : 'blue';
    const cx = s * BASE_X, cy = center(cx);
    const { flag } = fort(map, s, cx, cy);
    map.entity(`team_CTF_${team}flag`, { origin: flag });
    for (const [u, v] of [[0, -220], [0, 220], [180, -200], [180, 200]]) {
      const o = [s * (BASE_X + u), cy + v, PLATFORM_Z + 32];
      map.entity(`team_CTF_${team}player`, { origin: o, angle: s > 0 ? 180 : 0 });
      map.entity(`team_CTF_${team}spawn`, { origin: o, angle: s > 0 ? 180 : 0 });
    }
    for (const [x, y] of spawnXY.filter(([x]) => Math.sign(x) === s)) {
      map.entity(`team_CTF_${team}spawn`, { origin: [x, y, ground(x, y) + 32], angle: s > 0 ? 180 : 0 });
    }
    map.entity('item_armor_combat', { origin: [s * (BASE_X + 300), cy - 260, PLATFORM_Z + 24] });
    map.entity('item_health_large', { origin: [s * (BASE_X + 300), cy + 260, PLATFORM_Z + 24] });

    // vehicles: two buggies and a hover craft in front of the fort, a hover
    // craft in the gully
    // across the valley as a fraction of its width there, so nothing lands on a wall
    for (const [type, u, v] of [['apc', -1150, -0.5], ['apc', -1150, 0.5], ['hovertank', -1450, 0]]) {
      const x = s * (BASE_X + u), y = center(x) + s * v * halfWidth(x);
      addVehicle(map, type, [x, y, ground(x, y) + (type === 'apc' ? 72 : 60)], s > 0 ? 180 : 0, 20);
    }
    const [, [gx, gy]] = GULLY;
    const hx = s * (gx - 150), hy = s * (gy + 1100);
    addVehicle(map, 'hovertank', [hx, hy, ground(hx, hy) + 60], s > 0 ? 200 : 20, 30);
  }
  for (const [cls, x, y] of items) map.entity(cls, { origin: [x, y, ground(x, y) + 24] });
  // the pools' water: a box over each basin, its top under the apron (the
  // terrain hides the parts of the box outside the shore)
  for (const s of [1, -1]) {
    const [px, py] = poolCenter();
    const cx = s * px, cy = s * py, hx = POOL.rx * 1.25, hy = POOL.ry * 1.25;
    // only the top is water to look at; the sides and bottom draw nothing
    map.brush(box([cx - hx, cy - hy, POOL.waterZ - POOL.depth - 64], [cx + hx, cy + hy, POOL.waterZ],
      { top: 'oax_canyon/water', sides: 'oax_canyon/water_hidden', bottom: 'oax_canyon/water_hidden' }));
  }

  map.entity('info_player_intermission', { origin: [0, -2600, 1500], angles: [22, 70, 0] });
  map.entity('light', { origin: [0, 0, 3000], light: 4000 });
  return {
    map, aas: false, vis: 'fast',
    manifest: { features: ['terrain', 'nav', 'vehicles'] },
    files: {
      'scripts/oax_canyon.shader': shaders, ...t.files(), ...foliageFiles(), ...vehicleFiles(),
      ...assetFiles(),
    },
  };
}
