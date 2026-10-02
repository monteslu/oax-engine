// perfmap.mjs: the unified-lighting measurement maps (DESIGN 3.6): the
// oax_unified rooms with N shadowed point lights in the pillar hall (room
// A), static or each bound to one of four bobbing movers (moving: the static
// shadow cache never hits).

import { MapFile, box } from '../mapwriter.mjs';
import { geometry, materials } from '../src/oax_unified.mjs';

export function perfMap(n, moving) {
  const map = new MapFile({
    message: `oax test: unified lighting perf, ${n} ${moving ? 'moving' : 'static'} lights`,
    oax_lighting: 'unified',
    oax_ambient: '0.035 0.045 0.11',
    oax_shadowmode: 'maps',
    _keepLights: 1,
  });
  geometry(map);
  if (moving) {
    for (let m = 0; m < 4; m++) {
      map.entity('func_bobbing', { targetname: `perfbob${m}`, spawnflags: m % 2 ? 1 : 2, height: 60, speed: 3 + m, phase: m * 0.25 }, [
        box([-1300 + m * 20, 340, 270], [-1290 + m * 20, 350, 280], 'common/nodraw'),
      ]);
    }
  }
  // a grid over room A (x -1300..-340, y -340..340), above the pillars' feet
  const cols = Math.ceil(Math.sqrt(n * 2)), rows = Math.ceil(n / cols);
  for (let i = 0; i < n; i++) {
    const cx = i % cols, cy = Math.floor(i / cols);
    const x = -1300 + (cx + 0.5) * (960 / cols), y = -340 + (cy + 0.5) * (680 / rows);
    const keys = { origin: [Math.round(x), Math.round(y), 200 + (i % 3) * 25], light_radius: [420, 420, 420], _color: [0.5 + (i % 2) * 0.3, 0.6, 0.5 + ((i >> 1) % 2) * 0.3] };
    if (moving) keys.bind = `perfbob${i % 4}`;
    map.entity('light', keys);
  }
  return { map, light: 'none', files: materials(), manifest: { features: ['ulight'], lighting: 'unified' } };
}
