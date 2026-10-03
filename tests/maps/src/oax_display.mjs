// oax_display: flat grey panels at the texel values the UE1 reference
// renderer's display chain was measured with (texel 41, 74, 114, 180 shown
// as 84, 136, 183, 232 with its default Brightness 1.0, GammaOffset 0.1), lit at exactly 1
// (unified, oax_ambient 1 1 1, no lights). tests/romdev/tests/display-curve.mjs.
import { MapFile, room, box } from '../mapwriter.mjs';
import { tga, image } from '../lib/texgen.mjs';

export const TEXELS = [41, 74, 114, 180];
export const MEASURED = [84, 136, 183, 232];      // reference shots, default settings
export const HALF = 80;
export const centre = (i) => [0, (i - 1.5) * 200];
const T = 'oax_display';

export function build() {
  const map = new MapFile({ message: 'oax test: display curve', oax_lighting: 'unified', oax_ambient: '1 1 1', _keepLights: 1 });
  map.brush(room([-512, -512, -16], [512, 512, 512], `${T}/black`));
  TEXELS.forEach((v, i) => {
    const [x, y] = centre(i);
    map.brush(box([x - HALF, y - HALF, -16], [x + HALF, y + HALF, 0], { top: `${T}/g${v}`, sides: 'common/caulk', bottom: 'common/caulk' }));
  });
  map.entity('info_player_deathmatch', { origin: [0, -400, 24], angle: 90 });
  const files = { [`textures/${T}/black.tga`]: tga(8, 8, image(8, 8, () => [0, 0, 0, 1])) };
  let shader = `textures/${T}/black\n{\n\tqer_editorimage textures/${T}/black.tga\n\tdiffusemap textures/${T}/black.tga\n\tspecularmap _black\n}\n`;
  for (const v of TEXELS) {
    files[`textures/${T}/g${v}.tga`] = tga(8, 8, image(8, 8, () => [v / 255, v / 255, v / 255, 1]));
    shader += `\ntextures/${T}/g${v}\n{\n\tqer_editorimage textures/${T}/g${v}.tga\n\tdiffusemap textures/${T}/g${v}.tga\n\tspecularmap _black\n}\n`;
  }
  files['scripts/oax_display.shader'] = shader;
  return { map, light: 'none', files, manifest: { features: ['ulight'], lighting: 'unified' } };
}
