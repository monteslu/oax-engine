// oax_zones: zone volumes (func_oax_zone). One long hall along X with three
// zones and gaps of plain hall between them:
//   slot 0  low gravity (200)         x -1024 .. -512
//   slot 1  current +X 200 ups, accel 5  x -256 .. 256
//   slot 2  fog + reverb ("hall")      x 512 .. 1536
// Spawn 0 stands in the low-gravity zone facing +X; spawns 1-3 stand in the
// current zone, the plain gap and the fog room (tests place players there).

import { MapFile, room, box } from '../mapwriter.mjs';

export const ZONES = [
  { lo: [-1024, -384, 0], hi: [-512, 384, 512], keys: { gravity: 200, targetname: 'lowgrav' } },
  { lo: [-256, -384, 0], hi: [256, 384, 512], keys: { current: '200 0 0', current_accel: 5 } },
  { lo: [512, -384, 0], hi: [1536, 384, 512], keys: { fog_color: '0.55 0.6 0.7', fog_density: 0.004, fog_start: 64, fog_end: 1200, reverb: 'hall' } },
];

export function build() {
  const map = new MapFile({ message: 'oax test: zones' });
  map.brush(room([-1024, -384, 0], [1536, 384, 512], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  for (const z of ZONES) map.entity('func_oax_zone', z.keys, [box(z.lo, z.hi, 'common/trigger')]);
  map.entity('info_player_deathmatch', { origin: [-768, 0, 32], angle: 0 });
  map.entity('info_player_deathmatch', { origin: [0, 0, 32], angle: 0 });
  map.entity('info_player_deathmatch', { origin: [384, 0, 32], angle: 0 });
  map.entity('info_player_deathmatch', { origin: [1024, 0, 32], angle: 0 });
  for (const x of [-768, 0, 1024]) map.entity('light', { origin: [x, 0, 400], light: 900 });
  for (const x of [-384, 384]) map.entity('light', { origin: [x, 0, 300], light: 500 });
  return { map, manifest: { features: ['zones'] } };
}
