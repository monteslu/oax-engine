// oax_hooks: the verification hooks and material keywords (step 7.5 D,
// docs/test-hooks.md, docs/materials.md).
//
// One arena with a sky portal ceiling, and a sealed sky room:
// - north wall: five panels, each its own material, so surface ids name
//   them: tint_plain / tint_red (oaxTint 1 0.5 0.5) and detail_none /
//   detail_always / detail_fade (detailFade 128 384), the last three the
//   same fullbright base texture under a high-contrast 2x-modulate detail
//   stage;
// - south half: things that animate: a tcMod scroll wall panel, a
//   func_rotating block, a flickering light style, a bobbing item, a
//   particle emitter; the sky turns 6 degrees a second;
// - a gargoyle model (func_static model2) in the arena, and one in the sky
//   room, which has no lights: its light grid is empty, so it is lit by
//   the worldspawn oaxSkyAmbient / oaxSkyLight (green).
// Brush entities with an origin key give their brushes relative to it:
// this q3map2 does not shift them.

import { MapFile, room, box } from '../mapwriter.mjs';
import { shaderList } from '../shaderlist.mjs';
import { PROBE, solid } from './oax_iso.mjs';

export const ARENA = { mins: [-512, -512, 0], maxs: [512, 512, 384] };
export const SKY_ROOM = { center: [3456, 0, 0], half: 384 };
export const SKY_ROTATE_YAW = 6;
export const PANEL_Z = [32, 224];
export const PANEL_Y = 496;              // panel faces at y = 496, facing -y
export const PANELS = {
  tint_plain: [-480, -352],
  tint_red: [-336, -208],
  detail_none: [-160, -32],
  detail_always: [16, 144],
  detail_fade: [192, 320],
};
export const DETAIL_FADE = [128, 384];
export const TINT = [1, 0.5, 0.5];
export const PROBE_PANEL = [496, 0, 128];   // its face center (x = 496, facing -x)
export const GARGOYLE = 'models/mapobjects/czest2ctf/gargoyle.md3';
export const ARENA_MODEL = [400, 300, 64];
export const SKY_MODEL_OFFSET = [0, 0, 72];   // above the sky camera
export const SKY_AMBIENT = [30, 150, 30];

const BASE = 'textures/base_wall/metalfloor_wall_10.jpg';
const DETAIL = 'textures/gothic_block/blocks15.jpg';

const lit = (name, extra, stages = '') => `textures/oax_hooks/${name}
{
	qer_editorimage ${BASE}
${extra}	{
		map $lightmap
		rgbGen identity
	}
	{
		map ${BASE}
		blendfunc filter
		rgbGen identity
	}
${stages}}
`;

// the detail panels are fullbright (no lightmap), so the three compare
// without the light falling differently on each
const full = (name, stages = '') => `textures/oax_hooks/${name}
{
	qer_editorimage ${BASE}
	surfaceparm nolightmap
	{
		map ${BASE}
		rgbGen identity
	}
${stages}}
`;

const detailStage = (fade) => `	{
		map ${DETAIL}
		blendfunc gl_dst_color gl_src_color
		tcMod scale 4 4
		detail
${fade ? `		detailFade ${DETAIL_FADE[0]} ${DETAIL_FADE[1]}\n` : ''}	}
`;

const shaders = `// oax_hooks test map shaders (tests/maps/src/oax_hooks.mjs)

${lit('tint_plain', '')}
${lit('tint_red', `	oaxTint ${TINT.join(' ')}\n`)}
${full('detail_none')}
${full('detail_always', detailStage(false))}
${full('detail_fade', detailStage(true))}
// scrolls 0.5 a second: any two shots at different shader times differ
textures/oax_hooks/scroll
{
	qer_editorimage textures/gothic_block/blocks15.jpg
	surfaceparm nolightmap
	{
		map textures/gothic_block/blocks15.jpg
		tcMod scroll 0.5 0.25
	}
}

textures/oax_hooks/sky_arena
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	surfaceparm skyportal
	skyparms env/moon1/moon1 - -
}

textures/oax_hooks/sky_room
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	skyparms env/nebulae/nebulae - -
}

textures/oax_hooks/marker
{
	qer_editorimage textures/base_light/light1red.jpg
	surfaceparm nolightmap
	{
		map $whiteimage
		rgbGen const ( 1 0.55 0.2 )
	}
}
`;

