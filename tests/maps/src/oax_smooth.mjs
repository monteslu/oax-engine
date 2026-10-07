// oax_smooth / oax_smooth_flat: oax_smoothnormals (docs/map-format.md, "Map
// keys for stock maps"). A lightmapped room with a twelve-sided pillar (a
// low-poly curve: each facet a flat face) and one realtime light beside it
// (hybrid lighting). oax_smooth carries oax_smoothnormals 60 in its
// worldspawn, oax_smooth_flat does not: the pillar shades smoothly in the
// first and in visible steps, facet by facet, in the second.

import { MapFile, Brush, Face, room } from '../mapwriter.mjs';

export const PILLAR = { sides: 12, radius: 64, z: [0, 256] };

export function build({ smooth = true } = {}) {
  const map = new MapFile({
    message: `oax test: smooth normals (${smooth ? 'on' : 'off'})`,
    oax_lighting: 'hybrid',
    ...(smooth ? { oax_smoothnormals: 60 } : {}),
  });
  map.brush(room([-512, -512, 0], [512, 512, 256], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  const n = PILLAR.sides, apothem = PILLAR.radius * Math.cos(Math.PI / n);
  const faces = [new Face([0, 0, 1], PILLAR.z[1], 'base_wall/basewall01'), new Face([0, 0, -1], -PILLAR.z[0], 'base_wall/basewall01')];
  for (let k = 0; k < n; k++) {
    const a = ((k + 0.5) * 2 * Math.PI) / n;
    faces.push(new Face([Math.cos(a), Math.sin(a), 0], apothem, 'base_wall/basewall01'));
  }
  map.brush(new Brush(faces).check());
  map.entity('info_player_deathmatch', { origin: [-400, 0, 32], angle: 0 });
  map.entity('light', { origin: [0, 0, 220], light: 300 });
  map.entity('rtlight', { origin: [-170, 110, 150], light_radius: [420, 420, 300], _color: [2.2, 1.9, 1.5] });
  return { map, manifest: { features: ['ulight'], lighting: 'hybrid' } };
}
