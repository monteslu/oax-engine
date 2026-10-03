// oax_zone_many: one func_oax_zone made of 300 brushes (more than the 256 the
// renderer once kept; a reference map's zones have 1109), ambient 0.5. Floor panels
// of the flat texture (100, 150, 200) lie under the first brush written,
// under the last, and outside the zone (the old 256-brush renderer lit
// only the last: q3map2 stores this zone's brushes in reverse order);
// tests/romdev/tests/ulight-zone-brushes.mjs
// checks each panel's ambient and the renderer's count of zone brushes.

import { MapFile, room, box } from '../mapwriter.mjs';
import { tga, image } from '../lib/texgen.mjs';

export const GRID = [20, 15];            // 300 brushes
export const CELL = 32;
export const ORIGIN = [-320, -240];      // the grid's low corner
export const AMBIENT = 0.5;
export const FLAT = [100, 150, 200];
// panel centres: under brush 0, under brush 299, outside the zone
export const PANELS = {
  first: [ORIGIN[0] + CELL / 2, ORIGIN[1] + CELL / 2],
  last: [ORIGIN[0] + (GRID[0] - 0.5) * CELL, ORIGIN[1] + (GRID[1] - 0.5) * CELL],
  out: [ORIGIN[0] + CELL / 2, ORIGIN[1] - 3 * CELL],
};
export const PANEL_HALF = 12;
const T = 'oax_zone_many';

export function build() {
  const map = new MapFile({ message: 'oax test: a zone of 300 brushes', oax_lighting: 'unified', oax_ambient: '0 0 0', _keepLights: 1 });
  map.brush(room([-512, -512, -16], [512, 512, 512], `${T}/black`));
  for (const [name, [x, y]] of Object.entries(PANELS)) {
    map.brush(box([x - PANEL_HALF, y - PANEL_HALF, -16], [x + PANEL_HALF, y + PANEL_HALF, 0], { top: `${T}/${name}`, sides: 'common/caulk', bottom: 'common/caulk' }));
  }
  const brushes = [];
  for (let j = 0; j < GRID[1]; j++) for (let i = 0; i < GRID[0]; i++) {
    const x = ORIGIN[0] + i * CELL, y = ORIGIN[1] + j * CELL;
    brushes.push(box([x, y, -8], [x + CELL, y + CELL, 64], 'common/trigger'));
  }
  map.entity('func_oax_zone', { ambient: `${AMBIENT} ${AMBIENT} ${AMBIENT}` }, brushes);
  map.entity('info_player_deathmatch', { origin: [0, -400, 24], angle: 90 });
  const sh = (name, img) => `textures/${T}/${name}
{
	qer_editorimage textures/${T}/${img}.tga
	diffusemap textures/${T}/${img}.tga
	specularmap _black
}
`;
  return {
    map,
    light: 'none',
    files: {
      [`textures/${T}/flat.tga`]: tga(8, 8, image(8, 8, () => [...FLAT.map((v) => v / 255), 1])),
      [`textures/${T}/black.tga`]: tga(8, 8, image(8, 8, () => [0, 0, 0, 1])),
      'scripts/oax_zone_many.shader': [sh('first', 'flat'), sh('last', 'flat'), sh('out', 'flat'), sh('black', 'black')].join('\n'),
    },
    manifest: { features: ['ulight', 'zones'], lighting: 'unified' },
  };
}
