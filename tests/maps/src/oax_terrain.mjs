// oax_terrain: a heightmap terrain (misc_oax_terrain) in a sky box, for the
// terrain collision and rendering tests. Hills, a ridge, a steep bank too
// steep to walk (normal z < 0.7), a flat plateau and a bowl, so movement
// traces cross every kind of slope. No AAS (bspc sees only the floor under
// the terrain). Grass, and trees with solid trunks, on the gentle ground.

import { MapFile, box } from '../mapwriter.mjs';
import { Terrain } from '../lib/terrain.mjs';
import { foliageFiles } from '../lib/foliage-art.mjs';

export const HALF = 1600;
export const TERRAIN = { origin: [-1536, -1536, -256], cellSize: 48, samples: 65, heightScale: 1 / 64 };

export function makeTerrain() {
  const t = new Terrain({ name: 'oax_terrain', origin: TERRAIN.origin, cellSize: TERRAIN.cellSize, samplesX: TERRAIN.samples, samplesY: TERRAIN.samples, heightScale: TERRAIN.heightScale });
  t.shape((x, y) => {
    let z = 0;
    z += 90 * Math.sin(x / 410) * Math.cos(y / 330);                 // rolling hills
    z += 140 * Math.exp(-((x - 700) ** 2 + (y + 500) ** 2) / (2 * 260 ** 2));   // a hill
    z += 220 * Math.max(0, 1 - Math.abs(x + 650) / 120) * (y > 300 ? 1 : 0);    // a steep bank
    z -= 120 * Math.exp(-((x + 300) ** 2 + (y + 800) ** 2) / (2 * 300 ** 2));    // a bowl
    if (Math.abs(x - 500) < 260 && Math.abs(y - 700) < 260) z = 60;            // a plateau
    return z;
  });
  t.paint((x, y, z, slope) => ({
    // grass on gentle ground, dirt on slopes, rock on cliffs, sand in the bowl
    splat: [slope > 0.9 ? 1 : 0.1, slope > 0.75 ? 0.3 : 0.8, slope < 0.75 ? 1.5 : 0, z < -60 ? 1.2 : 0],
    density: [slope > 0.9 && z > -60 ? 1 : 0, slope > 0.92 && z > -40 ? 0.6 : 0, 0, 0],
  }));
  return t;
}

const shaders = `// oax_terrain test map shaders (tests/maps/src/oax_terrain.mjs)
textures/oax_terrain/sky
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	q3map_sun 1 0.95 0.85 220 35 55
	q3gl2_sun 1 0.95 0.85 220 35 55 0.35
	skyparms env/sky1/sky001 - -
}
`;

export function build() {
  const t = makeTerrain();
  const map = new MapFile({ message: 'oax test: terrain', _ambient: 30 });
  const w = 16, sky = 'oax_terrain/sky', wall = 'gothic_block/blocks18c';
  // a sealed box: walls, sky ceiling, a floor far below the terrain
  map.brush(
    box([-HALF - w, -HALF - w, -640 - w], [HALF + w, HALF + w, -640], 'base_floor/concrete'),
    box([-HALF - w, -HALF - w, 1200], [HALF + w, HALF + w, 1200 + w], sky),
    box([-HALF - w, -HALF - w, -640], [-HALF, HALF + w, 1200], wall),
    box([HALF, -HALF - w, -640], [HALF + w, HALF + w, 1200], wall),
    box([-HALF, -HALF - w, -640], [HALF, -HALF, 1200], wall),
    box([-HALF, HALF, -640], [HALF, HALF + w, 1200], wall),
  );
  map.entity('misc_oax_terrain', t.entityKeys({
    seed: 7,
    bottom: -560,
    layers: [
      { shader: 'textures/acc_dm3/grass', scale: 384 },
      { shader: 'textures/base_floor/dirt', scale: 256 },
      { shader: 'textures/cosmo_block/rock01', scale: 320 },
      { shader: 'textures/cosmo_floor/sand01', scale: 256 },
    ],
    foliage: [
      'grass textures/oax_terrain/grassblades 0 6 18 30 900 1400',
      'tree textures/oax_terrain/tree 1 0.06 220 300 3000 3600 10 120 0.9',
    ],
  }));
  const spawn = (x, y, yaw) => map.entity('info_player_deathmatch', { origin: [x, y, Math.ceil(t.groundZ(x, y)) + 32], angle: yaw });
  spawn(-1000, -1000, 45);
  spawn(1000, 1000, 225);
  spawn(0, 0, 0);
  map.entity('light', { origin: [0, 0, 900], light: 1200 });
  return { map, aas: false, manifest: { features: ['terrain'] }, files: { 'scripts/oax_terrain.shader': shaders, ...t.files(), ...foliageFiles() } };
}
