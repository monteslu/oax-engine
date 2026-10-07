// oax_litblend: unified lights light blended surfaces (docs/map-format.md,
// OSF_TRANSLUCENT|OSF_ADDITIVE: lit additive, UE1 Translucent water). One
// unified-lit room (x/y -512..512, z 0..512) with a black floor, white walls,
// a point light high in the middle, a water quad (an OAX_SURFACES surface)
// 64 up over the whole floor (its east half tinted, no red), and a slab
// east of the light at z 248..264
// whose shadow falls on the water around x 200..456. The floor is black so
// what the camera sees over the water is the water's own light.
// tests/romdev/tests/lit-blend.mjs looks straight down at the water in the
// open and in the slab's shadow, with the lit-blend pass on and off.

import { MapFile, box, room } from '../mapwriter.mjs';
import { SurfaceWorld, OSF, uvMatrix } from '../../../misc/tools/oax-surfaces.mjs';
import { tga, image } from '../lib/texgen.mjs';

export const LIGHT = { origin: [0, 0, 448], radius: [1200, 1200, 1200] };
export const SLAB = { mins: [100, -96, 248], maxs: [228, 96, 264] };
export const WATER_Z = 64;
export const WATER = { color: [0.3, 0.4, 0.5] };
export const TINT = [0, 0.6, 0.5];   // the east half's per-surface tint
// where the test looks straight down from z 200: the open water under the
// light, and the water in the slab's shadow (x 200..456, y -192..192)
export const OPEN_CAM = [0, 0, 200];
export const SHADOW_CAM = [330, 0, 200];

export function build() {
  const map = new MapFile({
    message: 'oax test: lit blended surfaces',
    oax_lighting: 'unified',
    oax_ambient: '0.05 0.05 0.05',
    oax_shadowmode: 'maps',
    _keepLights: 1,
  });
  const T = 'oax_litblend';
  const W = `${T}/white`, B = `${T}/black`, WATER_MAT = `textures/${T}/water`;
  map.brush(room([-512, -512, 0], [512, 512, 512], { floor: B, ceiling: W, walls: W }));
  map.brush(box(SLAB.mins, SLAB.maxs, W));
  map.entity('light', { origin: LIGHT.origin, light_radius: LIGHT.radius, _color: [3, 3, 3] });
  map.entity('info_player_deathmatch', { origin: [-400, -400, 24], angle: 45 });

  const sw = new SurfaceWorld();
  const z = WATER_Z, s = 1 / 128;
  // two halves (wound to face up): the west plain, the east with a
  // per-surface tint (no red): the lights' light on it must carry the tint
  const uv = uvMatrix({ u: [s, 0, 0], v: [0, -s, 0] });
  sw.polygon({ material: WATER_MAT, flags: OSF.TRANSLUCENT | OSF.ADDITIVE, points: [[-512, -512, z], [0, -512, z], [0, 512, z], [-512, 512, z]], uv });
  sw.polygon({ material: WATER_MAT, flags: OSF.TRANSLUCENT | OSF.ADDITIVE, points: [[0, -512, z], [512, -512, z], [512, 512, z], [0, 512, z]], uv, tint: [...TINT, 1] });

  const flat = (c) => tga(8, 8, image(8, 8, () => [...c, 1]));
  const shader = `textures/${W}
{
	qer_editorimage textures/${W}.tga
	diffusemap textures/${W}.tga
	specularmap _black
}
textures/${B}
{
	qer_editorimage textures/${B}.tga
	diffusemap textures/${B}.tga
	specularmap _black
}
`;
  return {
    map,
    light: 'none',
    aas: false,
    surfaces: sw,
    files: {
      [`textures/${W}.tga`]: flat([0.7, 0.7, 0.7]),
      [`textures/${B}.tga`]: flat([0, 0, 0]),
      [`${WATER_MAT}.tga`]: flat(WATER.color),
      'scripts/oax_litblend.shader': shader,
    },
    manifest: { features: ['ulight', 'surfaces'], lighting: 'unified' },
  };
}
