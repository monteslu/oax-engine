// oax_movershadow: movers cast unified-lighting shadows. One unified-lit
// room (x/y -512..512, z 0..512) with a point light high in the middle and a
// func_door slab (never opened) half way up under it, shadowing the floor
// (the shadow reaches about 300 units out). tests/romdev/tests/mover-shadow.mjs
// looks at the floor under the slab from below it (the slab out of view) and
// from the side (the slab in view).

import { MapFile, box, room } from '../mapwriter.mjs';
import { tga, image } from '../lib/texgen.mjs';

export const LIGHT = { origin: [0, 0, 448], radius: [1200, 1200, 1200] };
export const SLAB = { mins: [-128, -128, 248], maxs: [128, 128, 264] };

export function build() {
  const map = new MapFile({
    message: 'oax test: mover shadows',
    oax_lighting: 'unified',
    oax_ambient: '0.05 0.05 0.05',
    oax_shadowmode: 'maps',
    _keepLights: 1,
  });
  const W = 'oax_movershadow/white';
  map.brush(room([-512, -512, 0], [512, 512, 512], W));
  // a door that never opens: no targetname, its trigger field out of reach
  map.entity('func_door', { angle: -1, lip: 8, speed: 100, wait: -1 }, [box(SLAB.mins, SLAB.maxs, W)]);
  map.entity('light', { origin: LIGHT.origin, light_radius: LIGHT.radius, _color: [3, 3, 3] });
  map.entity('info_player_deathmatch', { origin: [-400, -400, 24], angle: 45 });
  const shader = `textures/${W}
{
	qer_editorimage textures/${W}.tga
	diffusemap textures/${W}.tga
	specularmap _black
}
`;
  return {
    map,
    light: 'none',
    files: { [`textures/${W}.tga`]: tga(8, 8, image(8, 8, () => [0.7, 0.7, 0.7, 1])), 'scripts/oax_movershadow.shader': shader },
    manifest: { features: ['ulight'], lighting: 'unified' },
  };
}
