// oax_zone_walk: a zone current that drags walking players (func_oax_zone
// current_walk 1, as UE1's ZoneVelocity drags pawns along a train's roof)
// beside one that does not. Two halls side by side along X:
//   south (y < 0): current +X 120 ups, accel 5, current_walk 1
//   north (y > 0): the same current without current_walk (airborne only)
// tests/romdev/tests/zone-walk.mjs stands a player in each.

import { MapFile, room, box } from '../mapwriter.mjs';

export const WALK = { x: -400, y: -256, z: 32, yaw: 0 };
export const STILL = { x: -400, y: 256, z: 32, yaw: 0 };

export function build() {
  const map = new MapFile({ message: 'oax test: walking current' });
  map.brush(room([-768, -512, 0], [768, 512, 384], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  map.brush(box([-768, -16, 0], [768, 16, 384], 'gothic_wall/oct20c'));
  map.entity('func_oax_zone', { current: '120 0 0', current_accel: 5, current_walk: 1 }, [box([-768, -512, 0], [768, -16, 384], 'common/trigger')]);
  map.entity('func_oax_zone', { current: '120 0 0', current_accel: 5 }, [box([-768, 16, 0], [768, 512, 384], 'common/trigger')]);
  map.entity('info_player_deathmatch', { origin: [-600, -256, 32], angle: 0 });
  map.entity('info_player_deathmatch', { origin: [-600, 256, 32], angle: 0 });
  for (const y of [-256, 256]) map.entity('light', { origin: [0, y, 300], light: 800 });
  return { map, manifest: { features: ['zones'] } };
}
