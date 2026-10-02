// oax_lightstyle: switched and animated lights (light styles).
//
// Two sealed rooms. Room A is long: a switched light ("switch1", style 32
// once q3map2 numbers the targeted lights in entity order) lights its far
// end, an unstyled fill light its near end, so one view shows both the
// region the switch changes and a region it must leave alone. A
// func_button and a one-shot trigger_multiple toggle the light.
// Room B has a flickering light ("flick", style 33, preset 1). q3map2
// compiles each targeted light into its own lightmap stage; the worldspawn
// keys _styleNrgbgen "lightstyle N" make those stages follow the styles the
// cgame sets, so q3map2 needs no patch.

import { MapFile, room, box } from '../mapwriter.mjs';

export const ROOM_A = { mins: [-1536, -320, 0], maxs: [-64, 320, 256] };
export const ROOM_B = { mins: [64, -320, 0], maxs: [768, 320, 256] };
export const SWITCH_LIGHT = [-1380, 0, 96];
export const FLICKER_LIGHT = [560, 0, 96];
export const TRIGGER = { mins: [-320, -300, 0], maxs: [-224, -204, 96] };
export const FLICKER_PATTERN = 'mmnmmommommnonmmonqnmmo';

export function build() {
  const world = { message: 'oax test: light styles' };
  for (let s = 32; s < 64; s++) world[`_style${s}rgbgen`] = `lightstyle ${s}`;
  const map = new MapFile(world);
  const tex = { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' };
  map.brush(room(ROOM_A.mins, ROOM_A.maxs, tex));
  map.brush(room(ROOM_B.mins, ROOM_B.maxs, tex));

  // the switched light (room A), then the flicker (room B): entity order
  // gives them styles 32 and 33
  map.entity('light', { origin: SWITCH_LIGHT, light: 700, targetname: 'switch1', _color: '1 0.85 0.6' });
  map.entity('light', { origin: FLICKER_LIGHT, light: 450, targetname: 'flick', lightstyle_preset: 1, _color: '0.6 0.8 1' });
  map.entity('light', { origin: [-300, 0, 200], light: 260 });
  map.entity('light', { origin: [416, 0, 240], light: 120 });

  map.entity('func_button', { angle: 180, lip: 4, wait: 1, target: 'switch1' },
    [box([-1536, 180, 48], [-1520, 244, 112], 'base_wall/basewall01')]);
  map.entity('trigger_multiple', { target: 'switch1', wait: -1 }, [box(TRIGGER.mins, TRIGGER.maxs, 'common/trigger')]);

  map.entity('info_player_deathmatch', { origin: [-200, 200, 32], angle: 180 });
  map.entity('info_player_deathmatch', { origin: [200, 200, 32], angle: 0 });
  return { map, manifest: { features: ['lightstyle'] } };
}
