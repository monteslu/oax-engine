// oax_ulight_phys: the physical light description (step 7.5 B,
// docs/lights.md). One hall along +x with a "room" every 2048 units, each a flat grey
// floor (texture 128) under one light, so tests/romdev/tests/ulight-physical.mjs
// can look straight down at each and compare the floor with a JS oracle of
// the profile (tests/romdev/lib/lightoracle.mjs):
//   ue1      a UE1 lamp (hue 32, saturation 128, brightness 96, radius 18): the line, its ceiling, unnormalised color
//   q3       q3map2 inverse square (light 300)
//   q3lin    q3map2 linear (spawnflags 1: no angle term)
//   doom3    light_radius 400, _color 1 0.5 0.25
//   custom   a falloff table, a soft cap (knee), no angular term, a color over 1
//   image    a falloff image
//   mask     a light in group 2 over a floor half in the default group, half in group 2
//   zone     no light: a func_oax_zone with `ambient` over half the floor, named
//   zonectl  the same zone without `ambient` (control), named too
//   effect   a pulse: oax_effect oax_sin
//   park     no light: where the player waits
// The map is unified with no world ambient and oax_overbright 2.

import { MapFile, room, box } from '../mapwriter.mjs';
import { tga, image } from '../lib/texgen.mjs';

export const SPACING = 2048, HALF = 512, HEIGHT = 768;
export const ALBEDO = 128;
export const centre = (i) => [i * SPACING, 0, 0];

export const ROOMS = [
  { name: 'ue1', h: 120, keys: { ue1_LightBrightness: 96, ue1_LightHue: 32, ue1_LightSaturation: 128, ue1_LightRadius: 18 } },
  { name: 'q3', h: 100, keys: { oax_profile: 'q3', light: 300 } },
  { name: 'q3lin', h: 100, keys: { oax_profile: 'q3', light: 300, spawnflags: 1, _color: '1 0.8 0.6' } },
  { name: 'doom3', h: 100, keys: { oax_profile: 'doom3', light_radius: '400 400 400', _color: '1 0.5 0.25' } },
  { name: 'custom', h: 150, keys: { oax_radius: 450, oax_falloff: 'table 0 1 0.3 0.9 0.6 0.4 1 0', oax_intensity: 1.6, oax_cap: 1.2, oax_capKnee: 0.3, oax_angular: 'none', oax_color: '0.9 1 1.1' } },
  { name: 'image', h: 100, keys: { oax_radius: 400, oax_falloff: 'image textures/oax_phys/ramp.tga', oax_intensity: 1.2 } },
  { name: 'mask', h: 120, keys: { oax_radius: 450, oax_intensity: 1, oax_mask: 2 } },
  { name: 'zone', h: 0, keys: null, zone: { ambient: '0.3 0.2 0.1', name: 'Amber Hall' } },
  { name: 'zonectl', h: 0, keys: null, zone: { name: 'Plain Hall' } },
  { name: 'effect', h: 120, keys: { oax_radius: 450, oax_intensity: 1, oax_effect: 'oax_sin 0.5 0 0.6 0.39' } },
  { name: 'park', h: 0, keys: null },     // the player waits here, out of every measured view
];
export const roomIndex = (n) => ROOMS.findIndex((r) => r.name === n);

// the image falloff: 64 texels of sqrt(1 - x), 8 bits
export const RAMP = Array.from({ length: 64 }, (_, i) => Math.round(Math.sqrt(Math.max(0, 1 - (i + 0.5) / 64)) * 255));

export function build() {
  const map = new MapFile({
    message: 'oax test: physical lights',
    oax_lighting: 'unified',
    oax_ambient: '0 0 0',
    oax_shadowmode: 'maps',
    oax_overbright: 2,
    _keepLights: 1,
  });
  const G = 'oax_phys/grey', B = 'oax_phys/grey_b', M = 'oax_phys/grey_mask2', W = 'oax_phys/wall';
  // one hall (one area, so the player parked at one end leaves every light
  // and surface visible to the camera); each room is a floor segment
  const hall = room([-SPACING / 2, -HALF, 0], [(ROOMS.length - 0.5) * SPACING, HALF, HEIGHT], { floor: 'common/caulk', ceiling: W, walls: W });
  map.brush(hall.slice(1));
  ROOMS.forEach((r, i) => {
    const [cx] = centre(i);
    const x0 = cx - SPACING / 2, x1 = cx + SPACING / 2;
    const floor = { sides: 'common/caulk', bottom: 'common/caulk' };
    if (r.name === 'mask' || r.zone) {
      // the floor in two halves at y = 0 with different materials (so q3map2
      // keeps them apart): mask: y > 0 in light group 2; zones: y > 0 inside
      map.brush(box([x0, -HALF - 16, -16], [x1, 0, 0], { ...floor, top: G }));
      map.brush(box([x0, 0, -16], [x1, HALF + 16, 0], { ...floor, top: r.zone ? B : M }));
    } else {
      map.brush(box([x0, -HALF - 16, -16], [x1, HALF + 16, 0], { ...floor, top: G }));
    }
    if (r.keys) map.entity('light', { origin: [cx, 0, r.h], ...r.keys });
    if (r.zone) {
      // the zone covers y > 0 of the room, floor to ceiling
      map.entity('func_oax_zone', r.zone, [box([cx - HALF, 0, 0], [cx + HALF, HALF, HEIGHT], 'common/trigger')]);
    }
  });
  map.entity('info_player_deathmatch', { origin: [roomIndex('park') * SPACING, 0, 24], angle: 90 });
  const flat = (v) => tga(8, 8, image(8, 8, () => [v / 255, v / 255, v / 255, 1]));
  const ramp = tga(64, 4, image(64, 4, (x) => { const v = RAMP[x] / 255; return [v, v, v, 1]; }));
  const shader = `textures/${G}
{
	qer_editorimage textures/${G}.tga
	diffusemap textures/${G}.tga
	specularmap _black
}

textures/${B}
{
	qer_editorimage textures/${G}.tga
	diffusemap textures/${G}.tga
	specularmap _black
}

textures/${M}
{
	qer_editorimage textures/${G}.tga
	oaxLightMask 2
	diffusemap textures/${G}.tga
	specularmap _black
}

textures/${W}
{
	qer_editorimage textures/${W}.tga
	diffusemap textures/${W}.tga
	specularmap _black
}
`;
  return {
    map,
    light: 'none',
    files: {
      [`textures/${G}.tga`]: flat(ALBEDO),
      [`textures/${W}.tga`]: flat(64),
      'textures/oax_phys/ramp.tga': ramp,
      'scripts/oax_phys.shader': shader,
    },
    manifest: { features: ['ulight'], lighting: 'unified' },
  };
}
