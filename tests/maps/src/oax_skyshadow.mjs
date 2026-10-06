// oax_skyshadow: sky faces cast shadows for point lights (docs/materials.md,
// "Sky faces as shadow casters"): a lamp does not shine through a sky panel
// onto what lies beyond it. The oax_movershadow room (x/y -512..512,
// z 0..512, a point light high in the middle) with the slab half way up
// made of a sky brush instead of a door: the floor under it must be in its
// shadow exactly as under the door. tests/romdev/tests/sky-shadow.mjs looks at
// the floor under the slab from below it (the slab out of view) and from
// the side (the slab in view, drawing the sky).

import { MapFile, box, room } from '../mapwriter.mjs';
import { tga, image } from '../lib/texgen.mjs';

export const LIGHT = { origin: [0, 0, 448], radius: [1200, 1200, 1200] };
export const SLAB = { mins: [-128, -128, 248], maxs: [128, 128, 264] };

export function build() {
  const map = new MapFile({
    message: 'oax test: sky faces cast shadows',
    oax_lighting: 'unified',
    oax_ambient: '0.05 0.05 0.05',
    oax_shadowmode: 'maps',
    _keepLights: 1,
  });
  const W = 'oax_skyshadow/white', S = 'oax_skyshadow/sky';
  map.brush(room([-512, -512, 0], [512, 512, 512], W));
  map.brush(box(SLAB.mins, SLAB.maxs, S));
  map.entity('light', { origin: LIGHT.origin, light_radius: LIGHT.radius, _color: [3, 3, 3] });
  map.entity('info_player_deathmatch', { origin: [-400, -400, 24], angle: 45 });
  const shader = `textures/${W}
{
	qer_editorimage textures/${W}.tga
	diffusemap textures/${W}.tga
	specularmap _black
}
textures/${S}
{
	qer_editorimage textures/${W}.tga
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	skyparms env/sky1/sky001 - -
}
`;
  return {
    map,
    light: 'none',
    files: { [`textures/${W}.tga`]: tga(8, 8, image(8, 8, () => [0.7, 0.7, 0.7, 1])), 'scripts/oax_skyshadow.shader': shader },
    manifest: { features: ['ulight'], lighting: 'unified' },
  };
}
