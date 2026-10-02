// oax_phys: the physics test room (physics-determinism). A large closed
// room with pillars, a ramp, steps and a curved patch quarter-pipe, so the
// BSP static collision has brushes of several shapes and a patch mesh. The
// scene itself is built by the game module (`physscene`, g_oax_phys.c).

import { MapFile, room, box, Brush, Face, Patch } from '../mapwriter.mjs';

export function build() {
  const map = new MapFile({ message: 'oax test: physics' });
  const floor = 'base_floor/clang_floor';
  const wall = 'gothic_wall/oct20c';
  map.brush(room([-1024, -1024, 0], [1024, 1024, 768], { floor, ceiling: 'base_floor/concrete', walls: wall }));
  // pillars
  for (const [x, y] of [[-512, -512], [512, -512], [-512, 512], [512, 512]]) {
    map.brush(box([x - 32, y - 32, 0], [x + 32, y + 32, 256], wall));
  }
  // a ramp along +x (a wedge: floor, back, sides and a sloped top)
  map.brush(new Brush([
    new Face([0, 0, -1], 0, floor),
    new Face([1, 0, 0], 960, floor),
    new Face([0, 1, 0], -256, floor),
    new Face([0, -1, 0], 512, floor),
    new Face([-1, 0, 2], 2 * 0 - 640, floor),
  ]).check());
  // steps along -x
  for (let i = 0; i < 6; i++) {
    map.brush(box([-960 + i * 48, 256, 0], [-912 + i * 48, 512, 16 + i * 16], floor));
  }
  // a curved quarter-pipe patch in the -y wall's corner region
  const pipe = [];
  for (let r = 0; r < 3; r++) {
    const x = -256 + r * 256;
    pipe.push([[x, -1024, 192, r, 0], [x, -1024, 0, r, 0.5], [x, -832, 0, r, 1]]);
  }
  map.brush(new Patch(pipe, floor));
  map.entity('info_player_deathmatch', { origin: [0, 0, 32], angle: 90 });
  // a hurt volume in the far corner: 140 damage kills a fresh player without
  // gibbing (health -15 > GIB_HEALTH), for the ragdoll test
  map.entity('trigger_hurt', { dmg: 140 }, [box([-1000, 800, 0], [-800, 1000, 96], 'common/trigger')]);
  map.entity('light', { origin: [0, 0, 700], light: 1500 });
  for (const [x, y] of [[-700, -700], [700, -700], [-700, 700], [700, 700]]) {
    map.entity('light', { origin: [x, y, 400], light: 800 });
  }
  return { map, manifest: { features: ['physics'] } };
}
