// oax_overlay: the map sidecar (docs/map-format.md, "Map overlay"). A plain
// lightmapped room (no oax_lighting, no realtime light: a stock-style map) with
// maps/oax_overlay.oaxmap shipped beside it, which switches the map to hybrid
// lighting and adds one bright realtime light over the box. With
// com_oaxEnhanced 1 the light is there; with 0 the map loads as it came. The
// sidecar also sets two renderer keys: oax_lightmapscale 0.5 (the baked light
// halved) and oax_subdivisions 64 (the curved arch in one corner tessellates
// coarsely instead of at r_subdivisions' default).

import { MapFile, Patch, box, room } from '../mapwriter.mjs';

export const OVERLAY = `// the sidecar: worldspawn keys are merged, other entities appended
{
"classname" "worldspawn"
"oax_lighting" "hybrid"
"oax_ambient" "0 0 0"
"oax_shadowmode" "maps"
"oax_lightmapscale" "0.5"
"oax_subdivisions" "64"
}
{
"classname" "rtlight"
"origin" "200 200 160"
"light_radius" "500 500 400"
"_color" "2.4 0.8 0.4"
}
`;

export function build() {
  const map = new MapFile({ message: 'oax test: map overlay' });
  map.brush(room([-512, -512, 0], [512, 512, 256], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  map.brush(box([-48, -48, 0], [48, 48, 96], 'base_wall/basewall01'));
  // a quarter-pipe arch in the +x -y corner, 3 x 3 control points
  const profile = [[0, 0], [0, 120], [120, 120]];
  map.brush(new Patch([0, 1, 2].map((r) => profile.map(([px, pz], c) => [300 + px, -340 + r * 40, pz, c * 0.5, r * 0.5])), 'base_wall/basewall01'));
  map.entity('info_player_deathmatch', { origin: [-384, -384, 32], angle: 45 });
  map.entity('light', { origin: [0, 0, 220], light: 500 });
  return { map, files: { 'maps/oax_overlay.oaxmap': OVERLAY }, manifest: { features: [] } };
}