export function build() {
  const world = {
    message: 'oax test: verification hooks and material keywords',
    oaxSkyAmbient: SKY_AMBIENT.join(' '),
    oaxSkyLight: SKY_AMBIENT.join(' '),
  };
  for (let s = 32; s < 64; s++) world[`_style${s}rgbgen`] = `lightstyle ${s}`;
  const map = new MapFile(world);
  map.brush(room(ARENA.mins, ARENA.maxs, {
    floor: 'base_floor/clang_floor', ceiling: 'oax_hooks/sky_arena', walls: 'gothic_wall/oct20c',
  }));

  // north wall panels, 16 thick, faces at y = PANEL_Y
  for (const [name, [x0, x1]] of Object.entries(PANELS)) {
    map.brush(box([x0, PANEL_Y, PANEL_Z[0]], [x1, ARENA.maxs[1], PANEL_Z[1]], { ny: `oax_hooks/${name}`, sides: 'gothic_wall/oct20c' }));
  }
  // south wall: the scrolling panel
  map.brush(box([-128, ARENA.mins[1], 32], [128, ARENA.mins[1] + 16, 224], { py: 'oax_hooks/scroll', sides: 'gothic_wall/oct20c' }));
  // east wall: the content isolation probe (oax_iso.mjs): this map is
  // loose, so the later package's copy shows
  map.brush(box([ARENA.maxs[0] - 16, -64, 64], [ARENA.maxs[0], 64, 192], { nx: 'oax_iso/probe', sides: 'gothic_wall/oct20c' }));
  // a pillar for depth
  map.brush(box([-400, -48, 0], [-304, 48, 256], 'gothic_block/blocks15'));

  // animated things in the south half
  map.entity('func_rotating', { origin: [240, -300, 96], speed: 45 }, [
    // brush coordinates relative to the origin key (q3map2 does not shift them)
    box([-40, -40, -40], [40, 40, 40], 'oax_hooks/marker'),
  ]);
  map.entity('item_armor_shard', { origin: [-200, -320, 24] });
  map.entity('func_oax_emitter', { origin: [0, -380, 40], particle: 'oax_fx/smoke', seed: 11 });
  map.entity('light', { origin: [0, -300, 200], light: 500, targetname: 'flick', lightstyle_preset: 1 });

  // a model in the arena (surface id "model")
  map.entity('func_static', { origin: ARENA_MODEL, model2: GARGOYLE }, [
    box([-8, -8, -8], [8, 8, 8], 'common/clip'),
  ]);

  // the sky room: no lights, so its light grid is empty
  const [cx, cy, cz] = SKY_ROOM.center, h = SKY_ROOM.half;
  map.brush(room([cx - h, cy - h, cz - h], [cx + h, cy + h, cz + h], 'oax_hooks/sky_room'));
  map.entity('misc_oax_skyportal', { origin: SKY_ROOM.center, rotate: `0 ${SKY_ROTATE_YAW} 0` });
  const sm = SKY_ROOM.center.map((v, i) => v + SKY_MODEL_OFFSET[i]);
  map.entity('func_static', { origin: sm, model2: GARGOYLE }, [
    box([-8, -8, -8], [8, 8, 8], 'common/clip'),
  ]);
  // something fixed in the sky room besides the model, so a turned sky shows
  map.brush(box([cx + 100, cy - 40, cz + 200], [cx + 180, cy + 40, cz + 280], 'oax_hooks/marker'));

  map.entity('info_player_deathmatch', { origin: [0, 0, 32], angle: 90 });
  map.entity('info_player_deathmatch', { origin: [-200, 0, 32], angle: 270 });
  map.entity('light', { origin: [0, 300, 300], light: 900 });
  map.entity('light', { origin: [-300, 300, 300], light: 600 });
  map.entity('light', { origin: [300, 300, 300], light: 600 });
  map.entity('light', { origin: [0, -100, 300], light: 500 });
  return {
    map,
    manifest: { features: ['skyportal', 'lightstyle', 'particles'] },
    // the probe image lives only in oax_iso's packages; q3map2 needs one
    compileFiles: { [PROBE]: solid(255, 255, 255) },
    files: {
      'scripts/oax_hooks.shader': shaders,
      'scripts/shaderlist.txt': shaderList('oax_hooks'),
    },
  };
}
