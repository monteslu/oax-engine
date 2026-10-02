// oax_occlusion: a terrain with a tall ridge across its middle, for the
// terrain occlusion query test. From the low south side the ridge hides
// every chunk (and its trees) north of it; a camera rising over the ridge
// reveals them, which is where a stale or over-eager query would pop.

import { MapFile, box } from '../mapwriter.mjs';
import { Terrain } from '../lib/terrain.mjs';
import { foliageFiles } from '../lib/foliage-art.mjs';

export const HALF = 1600;

export function makeTerrain() {
  const t = new Terrain({ name: 'oax_occlusion', origin: [-1536, -1536, -128], cellSize: 32, samplesX: 97, samplesY: 97, heightScale: 1 / 64 });
  t.shape((x, y) => 760 * Math.exp(-((y / 170) ** 2)) + 20 * Math.sin(x / 300));
  t.paint((x, y, z, slope) => ({
    splat: [slope > 0.85 ? 1 : 0.2, 0.3, slope < 0.8 ? 1.2 : 0, 0],
    density: [y < -300 && slope > 0.9 ? 1 : 0, y > 300 && slope > 0.9 ? 1 : 0, 0, 0],
  }));
  return t;
}

const shaders = `// oax_occlusion test map shaders (tests/maps/src/oax_occlusion.mjs)
textures/oax_occlusion/sky
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
  const map = new MapFile({ message: 'oax test: terrain occlusion', _ambient: 30 });
  const w = 16, sky = 'oax_occlusion/sky', wall = 'gothic_block/blocks18c';
  map.brush(
    box([-HALF - w, -HALF - w, -640 - w], [HALF + w, HALF + w, -640], 'base_floor/concrete'),
    box([-HALF - w, -HALF - w, 1400], [HALF + w, HALF + w, 1400 + w], sky),
    box([-HALF - w, -HALF - w, -640], [-HALF, HALF + w, 1400], wall),
    box([HALF, -HALF - w, -640], [HALF + w, HALF + w, 1400], wall),
    box([-HALF, -HALF - w, -640], [HALF, -HALF, 1400], wall),
    box([-HALF, HALF, -640], [HALF, HALF + w, 1400], wall),
  );
  map.entity('misc_oax_terrain', t.entityKeys({
    seed: 11,
    bottom: -560,
    layers: [
      { shader: 'textures/acc_dm3/grass', scale: 384 },
      { shader: 'textures/base_floor/dirt', scale: 256 },
      { shader: 'textures/cosmo_block/rock01', scale: 320 },
    ],
    foliage: [
      'grass textures/oax_terrain/grassblades 0 3 18 30 900 1400',
      'tree textures/oax_terrain/tree 1 0.08 220 300 3000 3600',
    ],
  }));
  map.entity('info_player_deathmatch', { origin: [0, -1300, Math.ceil(t.groundZ(0, -1300)) + 32], angle: 90 });
  map.entity('info_player_deathmatch', { origin: [0, 1300, Math.ceil(t.groundZ(0, 1300)) + 32], angle: 270 });
  map.entity('light', { origin: [0, 0, 1300], light: 1200 });
  return { map, aas: false, manifest: { features: ['terrain'] }, files: { 'scripts/oax_occlusion.shader': shaders, ...t.files(), ...foliageFiles() } };
}
