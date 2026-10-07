// oax_navfloor: a mover at rest is navmesh floor (docs/navigation.md,
// "Movers as floor"). Two ledges 128 up, west and east, with a 512-unit gap
// between them over the floor (a 128 drop: no way back up), bridged by a
// func_oax_mover with navfloor 1 that waits at rest until a trigger on the
// west ledge sends it down into the floor. tests/romdev/tests/nav-floor.mjs
// paths west to east across the bridge, then fires the trigger and paths
// again: the bridge is gone from the mesh.

import { MapFile, box, room } from '../mapwriter.mjs';

const SOLID = { top: 'base_floor/clang_floor', sides: 'gothic_block/blocks18c' };
export const LEDGE_Z = 128;
export const WEST = [-500, 0, LEDGE_Z + 8], EAST = [500, 0, LEDGE_Z + 8];
export const BRIDGE = { origin: [0, 0, 120], half: [256, 64, 8] };	// z 112..128, flush with the ledges
export const TRIGGER = { lo: [-740, -240, LEDGE_Z], hi: [-680, -180, LEDGE_Z + 96] };
export const SPAWN = { origin: [-500, 0, LEDGE_Z + 32], angle: 0 };

const around = (c, h) => [[c[0] - h[0], c[1] - h[1], c[2] - h[2]], [c[0] + h[0], c[1] + h[1], c[2] + h[2]]];

export function build() {
  const map = new MapFile({ message: 'oax test: a mover at rest is navmesh floor' });
  map.brush(room([-768, -768, 0], [768, 768, 512], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  map.brush(box([-768, -256, 0], [-256, 256, LEDGE_Z], SOLID));
  map.brush(box([256, -256, 0], [768, 256, LEDGE_Z], SOLID));
  // the bridge: one key, straight down into the floor, when used
  map.entity('func_oax_mover', { targetname: 'bridge', navfloor: 1, movetime: 1, key1: '0 0 -400' }, [
    box(...around(BRIDGE.origin, BRIDGE.half), 'base_wall/metalfloor_wall_10'),
    box(...around(BRIDGE.origin, [8, 8, 8]), 'common/origin'),
  ]);
  map.entity('trigger_multiple', { target: 'bridge', wait: 1 }, [box(TRIGGER.lo, TRIGGER.hi, 'common/trigger')]);
  map.entity('info_player_deathmatch', { origin: SPAWN.origin, angle: SPAWN.angle });
  for (const [x, y] of [[-500, 0], [0, 0], [500, 0]]) map.entity('light', { origin: [x, y, 420], light: 600 });
  return { map, aas: false, manifest: { features: ['nav'] } };
}
