// oax_box: the smallest test map. One lit room with two spawn points; used
// to prove the map pipeline (mapwriter, q3map2, bspc, BSPX manifest) and
// the engine-extension plumbing end to end.

import { MapFile, room } from '../mapwriter.mjs';

export function build() {
  const map = new MapFile({ message: 'oax test: box' });
  map.brush(room([-512, -512, 0], [512, 512, 256], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  map.entity('info_player_deathmatch', { origin: [-256, 0, 32], angle: 0 });
  map.entity('info_player_deathmatch', { origin: [256, 0, 32], angle: 180 });
  map.entity('light', { origin: [0, 0, 200], light: 600 });
  map.entity('light', { origin: [-384, -384, 128], light: 300 });
  map.entity('light', { origin: [384, 384, 128], light: 300 });
  return { map, manifest: { features: [] } };
}
