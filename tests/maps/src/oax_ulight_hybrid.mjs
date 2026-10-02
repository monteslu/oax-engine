// oax_ulight_hybrid: a lightmapped room (q3map2 -light bakes its `light`
// entities as usual) with one realtime `rtlight` over a box. q3map2 ignores
// the rtlight classname, so only the renderer sees it.

import { MapFile, box, room } from '../mapwriter.mjs';

export function build() {
  const map = new MapFile({ message: 'oax test: hybrid lighting', oax_lighting: 'hybrid' });
  map.brush(room([-512, -512, 0], [512, 512, 256], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  map.brush(box([-48, -48, 0], [48, 48, 96], 'base_wall/basewall01'));
  map.entity('info_player_deathmatch', { origin: [-384, -384, 32], angle: 45 });
  map.entity('light', { origin: [0, 0, 220], light: 1200 });
  map.entity('light', { origin: [-300, -300, 200], light: 600 });
  map.entity('rtlight', { origin: [200, 200, 160], light_radius: [500, 500, 400], _color: [1.2, 0.4, 0.2] });
  return { map, manifest: { features: ['ulight'], lighting: 'hybrid' } };
}
