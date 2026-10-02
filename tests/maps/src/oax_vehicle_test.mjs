// oax_vehicle_test: a small field for the vehicle tests (phase 8). A
// heightmap terrain of gentle hills (misc_oax_terrain) in a sealed sky box,
// a brush ramp and a block in the middle, one player spawn with a buggy and
// a hover craft parked in front of it. The vehicle-parity test enters the
// buggy from the spawn with a pad script and drives it over terrain, the
// ramp and the hills, on the native client and on the cart.

import { MapFile, Brush, Face, box } from '../mapwriter.mjs';
import { Terrain } from '../lib/terrain.mjs';
import { vehicleFiles, addVehicle } from '../lib/vehicles.mjs';

export const FIELD = { x0: -2048, x1: 2048, y0: -1024, y1: 1024 };
export const CELL = 64;
export const SPAWN = [-1700, 0];
export const BUGGY = [-1600, 0];
export const HOVER = [-1600, 260];

export function fieldHeight(x, y) {
  // flat around the spawn, rolling further out
  const flat = Math.min(1, Math.max(0, (x + 1300) / 400));
  return flat * (40 * Math.sin(x / 300) * Math.cos(y / 260) + 30 * Math.sin((x - y) / 410));
}

export function makeTerrain() {
  const t = new Terrain({
    name: 'oax_vehicle_test', origin: [FIELD.x0, FIELD.y0, -256], cellSize: CELL,
    samplesX: (FIELD.x1 - FIELD.x0) / CELL + 1, samplesY: (FIELD.y1 - FIELD.y0) / CELL + 1, heightScale: 1 / 32,
  });
  t.shape((x, y) => fieldHeight(x, y));
  t.paint((x, y, z, slope) => ({ splat: [1, slope > 0.85 ? 0.2 : 0.8, 0, 0], density: [0, 0, 0, 0] }));
  return t;
}

// a wedge rising along +x from z0 to z1 over [xa, xb]
function ramp(xa, xb, y0, y1, z0, z1, tex) {
  const L = xb - xa, H = z1 - z0;
  const n = [-H, 0, L];
  const d = n[0] * xa + n[2] * z0;
  return new Brush([
    new Face([0, 0, -1], -(z0 - 64), tex),
    new Face(n, d, tex),
    new Face([1, 0, 0], xb, tex),
    new Face([0, 1, 0], y1, tex),
    new Face([0, -1, 0], -y0, tex),
  ]).check();
}

const shaders = `// oax_vehicle_test map shaders (tests/maps/src/oax_vehicle_test.mjs)
textures/oax_vehicle_test/sky
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	q3map_sun 1 0.95 0.85 220 35 55
	q3gl2_sun 1 0.95 0.85 3 35 55 0.35
	skyparms env/sky1/sky001 - -
}
`;

export function build() {
  const t = makeTerrain();
  const map = new MapFile({ message: 'oax test: vehicles', _ambient: 30 });
  const w = 32, sky = 'oax_vehicle_test/sky', wall = 'gothic_block/blocks18c', stone = 'gothic_block/blocks18c';
  const { x0, x1, y0, y1 } = FIELD;
  const zFloor = -400, zTop = 1200;
  map.brush(
    box([x0 - w, y0 - w, zFloor - w], [x1 + w, y1 + w, zFloor], 'common/caulk'),
    box([x0 - w, y0 - w, zTop], [x1 + w, y1 + w, zTop + w], sky),
    box([x0 - w, y0 - w, zFloor], [x0, y1 + w, zTop], wall),
    box([x1, y0 - w, zFloor], [x1 + w, y1 + w, zTop], wall),
    box([x0, y0 - w, zFloor], [x1, y0, zTop], wall),
    box([x0, y1, zFloor], [x1, y1 + w, zTop], wall),
  );
  map.entity('misc_oax_terrain', t.entityKeys({
    seed: 11,
    bottom: -320,
    layers: [
      { shader: 'textures/acc_dm3/grass', scale: 384 },
      { shader: 'textures/base_floor/dirt', scale: 256 },
    ],
    foliage: [],
  }));
  // a ramp up onto a platform, and a block to drive around
  const ground = (x, y) => Math.ceil(t.groundZ(x, y));
  map.brush(ramp(-600, -300, -200, 200, -40, 72, stone));
  map.brush(box([-300, -200, -100], [100, 200, 72], { top: 'base_floor/clang_floor', sides: stone, bottom: 'common/caulk' }));
  map.brush(box([700, 300, -100], [860, 460, 160], stone));

  map.entity('info_player_deathmatch', { origin: [SPAWN[0], SPAWN[1], ground(...SPAWN) + 40], angle: 0 });
  addVehicle(map, 'buggy', [BUGGY[0], BUGGY[1], ground(...BUGGY) + 64], 0, 5);
  addVehicle(map, 'hover', [HOVER[0], HOVER[1], ground(...HOVER) + 64], 0, 5);
  map.entity('light', { origin: [0, 0, 1000], light: 2500 });
  return {
    map, aas: false,
    manifest: { features: ['terrain', 'vehicles'] },
    files: { 'scripts/oax_vehicle_test.shader': shaders, ...t.files(), ...vehicleFiles() },
  };
}
