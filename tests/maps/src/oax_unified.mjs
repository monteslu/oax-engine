// oax_unified: the Doom 3-class unified-lighting test map (phase 5).
//
// Three rooms in a row along x, joined by doors whose areaportals close
// the rooms off from each other:
//   A (west)   pillar hall: a row of pillars under a point light casts long
//              shadows; a projected spot light with our own gobo image;
//   B (middle) a light bound to a func_bobbing mover and a flickering light
//              driven by a table;
//   C (east)   a point light and a spot light, culled by area when door BC
//              is closed.
// Floors and walls are bump mapped with procedural art from texgen.mjs.
// The worldspawn selects unified lighting with a blue ambient; there is no
// q3map2 -light stage.

import { MapFile, box } from '../mapwriter.mjs';
import { tga, bricks, tiles, normalMap, image, gobo } from '../lib/texgen.mjs';

// Light values are doubled (x2): the map was authored when unified light drew
// at 2x on screen; since the frame contract (docs/lights.md: light 1 is the
// texture at 1x) the same frame needs twice the light.
const x2 = (c) => c.map((v) => v * 2);

const T = 'textures/oax_unified';
const M = 'oax_unified'; // map texture names omit the textures/ prefix
const WALL = `${M}/brick`, FLOOR = `${M}/floor`, CEIL = `${M}/ceil`;
const tx = (m) => `textures/${m}`;
const DOOR = 'base_door/shinymetaldoor', TRIM = 'base_trim/pewter';

const Z0 = 0, Z1 = 288, TH = 16;
const Y0 = -384, Y1 = 384;
// x extents of the rooms (insides); dividing walls are 32 thick
export const ROOMS = {
  A: [-1344, -320],
  B: [-288, 288],
  C: [320, 1344],
};
const DOOR_Y = [-64, 64], DOOR_Z = 128;

function art() {
  const files = {};
  const N = 256;
  const b = bricks(N, N);
  files[`${tx(WALL)}.tga`] = tga(N, N, image(N, N, b.color));
  files[`${tx(WALL)}_n.tga`] = tga(N, N, normalMap(N, N, b.height, 6));
  files[`${tx(WALL)}_s.tga`] = tga(N, N, image(N, N, b.spec));
  const f = tiles(N, N);
  files[`${tx(FLOOR)}.tga`] = tga(N, N, image(N, N, f.color));
  files[`${tx(FLOOR)}_n.tga`] = tga(N, N, normalMap(N, N, f.height, 5));
  files[`${tx(FLOOR)}_s.tga`] = tga(N, N, image(N, N, f.spec));
  const c = tiles(N, N, { size: 128, grout: 4, base: [0.45, 0.46, 0.5] });
  files[`${tx(CEIL)}.tga`] = tga(N, N, image(N, N, c.color));
  files[`${tx(CEIL)}_n.tga`] = tga(N, N, normalMap(N, N, c.height, 3));
  files[`${T}/gobo.tga`] = tga(128, 128, gobo(128));
  return files;
}

// materials: Q3 shader syntax with the id Tech 4 stage shorthands
const shaders = `
${tx(WALL)}
{
	qer_editorimage ${tx(WALL)}.tga
	diffusemap ${tx(WALL)}.tga
	bumpmap ${tx(WALL)}_n.tga
	specularmap ${tx(WALL)}_s.tga
}

${tx(FLOOR)}
{
	qer_editorimage ${tx(FLOOR)}.tga
	diffusemap ${tx(FLOOR)}.tga
	bumpmap ${tx(FLOOR)}_n.tga
	specularmap ${tx(FLOOR)}_s.tga
}

${tx(CEIL)}
{
	qer_editorimage ${tx(CEIL)}.tga
	diffusemap ${tx(CEIL)}.tga
	bumpmap ${tx(CEIL)}_n.tga
}

lights/oax_gobo
{
	lightFalloffImage _noFalloff
	{
		map ${T}/gobo.tga
	}
}

lights/oax_flicker
{
	lightFalloffImage _quadratic
	{
		map _pointlight
		rgb oaxflicker[ time * 2 ]
	}
}
`;

// a flicker curve sampled at 10 steps per cycle, snapped (no blending)
const tables = `
table oaxflicker { snap { 1, 0.9, 1, 0.25, 1, 1, 0.6, 1, 0.1, 0.95 } }
`;

