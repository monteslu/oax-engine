// oax_metal: the oaxMetal test map (docs/materials.md). One unified-lit room
// (x -512..512, y -768..768, z 0..384) with a silver mirror (oaxMetal,
// roughness 0) standing across it at y 300 and facing south, a red panel on
// the south wall behind the eye, and one misc_cubemap probe between them.
// tests/romdev/tests/metal.mjs looks north at the mirror: it must show the
// red panel behind the eye, and not with r_oaxReflect 0.

import { MapFile, box, room } from '../mapwriter.mjs';
import { tga, image } from '../lib/texgen.mjs';

export const EYE = [0, -500, 160];
export const MIRROR = { mins: [-200, 300, 32], maxs: [200, 316, 288] };

export function build() {
  const map = new MapFile({
    message: 'oax test: oaxMetal reflections',
    oax_lighting: 'unified',
    oax_ambient: '0.3 0.3 0.3',
    oax_shadowmode: 'maps',
    _keepLights: 1,
  });
  const W = 'oax_metal/white', RED = 'oax_metal/red', SILVER = 'oax_metal/silver';
  map.brush(room([-512, -768, 0], [512, 768, 384], W));
  // the red panel on the south wall, behind the eye
  map.brush(box([-300, -768, 40], [300, -752, 340], { py: RED, sides: W }));
  // the mirror: its south face is the metal
  map.brush(box(MIRROR.mins, MIRROR.maxs, { ny: SILVER, sides: W }));
  map.entity('misc_cubemap', { origin: [0, 0, 160], radius: 1000 });
  map.entity('light', { origin: [0, -300, 300], light_radius: [1400, 1400, 1400], _color: [1, 1, 1] });
  map.entity('info_player_deathmatch', { origin: [0, -500, 24], angle: 90 });
  const shader = `textures/${W}
{
	qer_editorimage textures/${W}.tga
	diffusemap textures/${W}.tga
	specularmap _black
}

textures/${RED}
{
	qer_editorimage textures/${RED}.tga
	diffusemap textures/${RED}.tga
	specularmap _black
}

textures/${SILVER}
{
	qer_editorimage textures/${W}.tga
	oaxMetal 0.95 0.93 0.88 0
	{
		map $whiteimage
		rgbGen const ( 0.05 0.05 0.05 )
	}
}
`;
  return {
    map,
    light: 'none',
    files: {
      [`textures/${W}.tga`]: tga(8, 8, image(8, 8, () => [0.6, 0.6, 0.6, 1])),
      [`textures/${RED}.tga`]: tga(8, 8, image(8, 8, () => [1, 0.05, 0.05, 1])),
      'scripts/oax_metal.shader': shader,
    },
    manifest: { features: ['ulight'], lighting: 'unified' },
  };
}
