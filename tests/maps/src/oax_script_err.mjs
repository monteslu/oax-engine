// oax_script_err: a map whose script does not compile. The game must go on
// without scripting and report the error (file, line, message).

import { MapFile, room } from '../mapwriter.mjs';

const SCRIPT = `/*
maps/oax_script_err.script: a compile error on line 6 (written for oax).
*/
void main() {
	float x;
	x = 1 + ;
}
`;

export function build() {
  const map = new MapFile({ message: 'oax test: script error' });
  map.brush(room([-512, -512, 0], [512, 512, 256], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  map.entity('info_player_deathmatch', { origin: [-256, 0, 32], angle: 0 });
  map.entity('light', { origin: [0, 0, 200], light: 600 });
  return { map, manifest: { features: ['script'] }, files: { 'maps/oax_script_err.script': SCRIPT } };
}
