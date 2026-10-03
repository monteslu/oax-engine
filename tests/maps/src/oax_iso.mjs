// oax_iso: content isolation (step 7.5 D, docs/test-hooks.md).
//
// Two packages are built next to the test maps:
// - zzz_oaxiso_a.pk3, this map's own package: maps/oax_iso.bsp, a RED
//   textures/oax_iso/probe.tga and textures/oax_iso/same.tga;
// - zzz_oaxiso_b.pk3, loaded after it: a GREEN probe.tga (different bytes,
//   so it wins the normal search order and is reported as a conflict) and
//   an identical same.tga (identical bytes: never reported).
// This map's floor is the probe: with the map package winning for its own
// assets it is red. oax_hooks has a probe panel too; that map is loose (no
// package), so it shows the green copy (the control).

import { MapFile, room, box } from '../mapwriter.mjs';
import { encodeTga } from '../../romdev/lib/tga.mjs';

export const PROBE = 'textures/oax_iso/probe.tga';
export const SAME = 'textures/oax_iso/same.tga';
export const PACK_A = 'zzz_oaxiso_a.pk3';
export const PACK_B = 'zzz_oaxiso_b.pk3';

export function solid(r, g, b, n = 64) {
  const data = Buffer.alloc(n * n * 4);
  for (let i = 0; i < n * n; i++) data.set([r, g, b, 255], i * 4);
  return encodeTga({ width: n, height: n, data });
}

export function build() {
  const map = new MapFile({ message: 'oax test: content isolation' });
  map.brush(room([-256, -256, 0], [256, 256, 256], { floor: 'oax_iso/probe', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  map.brush(box([-32, -32, 0], [32, 32, 96], 'gothic_block/blocks15'));
  map.entity('light', { origin: [0, 0, 220], light: 600 });
  map.entity('light', { origin: [-160, -160, 120], light: 300 });
  map.entity('info_player_deathmatch', { origin: [-160, 0, 32], angle: 0 });
  const same = solid(40, 60, 200);
  return {
    map,
    manifest: { features: [] },
    // q3map2 needs the probe at compile time; it is not left loose
    compileFiles: { [PROBE]: solid(255, 255, 255) },
    packages: [
      { name: PACK_A, map: true, files: { [PROBE]: solid(230, 30, 30), [SAME]: same } },
      { name: PACK_B, files: { [PROBE]: solid(30, 230, 30), [SAME]: same } },
    ],
  };
}
