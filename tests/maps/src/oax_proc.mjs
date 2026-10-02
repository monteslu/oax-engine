// oax_proc: procedural textures and image programs.
//
// One room with six fullbright panels on its +Y wall, left to right:
// fire, water, wet, ice, plasma (procedural stages, tr_procedural.c) and a
// DOOM-3 image program (heightmap(), tr_image_program.c) shown as colour.
// The image program reads textures/oax_proc/height16.tga, a 16x16 test
// image this module also exports, so the test can run the same program in
// JS and compare bytes.

import { MapFile, room, box } from '../mapwriter.mjs';
import { shaderList } from '../shaderlist.mjs';
import { encodeTga } from '../../romdev/lib/tga.mjs';

export const PANELS = ['fire', 'water', 'wet', 'ice', 'plasma', 'heightmap'];
export const PANEL_Y = 240;
export const panelCenter = (i) => [-600 + i * 240, PANEL_Y, 128];
export const HEIGHT_IMAGE = 'textures/oax_proc/height16.tga';
export const HEIGHT_PROGRAM = `heightmap(${HEIGHT_IMAGE}, 4)`;

// A 16x16 RGBA test image: ramps, a ridge and hashed noise, so every
// branch of heightmap() (wrap-around neighbours, both gradients) matters.
export function heightImage() {
  const w = 16, h = 16, data = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      let n = (x * 7919 + y * 104729 + 12345) >>> 0;
      n = Math.imul(n ^ (n >>> 13), 0x5bd1e995) >>> 0;
      const noise = (n >>> 24) & 63;
      const ridge = Math.abs(x - 8) < 2 ? 120 : 0;
      const i = (y * w + x) * 4;
      data[i] = Math.min(255, x * 12 + noise + ridge);
      data[i + 1] = Math.min(255, y * 13 + (noise >> 1) + ridge);
      data[i + 2] = Math.min(255, ((x + y) * 6) + (noise >> 2));
      data[i + 3] = 255 - x;
    }
  }
  return { width: w, height: h, data };
}

const panel = (name, stage) => `textures/oax_proc/${name}
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm nolightmap
	{
${stage}
		rgbGen identity
	}
}
`;

const shaders = `// oax_proc test map shaders (tests/maps/src/oax_proc.mjs)
${panel('fire', '\t\tprocedural fire 128')}
${panel('water', '\t\tprocedural water 128 textures/gothic_block/blocks15.jpg')}
${panel('wet', '\t\tprocedural wet 128 textures/gothic_block/blocks15.jpg')}
${panel('ice', '\t\tprocedural ice 128 textures/gothic_block/blocks15.jpg')}
${panel('plasma', '\t\tprocedural plasma 128 1 0.5 0.9')}
${panel('heightmap', `\t\tmap ${HEIGHT_PROGRAM}`)}`;

export function build() {
  const map = new MapFile({ message: 'oax test: procedural textures' });
  map.brush(room([-768, -256, 0], [768, 256, 256], {
    floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c',
  }));
  PANELS.forEach((name, i) => {
    const [x] = panelCenter(i);
    map.brush(box([x - 96, PANEL_Y, 32], [x + 96, 256, 224], { ny: `oax_proc/${name}`, sides: 'common/caulk' }, { sx: 0.75, sy: 0.75 }));
  });
  map.entity('info_player_deathmatch', { origin: [0, -160, 32], angle: 90 });
  map.entity('info_player_deathmatch', { origin: [-400, -160, 32], angle: 90 });
  map.entity('light', { origin: [0, 0, 220], light: 500 });
  map.entity('light', { origin: [-500, 0, 220], light: 400 });
  map.entity('light', { origin: [500, 0, 220], light: 400 });
  return {
    map,
    manifest: { features: ['proc'] },
    files: {
      'scripts/oax_proc.shader': shaders,
      [HEIGHT_IMAGE]: encodeTga(heightImage()),
      'scripts/shaderlist.txt': shaderList('oax_proc'),
    },
  };
}
