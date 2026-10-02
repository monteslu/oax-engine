// oax_warp: a seamless warp zone (trigger_teleport spawnflag 4) and, for
// the controls, the same layout with a classic teleporter.
//
// Corridor A runs +X (x -640..0, y -96..96, z 0..160). Its warp plane is
// x = -32 (the trigger spans x -48..-16); the end wall at x = 0 carries a
// portal shader (oax_sim/warpportal) and a misc_portal_surface, so it shows
// corridor B. A player crossing the plane lands in corridor B (x 928..1120) running +Y:
// the frame turns 90 degrees about Z, centre (-32 0 80) -> (1024 -320 80).
// The portal camera sits where the end wall maps to: 32 units past the
// target along +Y.
//
// Corridor C (A moved +1200 in Y) ends in a classic trigger_teleport to
// corridor D: the control, which kicks the player out at 400 ups.

import { MapFile, box } from '../mapwriter.mjs';

const FLOOR = 'base_floor/clang_floor', CEIL = 'base_floor/concrete', WALL = 'gothic_wall/oct20c';

// a sealed corridor [mins, maxs] (inside); `endTex` on the +X or +Y inner end face
function corridor(map, mins, maxs, { endAxis = null, endTex = WALL } = {}) {
  const [x0, y0, z0] = mins, [x1, y1, z1] = maxs, w = 16;
  map.brush(
    box([x0 - w, y0 - w, z0 - w], [x1 + w, y1 + w, z0], { top: FLOOR }),
    box([x0 - w, y0 - w, z1], [x1 + w, y1 + w, z1 + w], { bottom: CEIL }),
    box([x0 - w, y0, z0], [x0, y1, z1], { px: WALL }),
    box([x1, y0, z0], [x1 + w, y1, z1], { nx: endAxis === 'x' ? endTex : WALL }),
    box([x0 - w, y0 - w, z0], [x1 + w, y0, z1], { py: WALL }),
    box([x0 - w, y1, z0], [x1 + w, y1 + w, z1], { ny: endAxis === 'y' ? endTex : WALL }),
  );
}

// A see-through portal face. common/portal has no `alphaGen portal`, so its
// portal range is 0 and the renderer only ever uses it as a mirror.
const SHADERS = `textures/oax_sim/warpportal
{
	qer_editorimage textures/common/invisible.tga
	surfaceparm nolightmap
	portal
	{
		map textures/common/invisible.tga
		blendfunc GL_ONE GL_ONE_MINUS_SRC_ALPHA
		alphagen portal 8192
		depthWrite
	}
}
`;

export const WARP = { center: [-32, 0, 80], dest: [1024, -320, 80], destYaw: 90 };
export const CLASSIC = { dy: 1200, dest: [1024, 880, 24], destYaw: 90 };

export function build() {
  const map = new MapFile({ message: 'oax test: warp' });

  // A and B: the seamless warp
  corridor(map, [-640, -96, 0], [0, 96, 160], { endAxis: 'x', endTex: 'oax_sim/warpportal' });
  corridor(map, [928, -352, 0], [1120, 960, 160]);
  map.entity('trigger_teleport', { spawnflags: 4, target: 'warpB', angle: 0 }, [box([-48, -96, 0], [-16, 96, 160], 'common/trigger')]);
  map.entity('misc_teleporter_dest', { targetname: 'warpB', origin: WARP.dest, angle: WARP.destYaw });
  map.entity('misc_portal_surface', { target: 'warpcam', origin: [-4, 0, 80] });
  // noswing (4); the 180 roll lines Q3's portal camera basis up with the surface's
  map.entity('misc_portal_camera', { targetname: 'warpcam', origin: [1024, -288, 80], angle: 90, roll: 180, spawnflags: 4 });

  // C and D: the classic teleporter (control)
  const dy = CLASSIC.dy;
  corridor(map, [-640, dy - 96, 0], [0, dy + 96, 160]);
  corridor(map, [928, dy - 352, 0], [1120, dy + 960, 160]);
  map.entity('trigger_teleport', { target: 'classicD' }, [box([-48, dy - 96, 0], [-16, dy + 96, 160], 'common/trigger')]);
  map.entity('misc_teleporter_dest', { targetname: 'classicD', origin: CLASSIC.dest, angle: CLASSIC.destYaw });

  map.entity('info_player_deathmatch', { origin: [-560, 0, 32], angle: 0 });
  map.entity('info_player_deathmatch', { origin: [-560, dy, 32], angle: 0 });
  for (const [x, y] of [[-480, 0], [-160, 0], [1024, -200], [1024, 300], [1024, 800], [-480, dy], [-160, dy], [1024, dy - 200], [1024, dy + 500]]) {
    map.entity('light', { origin: [x, y, 120], light: 70 });
  }
  return { map, manifest: { features: ['warp'] }, files: { 'scripts/oax_sim.shader': SHADERS } };
}