// the rooms, doors, pillars, crate and spawn points (shared with the
// ulight-perf maps)
export function geometry(map) {
  const xa = ROOMS.A[0], xc = ROOMS.C[1];
  const tex = (k) => ({ top: k, bottom: k, sides: k });
  // floor, ceiling, outer walls around all three rooms
  map.brush(box([xa - TH, Y0 - TH, Z0 - TH], [xc + TH, Y1 + TH, Z0], { top: FLOOR, sides: 'common/caulk', bottom: 'common/caulk' }));
  map.brush(box([xa - TH, Y0 - TH, Z1], [xc + TH, Y1 + TH, Z1 + TH], { bottom: CEIL, sides: 'common/caulk', top: 'common/caulk' }));
  map.brush(box([xa - TH, Y0 - TH, Z0], [xa, Y1 + TH, Z1], { px: WALL, sides: 'common/caulk' }));
  map.brush(box([xc, Y0 - TH, Z0], [xc + TH, Y1 + TH, Z1], { nx: WALL, sides: 'common/caulk' }));
  map.brush(box([xa, Y0 - TH, Z0], [xc, Y0, Z1], { py: WALL, sides: 'common/caulk' }));
  map.brush(box([xa, Y1, Z0], [xc, Y1 + TH, Z1], { ny: WALL, sides: 'common/caulk' }));

  // dividing walls with a doorway, and a door with its areaportal
  for (const [name, x0, x1] of [['ab', ROOMS.A[1], ROOMS.B[0]], ['bc', ROOMS.B[1], ROOMS.C[0]]]) {
    const w = { px: WALL, nx: WALL, sides: TRIM, top: 'common/caulk', bottom: 'common/caulk' };
    map.brush(box([x0, Y0, Z0], [x1, DOOR_Y[0], Z1], w));
    map.brush(box([x0, DOOR_Y[1], Z0], [x1, Y1, Z1], w));
    map.brush(box([x0, DOOR_Y[0], DOOR_Z], [x1, DOOR_Y[1], Z1], { px: WALL, nx: WALL, sides: TRIM }));
    map.entity('func_door', { angle: -1, lip: 8, speed: 400, wait: 2 }, [
      box([x0 + 4, DOOR_Y[0], Z0], [x1 - 4, DOOR_Y[1], DOOR_Z], DOOR),
    ]);
    // q3map2 only takes areaportals in the world; the door spans both areas
    map.brush(box([x0 + 14, DOOR_Y[0], Z0], [x1 - 14, DOOR_Y[1], DOOR_Z], 'common/areaportal'));
  }

  // room A: a row of pillars
  for (const px of [-1180, -980, -780, -580]) {
    map.brush(box([px - 24, -24, Z0], [px + 24, 24, Z1], tex(WALL)));
  }

  // spawn points (one per room) and the things the tests stand on
  map.entity('info_player_deathmatch', { origin: [-1250, -300, 24], angle: 0 });
  map.entity('info_player_deathmatch', { origin: [-200, -300, 24], angle: 0 });
  map.entity('info_player_deathmatch', { origin: [1250, -300, 24], angle: 180 });
  // a crate for the room C light to shadow
  map.brush(box([780, -160, Z0], [860, -80, 80], tex(WALL)));
}

export function materials() {
  return { ...art(), 'scripts/oax_unified.shader': shaders, 'scripts/oax_unified.table': tables };
}

export function build() {
  const map = new MapFile({
    message: 'oax test: unified lighting',
    oax_lighting: 'unified',
    oax_ambient: x2([0.035, 0.045, 0.11]).join(' '),
    oax_shadowmode: 'maps',
    _keepLights: 1,
  });
  geometry(map);

  // lights. Point lights use D3 keys; light_radius bounds the light.
  map.entity('light', { origin: [-880, 300, 240], light_radius: [1000, 1000, 800], _color: x2([1.6, 1.5, 1.3]) });
  map.entity('light', {
    origin: [-1250, -250, 270], light_target: [0, 0, -270], light_end: [0, 0, -330], light_right: [140, 0, 0], light_up: [0, 140, 0],
    texture: 'lights/oax_gobo', _color: x2([1, 1, 1]),
  });
  // room B: a light bound to a bobbing mover, and a flickering light
  map.entity('func_bobbing', { targetname: 'bobber', spawnflags: 1, height: 160, speed: 6 }, [
    box([-8, -8, 192], [8, 8, 208], 'common/nodraw'),
  ]);
  map.entity('light', { origin: [0, 0, 200], light_radius: [260, 260, 260], _color: x2([0.4, 0.9, 0.5]), bind: 'bobber' });
  map.entity('light', { origin: [0, 330, 220], light_radius: [240, 240, 240], _color: x2([1, 0.6, 0.3]), texture: 'lights/oax_flicker' });
  // room C (kept inside room C so door BC culls it by area)
  map.entity('light', { origin: [830, 0, 200], light_radius: [420, 360, 520], _color: x2([1.2, 1.25, 1.5]) });
  map.entity('light', {
    origin: [1250, 250, 270], light_target: [0, 0, -270], light_end: [0, 0, -330], light_right: [110, 0, 0], light_up: [0, 110, 0],
    texture: 'lights/oax_gobo', _color: x2([1, 0.5, 0.5]),
  });

  return {
    map,
    light: 'none',
    files: materials(),
    manifest: { features: ['ulight'], lighting: 'unified' },
  };
}
