// oax_albedo: rendered albedo at a known ambient (step 7.5 B). No lights;
// worldspawn oax_ambient 1 1 1, so every surface must render at exactly
// texel x tint. Panels on the floor of one room, seen from above by
// tests/romdev/tests/ulight-albedo.mjs:
//   flat     a brush face, flat texture (100, 150, 200)
//   noise    a brush face, a noise texture with a known mean
//   tint     the flat texture through a material with oaxTint 0.8 0.6 1.2
//   surf     an OAX_SURFACES quad of the flat texture, surface tint 0.5 1 1.5
//   zone     the flat texture inside a func_oax_zone with ambient 0.5 0.5 0.5
// oax_albedo_ob2 is the same map with oax_overbright 2 (must not change
// ambient: the overbright ceiling is per light).

import { MapFile, room, box } from '../mapwriter.mjs';
import { SurfaceWorld, uvMatrix } from '../../../misc/tools/oax-surfaces.mjs';
import { tga, image, hash2 } from '../lib/texgen.mjs';

export const FLAT = [100, 150, 200];
export const NOISE = (x, y) => [40 + Math.floor(180 * hash2(x, y, 1)), 40 + Math.floor(180 * hash2(x, y, 2)), 40 + Math.floor(180 * hash2(x, y, 3))];
export const NOISE_SIZE = 64;
export const TINT = [0.8, 0.6, 1.2];
export const SURF_TINT = [0.5, 1, 1.5];
export const ZONE_AMBIENT = 0.5;
// panel centres (x, y), each 192 x 192, on the floor (z 0)
export const PANELS = { flat: [-256, 256], noise: [0, 256], tint: [256, 256], surf: [-256, -256], surfzone: [0, -256], zone: [256, -256] };
export const HALF = 96;

const T = 'oax_albedo';

export function buildAlbedo({ overbright = 1 } = {}) {
  const map = new MapFile({
    message: `oax test: albedo${overbright > 1 ? ' (overbright 2)' : ''}`,
    oax_lighting: 'unified',
    oax_ambient: '1 1 1',
    oax_overbright: overbright,
    _keepLights: 1,
  });
  map.brush(room([-512, -512, -16], [512, 512, 512], { floor: `${T}/black`, ceiling: `${T}/black`, walls: `${T}/black` }));
  const panel = (name, tex) => {
    const [x, y] = PANELS[name];
    // a thin slab standing on the floor: its top face is the panel
    map.brush(box([x - HALF, y - HALF, -16], [x + HALF, y + HALF, 0], { top: tex, sides: 'common/caulk', bottom: 'common/caulk' }));
  };
  panel('flat', `${T}/flat`);
  panel('noise', `${T}/noise`);
  panel('tint', `${T}/tinted`);
  panel('zone', `${T}/flat_zone`);
  const [zx, zy] = PANELS.zone;
  map.entity('func_oax_zone', { ambient: `${ZONE_AMBIENT} ${ZONE_AMBIENT} ${ZONE_AMBIENT}` }, [box([zx - HALF - 32, zy - HALF - 32, -8], [zx + HALF + 32, zy + HALF + 32, 64], 'common/trigger')]);
  // a second zone around the surface-world panel surfzone
  const [qx, qy] = PANELS.surfzone;
  map.entity('func_oax_zone', { ambient: `${ZONE_AMBIENT} ${ZONE_AMBIENT} ${ZONE_AMBIENT}` }, [box([qx - HALF - 16, qy - HALF - 16, -8], [qx + HALF + 16, qy + HALF + 16, 64], 'common/trigger')]);
  map.entity('info_player_deathmatch', { origin: [0, -400, 24], angle: 90 });

  // the surface-world quad, 1 unit above the floor
  const sw = new SurfaceWorld();
  const [sx, sy] = PANELS.surf;
  sw.polygon({
    material: `textures/${T}/flat_surf`,
    points: [[sx - HALF, sy - HALF, 1], [sx + HALF, sy - HALF, 1], [sx + HALF, sy + HALF, 1], [sx - HALF, sy + HALF, 1]],
    uv: uvMatrix({ u: [1 / 64, 0, 0], v: [0, -1 / 64, 0] }),
    tint: SURF_TINT,
  });
  const [zsx, zsy] = PANELS.surfzone;
  sw.polygon({
    material: `textures/${T}/flat_surf`,
    points: [[zsx - HALF, zsy - HALF, 1], [zsx + HALF, zsy - HALF, 1], [zsx + HALF, zsy + HALF, 1], [zsx - HALF, zsy + HALF, 1]],
    uv: uvMatrix({ u: [1 / 64, 0, 0], v: [0, -1 / 64, 0] }),
  });

  const flat = tga(8, 8, image(8, 8, () => [...FLAT.map((v) => v / 255), 1]));
  const noise = tga(NOISE_SIZE, NOISE_SIZE, image(NOISE_SIZE, NOISE_SIZE, (x, y) => [...NOISE(x, y).map((v) => v / 255), 1]));
  const black = tga(8, 8, image(8, 8, () => [0, 0, 0, 1]));
  const sh = (name, img, extra = '') => `textures/${T}/${name}
{
	qer_editorimage textures/${T}/${img}.tga
${extra}	diffusemap textures/${T}/${img}.tga
	specularmap _black
}
`;
  const shader = [sh('flat', 'flat'), sh('flat_zone', 'flat'), sh('flat_surf', 'flat'), sh('noise', 'noise'), sh('tinted', 'flat', `\toaxTint ${TINT.join(' ')}\n`), sh('black', 'black')].join('\n');
  return {
    map,
    light: 'none',
    surfaces: sw,
    files: {
      [`textures/${T}/flat.tga`]: flat,
      [`textures/${T}/noise.tga`]: noise,
      [`textures/${T}/black.tga`]: black,
      'scripts/oax_albedo.shader': shader,
    },
    manifest: { features: ['ulight', 'surfaces'], lighting: 'unified' },
  };
}

export function build() {
  return buildAlbedo();
}
