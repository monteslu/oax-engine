// oax_gui: in-world GUIs. A door-control panel (func_oax_gui) on the west
// wall shows guis/oax_door_panel.gui: a title, a state label and two
// buttons. OPEN sets gui::state 1 and activates the panel's target, a
// func_door across the room; LOCK sets gui::state 2 and runs a script.
//
// Panel geometry the tests rely on (tests/romdev/tests/gui.mjs):
//   GUI face: x = -504, facing +x; y -64..64 (s 0..1), z 8..104 (t 0..1)
//   a player at x -460 facing west is 44 units away (focus is 64)
//   OPEN center: s 0.28125 -> y -28; LOCK center: s 0.71875 -> y 28
//   both at t 0.5625 -> z 50, the eye height of a player on the floor

import { MapFile, Brush, Face, box, room } from '../mapwriter.mjs';

export const PANEL = { x: -504, y0: -64, y1: 64, z0: 8, z1: 104 };

// The face's texture projection must put the GUI's 0..1 square exactly on
// the panel (q3map2: s = (y / sx + xoff) / w, t = (-z / sy + yoff) / h for
// a face along +x). The editor image is gfx/2d/bigchars.tga, 256 x 256.
function panelBrush() {
  const W = 256;
  const width = PANEL.y1 - PANEL.y0;
  const height = PANEL.z1 - PANEL.z0;
  const sx = width / W;
  const sy = height / W;
  const mod = (a, m) => ((a % m) + m) % m;
  const gui = {
    xoff: mod(-PANEL.y0 / sx, W),
    yoff: mod(PANEL.z1 / sy, W),
    rot: 0, sx, sy,
  };
  const trim = 'base_trim/dark_tin2';
  return new Brush([
    new Face([1, 0, 0], PANEL.x, 'oax/gui_panel', gui),
    new Face([-1, 0, 0], 512, 'common/caulk'),
    new Face([0, 1, 0], PANEL.y1, trim),
    new Face([0, -1, 0], -PANEL.y0, trim),
    new Face([0, 0, 1], PANEL.z1, trim),
    new Face([0, 0, -1], -PANEL.z0, trim),
  ]).check();
}

const SHADER = `// oax_gui test map shaders
textures/oax/gui_panel
{
	qer_editorimage gfx/2d/bigchars.tga
	surfaceparm gui
	surfaceparm nolightmap
	surfaceparm nomarks
	{
		map $gui
		rgbGen identity
	}
}
`;

// q3map2 custom surface parameters (-custinfoparms): the engine's
// SURF_OAX_GUI bit (code/qcommon/oax.h)
const CUSTINFOPARMS = `// Custom Contentsflags
{
}
// Custom Surfaceflags
{
	gui 0x01000000
}
`;

const GUI = `// oax_door_panel.gui: the oax_gui test map's door panel (DOOM-3 .gui syntax)
#define PANEL_GREEN		0.10, 0.40, 0.15, 1
#define PANEL_RED		0.50, 0.10, 0.10, 1

windowDef Desktop
{
	rect		0, 0, 640, 480
	backcolor	0.04, 0.06, 0.10, 1
	forecolor	1, 1, 1, 1

	windowDef frame
	{
		rect		8, 8, 624, 464
		bordersize	2
		bordercolor	0.3, 0.5, 0.8, 1
		noevents	1
	}
	windowDef title
	{
		rect		20, 24, 600, 56
		text		"DOOR CONTROL"
		textscale	0.7
		textalign	1
		forecolor	1, 0.78, 0.2, 1
		noevents	1
	}
	windowDef stateLabel
	{
		rect		20, 100, 600, 56
		text		"gui::status"
		textscale	0.55
		textalign	1
		forecolor	0.6, 1, 0.6, 1
		noevents	1
	}
	windowDef btnOpen
	{
		rect		60, 200, 240, 140
		backcolor	PANEL_GREEN
		bordersize	3
		bordercolor	1, 1, 1, 1
		text		"OPEN"
		textscale	0.6
		textalign	1
		textaligny	40
		onMouseEnter { set "backcolor" "0.16 0.62 0.24 1"; }
		onMouseExit { set "backcolor" "0.10 0.40 0.15 1"; }
		onAction {
			set "gui::state" "1";
			set "gui::status" "OPENING";
			set "cmd" "activate";
		}
	}
	windowDef btnLock
	{
		rect		340, 200, 240, 140
		backcolor	PANEL_RED
		bordersize	3
		bordercolor	1, 1, 1, 1
		text		"LOCK"
		textscale	0.6
		textalign	1
		textaligny	40
		onMouseEnter { set "backcolor" "0.75 0.16 0.16 1"; }
		onMouseExit { set "backcolor" "0.50 0.10 0.10 1"; }
		onAction {
			set "gui::state" "2";
			set "gui::status" "LOCKED";
			set "cmd" "runScript oax_gui_lock";
		}
	}
	windowDef footer
	{
		rect		20, 400, 600, 40
		text		"AIM AND FIRE TO PRESS"
		textscale	0.3
		textalign	1
		forecolor	0.6, 0.7, 0.9, 1
		noevents	1
	}
}
`;

export function build() {
  const map = new MapFile({ message: 'oax test: in-world GUI' });
  map.brush(room([-512, -256, 0], [512, 256, 192], { floor: 'base_floor/clang_floor', ceiling: 'base_wall/basewall01', walls: 'base_wall/basewall01' }));
  map.entity('func_oax_gui', {
    gui: 'guis/oax_door_panel.gui',
    'gui::status': 'CLOSED',
    'gui::state': '0',
    target: 'door1',
  }, [panelBrush()]);
  map.entity('func_door', { targetname: 'door1', angle: -1, speed: 100, wait: 5, lip: 8 },
    [box([100, -64, 0], [116, 64, 128], 'base_door/shinymetaldoor')]);
  // spawns face away from the panel: nobody is focused until a test aims
  map.entity('info_player_deathmatch', { origin: [-200, 150, 24], angle: 0 });
  map.entity('info_player_deathmatch', { origin: [300, 0, 24], angle: 180 });
  map.entity('light', { origin: [-300, 0, 160], light: 500 });
  map.entity('light', { origin: [200, 0, 160], light: 500 });
  return {
    map,
    bspArgs: ['-custinfoparms'],
    lightArgs: ['-custinfoparms'],
    manifest: { features: ['gui'] },
    files: {
      'scripts/oax_gui.shader': SHADER,
      'scripts/custinfoparms.txt': CUSTINFOPARMS,
      'guis/oax_door_panel.gui': GUI,
    },
  };
}
