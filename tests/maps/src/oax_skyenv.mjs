// oax_skyenv: the sidecar's environment keys on a stock-style map: oax_sundisc
// (the sun drawn in the sky) and oax_bloom (bloom while r_oaxBloom is 2). A
// closed room with a sky ceiling whose shader has a sun (q3map_sun, azimuth 90,
// elevation 70) and the stock `sun` shader; the sidecar carries the two keys.
// Without the sidecar (com_oaxEnhanced 0) the map is a plain room with a sky.

import { MapFile, box, room } from '../mapwriter.mjs';

export const SIDECAR = `// the enhanced content's environment keys
{
"classname" "worldspawn"
"oax_sundisc" "0.12"
"oax_bloom" "1"
}
`;

const shaders = `// oax_skyenv shaders (tests/maps/src/oax_skyenv.mjs)
textures/oax_skyenv/sky
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	q3map_sun 1 0.92 0.8 200 90 70
	skyparms env/sky1/sky001 - -
}

`;

export function build() {
  const map = new MapFile({ message: 'oax test: sky environment' });
  map.brush(room([-512, -512, 0], [512, 512, 256], { floor: 'base_floor/clang_floor', ceiling: 'oax_skyenv/sky', walls: 'gothic_wall/oct20c' }));
  map.brush(box([-48, -48, 0], [48, 48, 96], 'base_wall/basewall01'));
  map.entity('info_player_deathmatch', { origin: [-384, -384, 32], angle: 45 });
  map.entity('light', { origin: [0, 0, 200], light: 500 });
  return { map, files: { 'maps/oax_skyenv.oaxmap': SIDECAR, 'scripts/oax_skyenv.shader': shaders }, manifest: { features: [] } };
}
