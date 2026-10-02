// oax_portal: area portals that doors and scripts close (design 2.8).
//
// Two rooms split by a wall with two openings, each sealed by a
// common/areaportal brush:
// - the door opening: a func_door the map script opens 4 s in and closes
//   8 s in (door events; the door opens and closes the area portal);
// - the arch: a func_oax_portal with portal_dist, closed while every player
//   is farther than PORTAL_DIST from it.
// The far room holds items and a looping speaker. The player starts in the
// near room, far from the arch, facing the door.

import { MapFile, room, box } from '../mapwriter.mjs';

export const SPAWN = [-700, -192, 24];
export const ARCH = [0, 192, 64];
export const PORTAL_DIST = 400;
export const SPEAKER = [600, 0, 64];

const SCRIPT = `/*
maps/oax_portal.script: the oax_portal test map's script (written for oax).
*/

void main() {
	sys.wait( 4 );
	$door.open();
	sys.wait( 4 );
	$door.close();
}
`;

export function build() {
  const map = new MapFile({ message: 'oax test: area portals' });
  const tex = { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' };
  map.brush(room([-1024, -384, 0], [1024, 384, 256], tex));

  // the dividing wall, with the door opening (y -256..-128) and the arch (y 128..256)
  const wall = 'gothic_block/blocks18c_3';
  map.brush(
    box([-16, -384, 0], [16, -256, 256], wall),
    box([-16, -256, 128], [16, -128, 256], wall),
    box([-16, -128, 0], [16, 128, 256], wall),
    box([-16, 128, 128], [16, 256, 256], wall),
    box([-16, 256, 0], [16, 384, 256], wall),
    box([-16, -256, 0], [16, -128, 128], 'common/areaportal'),
    box([-16, 128, 0], [16, 256, 128], 'common/areaportal'),
  );

  map.entity('func_door', { targetname: 'door', angle: -1, lip: 8, speed: 200, wait: -1 }, [
    box([-20, -256, 0], [20, -128, 128], 'base_wall/metalfloor_wall_10'),
  ]);
  map.entity('func_oax_portal', { targetname: 'arch', portal_dist: PORTAL_DIST }, [
    box([-24, 128, 0], [24, 256, 128], 'common/trigger'),
  ]);

  // the far room: columns, so it has surfaces worth culling
  for (const x of [256, 512, 768]) {
    for (const y of [-320, 320]) map.brush(box([x - 24, y - 24, 0], [x + 24, y + 24, 256], 'gothic_block/blocks18c_3'));
  }
  map.brush(box([880, -64, 0], [944, 64, 96], 'base_wall/metalfloor_wall_10'));
  for (const [i, y] of [-256, -96, 64, 224].entries()) {
    map.entity(['weapon_rocketlauncher', 'item_health_large', 'item_armor_body', 'weapon_railgun'][i], { origin: [500, y, 16] });
  }
  map.entity('target_speaker', { origin: SPEAKER, noise: 'sound/world/fan.wav', spawnflags: 1 });
  map.entity('info_player_deathmatch', { origin: SPAWN, angle: 0 });
  map.entity('light', { origin: [-512, 0, 200], light: 1000 });
  map.entity('light', { origin: [512, 0, 200], light: 1000 });
  map.entity('light', { origin: [-900, -300, 128], light: 500 });
  map.entity('light', { origin: [900, 300, 128], light: 500 });
  return { map, manifest: { features: ['script', 'portal'] }, files: { 'maps/oax_portal.script': SCRIPT } };
}
