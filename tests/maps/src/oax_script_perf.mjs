// oax_script_perf: 50 script threads that each call one engine event and
// one game event every frame (the design's script cost check). The same
// room as oax_box.

import { MapFile, room } from '../mapwriter.mjs';

export const THREADS = 50;

const SCRIPT = `/*
maps/oax_script_perf.script: 50 busy threads (written for oax).
*/

float total;

void worker() {
	float i;
	vector v;

	while ( 1 ) {
		v = $probe.getOrigin();
		total = total + sys.sin( i * 10 ) + v_x;
		i = i + 1;
		sys.waitFrame();
	}
}

void main() {
	float i;

	for ( i = 0; i < ${THREADS}; i++ ) {
		thread worker();
	}
}
`;

export function build() {
  const map = new MapFile({ message: 'oax test: script cost' });
  map.brush(room([-512, -512, 0], [512, 512, 256], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));
  map.entity('info_player_deathmatch', { origin: [-256, 0, 32], angle: 0 });
  map.entity('target_position', { targetname: 'probe', origin: [0, 0, 64] });
  map.entity('light', { origin: [0, 0, 200], light: 600 });
  return { map, manifest: { features: ['script'] }, files: { 'maps/oax_script_perf.script': SCRIPT } };
}
