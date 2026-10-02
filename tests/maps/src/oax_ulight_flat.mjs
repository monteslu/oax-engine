// oax_ulight_flat: the analytic unified-lighting map. One point light over
// a flat white floor, no ambient, no specular, and one box that casts a
// shadow. tests/romdev/tests/ulight-analytic.mjs renders it from straight
// above and compares the pixels with a JS oracle of the same light math.

import { MapFile, box, room } from '../mapwriter.mjs';
import { tga, image } from '../lib/texgen.mjs';

export const LIGHT = { origin: [0, 0, 200], radius: [800, 800, 800], color: [1, 1, 1] };
export const BOX = { mins: [100, -20, 0], maxs: [140, 20, 64] };

export function build() {
  const map = new MapFile({
    message: 'oax test: unified lighting oracle',
    oax_lighting: 'unified',
    oax_ambient: '0 0 0',
    oax_shadowmode: 'maps',
    _keepLights: 1,
  });
  const W = 'oax_ulight/white';
  map.brush(room([-1024, -1024, 0], [1024, 1024, 1024], W));
  map.brush(box(BOX.mins, BOX.maxs, W));
  map.entity('info_player_deathmatch', { origin: [-800, -800, 24], angle: 45 });
  map.entity('light', { origin: LIGHT.origin, light_radius: LIGHT.radius, _color: LIGHT.color });
  const shader = `textures/${W}
{
	qer_editorimage textures/${W}.tga
	diffusemap _white
	specularmap _black
}
`;
  return {
    map,
    light: 'none',
    files: { [`textures/${W}.tga`]: tga(8, 8, image(8, 8, () => [1, 1, 1, 1])), 'scripts/oax_ulight.shader': shader },
    manifest: { features: ['ulight'], lighting: 'unified' },
  };
}
