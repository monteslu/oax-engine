// oax_movers: keyframed and spline movers (gamecode g_oax_mover.c).
//
// - mover0: a func_oax_mover with four keys and glide, looping on its own
//   (mode loop), turning 90 degrees between key1 and key2.
// - platform: a func_oax_splinemover riding a closed CatmullRom loop
//   (info_oax_spline "loop1"). The only spawn point is on it; a trigger over
//   the spawn starts it, so the player is aboard when it leaves.
// - spinner: a func_oax_splinemover on a closed NUBS loop that faces along
//   the curve (spline_angles), its origin on the curve (spline_absolute).
//
// The test (tests/romdev/tests/oax-movers.mjs) evaluates the same keys in
// JS; keep MOVER0 in sync with the entity keys below.

import { MapFile, room, box } from '../mapwriter.mjs';

export const MOVER0 = {
  origin: [-600, -600, 64],
  keys: [[0, 0, 0], [0, 0, 128], [256, 0, 128], [256, 0, 0]],
  angles: [[0, 0, 0], [0, 0, 0], [0, 90, 0], [0, 90, 0]],
  movetime: 1, glide: 0.25, stayOpen: 1,
};

export const PLATFORM = {
  origin: [0, -400, 120],
  curve: [[0, -400, 120], [400, 0, 120], [0, 400, 120], [-400, 0, 120]],
  time: 16,
};

const SPINNER = [[450, 600, 64], [600, 450, 64], [750, 600, 64], [600, 750, 64]];

const v3 = (p) => p.join(' ');
const curve = (pts) => `${pts.length} ( ${pts.map(v3).join(' ')} )`;

// a box around c with half extents h
const around = (c, h) => [[c[0] - h[0], c[1] - h[1], c[2] - h[2]], [c[0] + h[0], c[1] + h[1], c[2] + h[2]]];

export function build() {
  const map = new MapFile({ message: 'oax test: movers' });
  map.brush(room([-1024, -1024, 0], [1024, 1024, 512], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));

  const m0 = MOVER0;
  map.entity('func_oax_mover', {
    targetname: 'mover0', mode: 'loop', movetime: m0.movetime, glide: m0.glide, stay_open: m0.stayOpen,
    ...Object.fromEntries(m0.keys.slice(1).map((k, i) => [`key${i + 1}`, v3(k)])),
    ...Object.fromEntries(m0.angles.slice(1).map((k, i) => [`key${i + 1}_angles`, v3(k)])),
  }, [
    box(...around(m0.origin, [32, 32, 8]), 'base_wall/metalfloor_wall_10'),
    box(...around(m0.origin, [8, 8, 8]), 'common/origin'),
  ]);

  const p = PLATFORM;
  map.entity('info_oax_spline', { origin: [0, 0, 300], targetname: 'loop1', closed: 1, curve_CatmullRomSpline: curve(p.curve) });
  map.entity('func_oax_splinemover', {
    targetname: 'platform', spline: 'loop1', time: p.time, loop: 1, start_off: 1,
  }, [
    box(...around(p.origin, [128, 128, 8]), 'base_floor/diamond2c'),
    box(...around(p.origin, [8, 8, 8]), 'common/origin'),
  ]);
  // starts the platform when the player is on it
  map.entity('trigger_multiple', { target: 'platform', wait: 1 }, [
    box([p.origin[0] - 64, p.origin[1] - 64, p.origin[2] + 16], [p.origin[0] + 64, p.origin[1] + 64, p.origin[2] + 80], 'common/trigger'),
  ]);
  map.entity('info_player_deathmatch', { origin: [p.origin[0], p.origin[1], p.origin[2] + 40], angle: 90 });

  const c = [600, 600, 64];
  map.entity('info_oax_spline', { origin: [600, 600, 300], targetname: 'loop2', closed: 1, curve_nubs: curve(SPINNER) });
  map.entity('func_oax_splinemover', {
    targetname: 'spinner', spline: 'loop2', time: 6, loop: 1, spline_angles: 1, spline_absolute: 1,
  }, [
    box(...around(c, [64, 16, 16]), 'base_wall/metalfloor_wall_10'),
    box(...around(c, [8, 8, 8]), 'common/origin'),
  ]);

  map.entity('light', { origin: [0, 0, 400], light: 2500 });
  for (const [x, y] of [[-600, -600], [600, 600], [600, -600], [-600, 600]]) map.entity('light', { origin: [x, y, 300], light: 1500 });
  return { map, manifest: { features: ['movers'] } };
}
