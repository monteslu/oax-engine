// oax_ue1_calib: the UE1 lamp calibration rebuilt for the ue1 light profile.
// The rooms the UE1 reference renderer baked and shot with its display chain
// neutral
// (tests/romdev/reference/ue1-light-calib.json, misc/tools/ue1-light-calib.mjs):
//   calib  four calibration corridors (2000 x 800 x 800), one lamp each
//          with small-lamp settings from a reference map;
//   big    two 7000 x 3000 x 2400 halls, lamps at LightRadius 200 and 255.
// Every wall is one flat colour, the mean of a smooth stone texture, so a
// pixel compares with the median reference pixel of the same lamp, distance
// and angle.
// The big set sits BIG_OFFSET units along y. The lamps cast no shadows (an
// empty room has nothing to shadow; the player standing in one would).
// tests/romdev/tests/ulight-ue1-calib.mjs renders it and compares.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { MapFile, room } from '../mapwriter.mjs';
import { tga, image } from '../lib/texgen.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
export const CALIB = JSON.parse(fs.readFileSync(path.join(here, '..', '..', 'romdev', 'reference', 'ue1-light-calib.json'), 'utf8'));
export const OFFSET = { calib: 0, big: 20000 };
// the y of lamp i's room in a set
export const centre = (set, i) => OFFSET[set] + i * CALIB.sets[set].box.gap;

// keysFor(lamp): extra keys on a lamp (the control map overrides the profile)
export function buildCalib(name, keysFor = () => ({})) {
  const map = new MapFile({
    message: `oax test: UE1 light calibration (${name})`,
    oax_lighting: 'unified',
    oax_ambient: '0 0 0',
    oax_overbright: 2,
    _keepLights: 1,
  });
  const TEX = 'oax_calib/wallmean';
  for (const [set, S] of Object.entries(CALIB.sets)) {
    const { len, halfWidth, height } = S.box;
    S.lamps.forEach((l, i) => {
      const c = centre(set, i);
      map.brush(room([0, c - halfWidth, 0], [len, c + halfWidth, height], TEX));
      map.entity('light', {
        origin: [l.x, c, l.z], noshadows: 1,
        ...Object.fromEntries(Object.entries(l.props).map(([k, v]) => [`ue1_${k}`, v])),
        ...keysFor(l),
      });
      map.entity('info_player_deathmatch', { origin: [len - 500, c + halfWidth - 100, 24], angle: 0 });
    });
  }
  const [r, g, b] = CALIB.textureMean;
  const shader = `textures/${TEX}
{
	qer_editorimage textures/${TEX}.tga
	diffusemap textures/${TEX}.tga
	specularmap _black
}
`;
  return {
    map,
    light: 'none',
    files: {
      [`textures/${TEX}.tga`]: tga(8, 8, image(8, 8, () => [r, g, b, 1])),
      'scripts/oax_calib.shader': shader,
    },
    manifest: { features: ['ulight'], lighting: 'unified' },
  };
}

export function build() {
  return buildCalib('oax_ue1_calib');
}
