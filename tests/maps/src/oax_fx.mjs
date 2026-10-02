// oax_fx: the phase 6 effects (renderergl2 tr_oax_fx*.c, gamecode
// cg_oax_fx.c / g_oax_fx.c). One room, x from -1024 to 1024:
//
// - pool (x < -320): a basin 96 deep with a water surface 16 under the
//   floor (shader oax_fx/water: the oaxWater keyword, fallback stages), a
//   pillar standing in it and a bright red panel on the wall behind it, so
//   the water has something to reflect and something to refract;
// - emitters (x -200..300): func_oax_emitter entities: a spark fountain
//   (aimed sparks), a smoke pile sitting on the floor and a block (soft
//   particles), a glow helix (custom path);
// - bright panel on the -y wall (x 0): a fullbright panel at twice white in
//   scene light, for bloom;
// - decal wall (x > 400): the room's +x wall, a block for corner decals, and
//   a horizontally bobbing brush model (decals ride it) that carries an
//   `oaxtrail` key (a ribbon behind any entity that asks for one).
//
// Particle decls of the emitters are in particles/oax_fx_test.prt (written
// with the map); the weapon decls ship with the QVMs.

import { MapFile, room, box } from '../mapwriter.mjs';

export const POOL = { mins: [-900, -360, -96], maxs: [-420, 360, -16] };
export const BOBBER = { mins: [640, -64, 96], maxs: [672, 64, 224], height: 96, speed: 4 };
export const EMITTERS = {
  fountain: [-100, 250, 8],
  smoke: [60, -60, 0],
  helix: [200, 250, 40],
};
export const BRIGHT = { mins: [-48, -640, 96], maxs: [48, -632, 192] };

const shaders = `// oax_fx test map shaders (tests/maps/src/oax_fx.mjs)
textures/oax_fx/water
{
	qer_editorimage textures/liquids/pool2.jpg
	surfaceparm nomarks
	surfaceparm trans
	surfaceparm nonsolid
	surfaceparm water
	surfaceparm nolightmap
	oaxWater
	oaxWaterParm tint 0.02 0.16 0.18
	oaxWaterParm density 0.006
	oaxWaterParm scale 0.008
	oaxWaterParm speed 0.05 0.035
	oaxWaterParm distortion 0.02
	oaxWaterParm reflectivity 0.9
	oaxWaterParm fresnel 0.06
	{
		map textures/liquids/pool2.jpg
		blendfunc filter
		tcMod scroll 0.05 0.05
	}
	{
		map textures/liquids/pool3d_3e.jpg
		blendfunc add
		rgbGen const ( 0.4 0.4 0.4 )
		tcMod scroll -0.03 -0.06
	}
}

textures/oax_fx/red
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm nolightmap
	{
		map $whiteimage
		rgbGen const ( 0.9 0.15 0.1 )
	}
}

textures/oax_fx/spot
{
	polygonOffset
	{
		map *oaxglow
		blendFunc GL_ONE GL_ONE
		rgbGen vertex
	}
}

textures/oax_fx/bright
{
	qer_editorimage textures/base_wall/basewall01.jpg
	surfaceparm nolightmap
	{
		map $whiteimage
		rgbGen const ( 1 0.95 0.8 )
	}
	{
		map $whiteimage
		blendfunc add
		rgbGen const ( 1 0.95 0.8 )
	}
}
`;

const decls = `// oax_fx test map particle decls (tests/maps/src/oax_fx.mjs)

particle oax_fx/fountain {
	{
		count				80
		material			oaxfx/spark
		time				1.2
		bunching			1
		distribution		sphere 4 4 2
		direction			cone 22
		orientation			aimed 0 0.05
		speed				"300" to "150"
		size				"1.4" to "0.8"
		gravity				world 330
		fadeIn				0.05
		fadeOut				0.4
		color				1 0.8 0.4 1
		fadeColor			0 0 0 0
		softDistance		-1
	}
}

particle oax_fx/smoke {
	{
		count				24
		material			oaxfx/smoke
		time				3
		bunching			1
		distribution		cylinder 56 56 4
		direction			cone 30
		speed				"10" to "4"
		size				"30" to "46"
		rotation			"6" to "12"
		gravity				world -4
		fadeIn				0.2
		fadeOut				0.4
		color				0.7 0.7 0.75 0.85
		fadeColor			0.7 0.7 0.75 0
		softDistance		32
	}
}

particle oax_fx/helix {
	{
		count				40
		material			oaxfx/glow
		time				2
		bunching			1
		customPath			helix 24 24 8 3 40
		size				"5" to "3"
		gravity				0
		fadeIn				0.1
		fadeOut				0.3
		color				0.3 0.7 1 1
		fadeColor			0 0 0 0
	}
}
`;

