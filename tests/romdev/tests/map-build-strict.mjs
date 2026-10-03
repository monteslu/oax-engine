// The strict map build: the
// q3map2 toolchain exits 0 on several failures, so tests/maps/build.mjs
// checks for them itself and throws. Each case below builds a crafted map
// that must FAIL with its reason, next to a control that must build:
//   - unknown material on a surface-world surface, and on a brush face;
//   - empty meta output (a hull of caulk only, no surface world);
//   - more brushes than q3map2 keeps (it silently drops past 65536);
//   - a brush q3map2 drops (an unbounded brush);
//   - a surface buried in the hull (binds to no BSP leaf);
//   - control: the same hull with a valid surface world builds, and the
//     lump it writes reads back with every surface bound to leaves.
// No romdev: this runs q3map2 and bspc only. Outputs are removed.

import fs from 'node:fs';
import path from 'node:path';
import { buildSpec, outDir } from '../../maps/build.mjs';
import { MapFile, box, Brush, Face } from '../../maps/mapwriter.mjs';
import { SurfaceWorld, OSF, uvMatrix, surfacesFromBsp } from '../../../misc/tools/oax-surfaces.mjs';

export const name = 'map-build-strict';

const PREFIX = 'zz_strict_';
const CAULK = 'common/caulk';

function hull(map) {
  map.brush(box([-272, -272, -16], [272, 272, 0], CAULK));
  map.brush(box([-272, -272, 256], [272, 272, 272], CAULK));
  map.brush(box([-272, -272, 0], [-256, 272, 256], CAULK));
  map.brush(box([256, -272, 0], [272, 272, 256], CAULK));
  map.brush(box([-256, -272, 0], [256, -256, 256], CAULK));
  map.brush(box([-256, 256, 0], [256, 272, 256], CAULK));
  map.entity('info_player_deathmatch', { origin: [0, 0, 32] });
  map.entity('light', { origin: [0, 0, 200], light: 300 });
}

function floor(sw, material, z = 0, extra = {}) {
  sw.polygon({ material, points: [[-256, -256, z], [256, -256, z], [256, 256, z], [-256, 256, z]], uv: uvMatrix({ u: [1 / 128, 0, 0], v: [0, -1 / 128, 0] }), ...extra });
}

function spec(map, extra = {}) {
  return { map, light: 'none', aas: false, manifest: { features: ['surfaces'] }, ...extra };
}

const CASES = [
  {
    name: 'unknown surface material', expect: /unknown materials.*textures\/oax_no_such\/material/,
    make() {
      const map = new MapFile(); hull(map);
      const sw = new SurfaceWorld(); floor(sw, 'textures/oax_no_such/material');
      return spec(map, { surfaces: sw });
    },
  },
  {
    name: 'unknown brush texture', expect: /unknown materials.*oax_no_such\/brushtex/,
    make() {
      const map = new MapFile(); hull(map);
      map.brush(box([-32, -32, 0], [32, 32, 64], 'oax_no_such/brushtex'));
      return spec(map);
    },
  },
  {
    name: 'empty meta output', expect: /empty meta output/,
    make() {
      const map = new MapFile(); hull(map);
      return spec(map);
    },
  },
  {
    name: 'more brushes than q3map2 keeps', expect: /q3map2 silently drops brushes past 65536/,
    make() {
      const map = new MapFile(); hull(map);
      for (let i = 0; i < 65536; i++) map.world.brushes.push(new Brush([new Face([0, 0, 1], 1, CAULK)]));
      return spec(map);
    },
  },
  {
    name: 'a brush q3map2 drops', expect: /q3map2 dropped 1 brushes/,
    make() {
      const map = new MapFile(); hull(map);
      map.brush(box([-32, -32, 0], [32, 32, 64], 'base_floor/clang_floor'));
      // five sides: open at the top, so q3map2 throws it away without failing
      map.brush(new Brush([
        new Face([0, 0, -1], -8, 'base_floor/clang_floor'), new Face([1, 0, 0], 120, 'base_floor/clang_floor'),
        new Face([-1, 0, 0], -80, 'base_floor/clang_floor'), new Face([0, 1, 0], 120, 'base_floor/clang_floor'),
        new Face([0, -1, 0], -80, 'base_floor/clang_floor'),
      ]));
      return spec(map);
    },
  },
  {
    name: 'a surface buried in the hull', expect: /1 surface-world surfaces bind to no BSP leaf/,
    make() {
      const map = new MapFile(); hull(map);
      const sw = new SurfaceWorld();
      floor(sw, 'textures/base_floor/clang_floor');
      floor(sw, 'textures/base_floor/clang_floor', -8);
      return spec(map, { surfaces: sw });
    },
  },
  {
    name: 'control: a valid surface world', expect: null,
    make() {
      const map = new MapFile(); hull(map);
      const sw = new SurfaceWorld();
      floor(sw, 'textures/base_floor/clang_floor');
      floor(sw, 'textures/base_floor/clang_floor', 128, { flags: OSF.TWOSIDED | OSF.DETAIL });
      return spec(map, { surfaces: sw });
    },
  },
];

export async function run() {
  const failures = [];
  const rows = [];
  for (const [i, c] of CASES.entries()) {
    const map = `${PREFIX}${i}`;
    let err = null, bsp = null;
    try {
      bsp = await buildSpec(map, c.make());
    } catch (e) {
      err = e;
    }
    const msg = err ? String(err.message).split('\n')[0] : 'built';
    rows.push(`${c.name}: ${msg.slice(0, 160)}`);
    if (c.expect && !err) failures.push(`${c.name}: the build did not fail`);
    if (c.expect && err && !c.expect.test(err.message)) failures.push(`${c.name}: failed for another reason: ${msg}`);
    if (!c.expect && err) failures.push(`${c.name}: the control failed to build: ${msg}`);
    if (!c.expect && bsp) {
      const { surfaces } = surfacesFromBsp(fs.readFileSync(bsp));
      const bound = surfaces ? surfaces.surfaces.filter((s) => s.leaves && s.leaves.length > 0).length : 0;
      rows.push(`  ${surfaces ? surfaces.surfaces.length : 0} surfaces read back, ${bound} bound to leaves`);
      if (!surfaces || bound !== surfaces.surfaces.length) failures.push(`${c.name}: the lump did not read back bound`);
    }
    for (const f of fs.readdirSync(path.join(outDir, 'baseoa', 'maps')).filter((n) => n.startsWith(`${map}.`))) {
      fs.rmSync(path.join(outDir, 'baseoa', 'maps', f), { force: true });
    }
  }
  return { ok: failures.length === 0, failures, rows };
}
