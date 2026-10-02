// oax_skyportal: a sky seen through a sky portal (misc_oax_skyportal).
//
// The arena's ceiling is a skyportal sky (fallback: the moon1 skybox). A
// sealed 768-unit sky room far away holds the portal camera, nebula sky
// walls, a flat green "moon" block 50 degrees up toward yaw 45 and a
// func_rotating "planet" (a block straight above the camera that spins,
// and one that orbits at eye level, below the moon). The whole sky also
// turns 6 degrees a second (rotate key).

import { MapFile, room, box } from '../mapwriter.mjs';
import { shaderList } from '../shaderlist.mjs';

export const SKY_ROOM = { center: [3456, 0, 0], half: 384 };
export const MOON_DIR = { yaw: 45, pitch: 50 };   // degrees, in the sky room
export const SKY_ROTATE_YAW = 6;                  // degrees per second
export const ARENA = { mins: [-512, -512, 0], maxs: [512, 512, 512] };

const shaders = `// oax_skyportal test map shaders (tests/maps/src/oax_skyportal.mjs)

// the arena sky: a sky portal where the engine and cgame support it,
// the moon1 skybox where they do not
textures/oax_skyportal/sky_arena
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	surfaceparm skyportal
	skyparms env/moon1/moon1 - -
}

// the sky room's own walls: a nebula skybox
textures/oax_skyportal/sky_room
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm noimpact
	surfaceparm nolightmap
	surfaceparm sky
	skyparms env/nebulae/nebulae - -
}

// fullbright sky objects
textures/oax_skyportal/planet
{
	qer_editorimage textures/base_light/light1red.jpg
	surfaceparm nolightmap
	{
		map textures/base_light/light1red.jpg
		rgbGen const ( 1 0.55 0.2 )
	}
}

// flat green, so tests can find it in a frame
textures/oax_skyportal/moon
{
	qer_editorimage textures/base_light/light1blue.jpg
	surfaceparm nolightmap
	{
		map $whiteimage
		rgbGen const ( 0.2 1 0.2 )
	}
}
`;

export function build() {
  const map = new MapFile({ message: 'oax test: sky portal' });
  map.brush(room(ARENA.mins, ARENA.maxs, {
    floor: 'base_floor/clang_floor', ceiling: 'oax_skyportal/sky_arena', walls: 'gothic_wall/oct20c',
  }));
  // a pillar, so arena views have foreground against the sky
  map.brush(box([-48, -48, 0], [48, 48, 320], 'gothic_block/blocks15'));

  const [cx, cy, cz] = SKY_ROOM.center, h = SKY_ROOM.half;
  map.brush(room([cx - h, cy - h, cz - h], [cx + h, cy + h, cz + h], 'oax_skyportal/sky_room'));
  // the static moon, 50 degrees up toward yaw 45 (MOON_DIR), 300 units out
  map.brush(box([cx + 96, cy + 96, cz + 190], [cx + 176, cy + 176, cz + 270], 'oax_skyportal/moon'));

  map.entity('misc_oax_skyportal', { origin: SKY_ROOM.center, rotate: `0 ${SKY_ROTATE_YAW} 0` });
  map.entity('func_rotating', { origin: SKY_ROOM.center, speed: 40 }, [
    box([cx - 48, cy - 48, cz + 180], [cx + 48, cy + 48, cz + 276], 'oax_skyportal/planet'),
    box([cx + 180, cy - 40, cz - 40], [cx + 260, cy + 40, cz + 40], 'oax_skyportal/planet'),
  ]);

  map.entity('info_player_deathmatch', { origin: [-320, -320, 32], angle: 45 });
  map.entity('info_player_deathmatch', { origin: [320, 320, 32], angle: 225 });
  map.entity('light', { origin: [0, 0, 400], light: 700 });
  map.entity('light', { origin: [-384, -384, 200], light: 400 });
  map.entity('light', { origin: [384, 384, 200], light: 400 });
  return {
    map,
    manifest: { features: ['skyportal'] },
    files: {
      'scripts/oax_skyportal.shader': shaders,
      'scripts/shaderlist.txt': shaderList('oax_skyportal'),
    },
  };
}
