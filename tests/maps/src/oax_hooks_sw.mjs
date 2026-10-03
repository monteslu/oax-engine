// oax_hooks_sw: detailFade on every path a detail stage can take (step 7.5
// D, docs/materials.md): a unified-lighting map whose visible walls are
// surface-world (OAX_SURFACES) surfaces over a caulk hull, plus one brush
// face, so the same detail stage is drawn on
// - surface-world surfaces under unified lighting (the stage pass after
//   the light interactions),
// - a tinted surface-world surface (its shader is a tinted variant),
// - an ordinary brush face of the same map.
// The detail stage is a flat green 2x modulate (as agent E's probe):
// at full strength it turns the wall green, faded out it has no effect.
// Panels on the north wall (faces at y = PANEL_Y, facing -y):
//   sw_plain  the base material, no detail
//   sw_always the base + the green detail stage
//   sw_fade   the base + the green detail stage with detailFade 0 FADE_END
//   sw_fade (tinted surface), and the brush face with sw_fade.

import { MapFile, box } from '../mapwriter.mjs';
import { SurfaceWorld, uvMatrix } from '../../../misc/tools/oax-surfaces.mjs';
import { tga, image, hash2 } from '../lib/texgen.mjs';

const T = 'textures/oax_hooks_sw';
export const MAT = { base: `${T}/base`, plain: `${T}/sw_plain`, always: `${T}/sw_always`, fade: `${T}/sw_fade`, floor: `${T}/floor`, wall: `${T}/wall`,
  fadeVertex: `${T}/sw_fade_vertex`, floorVertex: `${T}/floor_fade_vertex`, floorPixel: `${T}/floor_fade_pixel` };
export const FADE_END = 380;
export const ROOM = { x: [-640, 640], y: [-1024, 256], z: [0, 256] };
export const PANEL_Y = 254;
export const PANEL_Z = [32, 224];
export const PANELS = {
  plain: [-600, -480],
  always: [-400, -280],
  fade: [-200, -80],
  fadeTinted: [0, 120],
  fadeVertex: [200, 320],
  fadeBrush: [400, 520],
};
// two big floor quads (z FLOOR_Z), the same green detail with detailFade
// 0 FADE_END per vertex and per pixel; seen from FLOOR_EYE (32 up, 45
// degrees down) every corner's view depth is beyond FADE_END (the near
// ones behind the eye), while the centre pixel is about 42 deep
export const FLOOR_Z = 2;
export const FLOOR_QUADS = { vertex: [-620, -20], pixel: [20, 620] };
export const FLOOR_Y = [-1000, 240];
export const FLOOR_EYE = { y: -380, z: 32, pitch: 45 };
export const TINT = [1, 1, 1, 1];   // a neutral tint: the variant path without changing the colour
const CAULK = 'common/caulk';

const detail = (fade, mode = '') => `	{
		map $whiteimage
		rgbGen const ( 0 1 0 )
		blendFunc GL_DST_COLOR GL_SRC_COLOR
${fade ? `		detailFade 0 ${FADE_END}${mode ? ` ${mode}` : ''}\n` : ''}	}
`;
const mat = (name, stages = '') => `${name}
{
	qer_editorimage ${MAT.base}.tga
	diffusemap ${MAT.base}.tga
${stages}}
`;
const shaders = `// oax_hooks_sw test map shaders (tests/maps/src/oax_hooks_sw.mjs)
${mat(MAT.plain)}
${mat(MAT.always, detail(false))}
${mat(MAT.fade, detail(true))}
${mat(MAT.fadeVertex, detail(true, 'vertex'))}
${mat(MAT.floorVertex, detail(true, 'vertex'))}
${mat(MAT.floorPixel, detail(true, 'pixel'))}
${mat(MAT.floor)}
${mat(MAT.wall)}
`;

function rectY(sw, y, x, z, material, extra = {}) {
  // facing -y: counter-clockwise seen from -y
  const pts = [[x[0], y, z[0]], [x[1], y, z[0]], [x[1], y, z[1]], [x[0], y, z[1]]];
  sw.polygon({ material, points: pts, uv: uvMatrix({ u: [1 / 128, 0, 0], v: [0, 0, -1 / 128] }), ...extra });
}