// 300 decals on the floor of the decal zone (more than the renderer's cap of 256)
export const CAP_DECALS = 300;
function capCfg() {
  const lines = [];
  for (let i = 0; i < CAP_DECALS; i++) {
    const x = 450 + (i % 20) * 24, y = 200 + Math.floor(i / 20) * 24;
    lines.push(`oaxdecal textures/oax_fx/spot ${x} ${y} 1 0 0 1 10`);
  }
  return lines.join('\n') + '\n';
}

export function build() {
  const map = new MapFile({ message: 'oax test: effects' });
  const tex = { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' };
  const [x0, y0] = [-1024, -640], [x1, y1] = [1024, 640];

  // the room without its floor; the floor goes around the pool
  map.brush(room([x0, y0, 0], [x1, y1, 384], tex).slice(1));
  const P = POOL;
  const floor = (a, b) => box([a[0], a[1], -112], [b[0], b[1], 0], { top: tex.floor, sides: 'base_wall/basewall01', bottom: 'common/caulk' });
  map.brush(floor([x0 - 16, y0 - 16], [P.mins[0], y1 + 16]));
  map.brush(floor([P.maxs[0], y0 - 16], [x1 + 16, y1 + 16]));
  map.brush(floor([P.mins[0], y0 - 16], [P.maxs[0], P.mins[1]]));
  map.brush(floor([P.mins[0], P.maxs[1]], [P.maxs[0], y1 + 16]));
  // pool bottom, a pillar in the water, the water
  map.brush(box([P.mins[0], P.mins[1], -128], [P.maxs[0], P.maxs[1], P.mins[2]], { top: 'base_floor/diamond2c', sides: 'common/caulk' }));
  map.brush(box([-700, -60, P.mins[2]], [-620, 20, 96], 'base_wall/basewall01'));
  map.brush(box(P.mins, P.maxs, 'oax_fx/water'));
  // a red panel behind the pool, for the reflection
  map.brush(box([x0, -200, 64], [x0 + 8, 200, 256], { px: 'oax_fx/red', sides: 'common/caulk' }));

  // the emitters' block (smoke sits against it) and the bright panel
  map.brush(box([100, -140, 0], [180, -40, 48], 'base_wall/basewall01'));
  map.brush(box(BRIGHT.mins, BRIGHT.maxs, { py: 'oax_fx/bright', sides: 'common/caulk' }));

  // the decal block in the corner zone
  map.brush(box([880, -240, 0], [1024, -96, 112], 'base_wall/basewall01'));

  const E = EMITTERS;
  map.entity('func_oax_emitter', { origin: E.fountain, particle: 'oax_fx/fountain', seed: 7 });
  map.entity('func_oax_emitter', { origin: E.smoke, particle: 'oax_fx/smoke', seed: 11 });
  map.entity('func_oax_emitter', { origin: E.helix, particle: 'oax_fx/helix', seed: 3 });

  // a brush model bobbing along y, with a trail
  const B = BOBBER;
  map.entity('func_bobbing', {
    height: B.height, speed: B.speed, spawnflags: 2,
    oaxtrail: 'oaxfx/trailGlow 24 600 0.3 1 0.4 1',
  }, [box(B.mins, B.maxs, 'base_wall/metalfloor_wall_10')]);

  map.entity('info_player_deathmatch', { origin: [0, -480, 32], angle: 90 });
  map.entity('info_player_deathmatch', { origin: [400, 0, 32], angle: 0 });
  map.entity('light', { origin: [-660, 0, 300], light: 1600 });
  map.entity('light', { origin: [0, 0, 330], light: 1800 });
  map.entity('light', { origin: [700, 0, 330], light: 1800 });
  map.entity('light', { origin: [700, -400, 200], light: 900 });
  map.entity('light', { origin: [-800, 200, -40], light: 400 });
  return {
    map,
    manifest: { features: ['particles', 'decals', 'trails', 'water', 'bloom'] },
    files: {
      'scripts/oax_fx_test.shader': shaders,
      'particles/oax_fx_test.prt': decls,
      'fx_cap.cfg': capCfg(),
    },
  };
}
