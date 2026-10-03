// oax_ue1_level: worldspawn ue1_LevelBrightness 1.5 (UE1 LevelInfo.Brightness,
// the value of a reference map). Two ue1 lamps over a flat grey floor (texture 128) in
// one hall, shot straight down by tests/romdev/tests/ulight-ue1-level.mjs:
//   near    hue 32, saturation 128, brightness 96, radius 18, 120 up
//   wide    brightness 50, radius 30, 300 up
// oax_overbright 2, no ambient.

import { MapFile, room, box } from '../mapwriter.mjs';
import { tga, image } from '../lib/texgen.mjs';

export const LEVEL_BRIGHTNESS = 1.5;
export const SPACING = 2048, HALF = 512, ALBEDO = 128;
export const LAMPS = [
  { name: 'near', h: 120, keys: { ue1_LightBrightness: 96, ue1_LightHue: 32, ue1_LightSaturation: 128, ue1_LightRadius: 18 } },
  { name: 'wide', h: 300, keys: { ue1_LightBrightness: 50, ue1_LightHue: 32, ue1_LightSaturation: 128, ue1_LightRadius: 30 } },
  { name: 'park', h: 0, keys: null },
];
export const WORLD = { ue1_LevelBrightness: LEVEL_BRIGHTNESS };

export function build() {
  const map = new MapFile({
    message: 'oax test: UE1 LevelInfo Brightness',
    oax_lighting: 'unified', oax_ambient: '0 0 0', oax_shadowmode: 'maps', oax_overbright: 2, _keepLights: 1,
    ...WORLD,
  });
  const G = 'oax_ue1_level/grey', Wl = 'oax_ue1_level/wall';
  const hall = room([-SPACING / 2, -HALF, 0], [(LAMPS.length - 0.5) * SPACING, HALF, 768], { floor: 'common/caulk', ceiling: Wl, walls: Wl });
  map.brush(hall.slice(1));
  map.brush(box([-SPACING / 2, -HALF - 16, -16], [(LAMPS.length - 0.5) * SPACING, HALF + 16, 0], { top: G, sides: 'common/caulk', bottom: 'common/caulk' }));
  LAMPS.forEach((l, i) => { if (l.keys) map.entity('light', { origin: [i * SPACING, 0, l.h], ...l.keys }); });
  map.entity('info_player_deathmatch', { origin: [2 * SPACING, 0, 24], angle: 90 });
  const flat = (v) => tga(8, 8, image(8, 8, () => [v / 255, v / 255, v / 255, 1]));
  const sh = (n) => `textures/${n}
{
	qer_editorimage textures/${n}.tga
	diffusemap textures/${n}.tga
	specularmap _black
}
`;
  return {
    map, light: 'none',
    files: { [`textures/${G}.tga`]: flat(ALBEDO), [`textures/${Wl}.tga`]: flat(64), 'scripts/oax_ue1_level.shader': sh(G) + '\n' + sh(Wl) },
    manifest: { features: ['ulight'], lighting: 'unified' },
  };
}