export function build() {
  const map = new MapFile({
    message: 'oax test: detailFade on surface-world surfaces under unified lighting',
    oax_lighting: 'unified',
    oax_ambient: '0.15 0.15 0.15',
    _keepLights: 1,
  });
  const [x0, x1] = ROOM.x, [y0, y1] = ROOM.y, [z0, z1] = ROOM.z, w = 16;
  // the hull: caulk; the north wall is caulk too except where the brush panel is
  map.brush(box([x0 - w, y0 - w, z0 - w], [x1 + w, y1 + w, z0], CAULK));
  map.brush(box([x0 - w, y0 - w, z1], [x1 + w, y1 + w, z1 + w], CAULK));
  map.brush(box([x0 - w, y0 - w, z0], [x0, y1 + w, z1], CAULK));
  map.brush(box([x1, y0 - w, z0], [x1 + w, y1 + w, z1], CAULK));
  map.brush(box([x0, y0 - w, z0], [x1, y0, z1], CAULK));
  map.brush(box([x0, y1, z0], [x1, y1 + w, z1], CAULK));
  // the brush-face panel, standing just off the wall
  const b = PANELS.fadeBrush;
  map.brush(box([b[0], y1 - 8, PANEL_Z[0]], [b[1], y1, PANEL_Z[1]], { ny: MAT.fade.slice('textures/'.length), sides: CAULK }));

  const sw = new SurfaceWorld();
  // floor, ceiling and walls
  sw.polygon({ material: MAT.floor, points: [[x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0]], uv: uvMatrix({ u: [1 / 128, 0, 0], v: [0, -1 / 128, 0] }) });
  sw.polygon({ material: MAT.wall, points: [[x0, y0, z1], [x0, y1, z1], [x1, y1, z1], [x1, y0, z1]], uv: uvMatrix({ u: [1 / 128, 0, 0], v: [0, -1 / 128, 0] }) });
  rectY(sw, y1 - 1, [x0, x1], [z0, z1], MAT.wall);
  // the other three walls, a unit inside the hull
  const uvx = uvMatrix({ u: [0, 1 / 128, 0], v: [0, 0, -1 / 128] }), uvy = uvMatrix({ u: [1 / 128, 0, 0], v: [0, 0, -1 / 128] });
  sw.polygon({ material: MAT.wall, points: [[x0 + 1, y0, z0], [x0 + 1, y1, z0], [x0 + 1, y1, z1], [x0 + 1, y0, z1]], uv: uvx });
  sw.polygon({ material: MAT.wall, points: [[x1 - 1, y0, z0], [x1 - 1, y0, z1], [x1 - 1, y1, z1], [x1 - 1, y1, z0]], uv: uvx });
  sw.polygon({ material: MAT.wall, points: [[x0, y0 + 1, z0], [x0, y0 + 1, z1], [x1, y0 + 1, z1], [x1, y0 + 1, z0]], uv: uvy });
  // the panels, half a unit in front of the north wall
  rectY(sw, PANEL_Y, PANELS.plain, PANEL_Z, MAT.plain);
  rectY(sw, PANEL_Y, PANELS.always, PANEL_Z, MAT.always);
  rectY(sw, PANEL_Y, PANELS.fade, PANEL_Z, MAT.fade);
  rectY(sw, PANEL_Y, PANELS.fadeTinted, PANEL_Z, MAT.fade, { tint: [0.999, 0.999, 0.999, 1] });
  rectY(sw, PANEL_Y, PANELS.fadeVertex, PANEL_Z, MAT.fadeVertex);
  // the floor quads (facing up)
  for (const [k, m] of [['vertex', MAT.floorVertex], ['pixel', MAT.floorPixel]]) {
    const [qa, qb] = FLOOR_QUADS[k];
    sw.polygon({ material: m, points: [[qa, FLOOR_Y[0], FLOOR_Z], [qb, FLOOR_Y[0], FLOOR_Z], [qb, FLOOR_Y[1], FLOOR_Z], [qa, FLOOR_Y[1], FLOOR_Z]], uv: uvMatrix({ u: [1 / 128, 0, 0], v: [0, -1 / 128, 0] }) });
  }

  map.entity('info_player_deathmatch', { origin: [0, -600, 24], angle: 90 });
  map.entity('light', { origin: [0, 0, 200], light_radius: [1400, 1400, 800], _color: [0.8, 0.8, 0.8] });

  const N = 128;
  const files = {
    [`${MAT.base}.tga`]: tga(N, N, image(N, N, (x, y) => {
      const v = 0.55 + 0.2 * hash2(x >> 3, y >> 3, 5);
      return [v, v * 0.9, v * 0.85, 1];
    })),
    'scripts/oax_hooks_sw.shader': shaders,
  };
  return { map, light: 'none', aas: false, files, surfaces: sw, manifest: { features: ['ulight', 'surfaces'], lighting: 'unified' } };
}
