// oax_script: map scripting (the id Tech 4 script VM in the engine, driven
// by the oax game module). Everything here runs without player input, so a
// run's script event log is the same on every build:
//
// - a trigger_oax_timer fires the button 2 s in; the button's "call" runs
//   door_cycle(), which moves the door with moveTo / waitFor / wait, turns
//   the spinner with rotateOnce, and triggers the counter each cycle;
// - a trigger_oax_count (count 3) calls count_done(), which hides the
//   pillar and fires a target_oax_setkeyval and a target_oax_shaderparm;
// - main() spawns an entity, starts the lift and a thread that waits for
//   it with sys.waitFor, and 3 s in starts a deliberate runaway thread,
//   which the engine kills at its instruction limit.
//
// The control input: a trigger_multiple near the spawn point calls
// player_entered(); a test that walks the player into it gets a different
// log.

import { MapFile, room, box } from '../mapwriter.mjs';

const around = (c, h) => [[c[0] - h[0], c[1] - h[1], c[2] - h[2]], [c[0] + h[0], c[1] + h[1], c[2] + h[2]]];

// the player trigger (tests walk into it); exported for the test
export const PLAYER_TRIGGER = { center: [-256, -320, 48], half: [48, 48, 48] };
export const SPAWN = [-512, 0, 24];

const SCRIPT = `/*
maps/oax_script.script: the oax_script test map's script (written for oax).
*/

float doorCycles;

void count_done() {
	sys.println( "oax_script: counter reached " + doorCycles );
	$pillar.hide();
	sys.trigger( $setter );
	sys.trigger( $tint );
}

void door_cycle() {
	float i;

	$door.time( 1 );
	$door.accelTime( 0.25 );
	$door.decelTime( 0.25 );
	for ( i = 0; i < 3; i++ ) {
		$door.moveTo( $door_open );
		sys.waitFor( $door );
		sys.wait( 0.5 );
		$door.moveToPos( '0 0 0' );
		sys.waitFor( $door );
		$spinner.time( 0.5 );
		$spinner.rotateOnce( '0 90 0' );
		sys.waitFor( $spinner );
		doorCycles = doorCycles + 1;
		sys.trigger( $counter );
	}
}

void watch_lift() {
	sys.waitFor( $lift );
	$marker.setOrigin( $lift.getOrigin() + '0 0 64' );
}

void player_entered() {
	$spinner.time( 0.25 );
	$spinner.rotateOnce( '0 45 0' );
}

void runaway() {
	float n;

	while ( 1 ) {
		n = n + 1;
	}
}

void main() {
	entity e;

	sys.setSpawnArg( "targetname", "spawned_marker" );
	sys.setSpawnArg( "origin", "0 -256 64" );
	e = sys.spawn( "target_position" );
	if ( e != $null_entity ) {
		sys.println( "oax_script: spawned " + e.getName() );
	}
	$lift.time( 2 );
	$lift.moveToPos( '0 0 48' );
	thread watch_lift();
	sys.wait( 3 );
	thread runaway();
}
`;

export function build() {
  const map = new MapFile({ message: 'oax test: script' });
  map.brush(room([-768, -512, 0], [768, 512, 320], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));

  // the door: a slab across the middle that rises
  map.entity('func_oax_mover', { targetname: 'door' }, [
    box([256, -96, 0], [272, 96, 128], 'base_wall/metalfloor_wall_10'),
  ]);
  map.entity('target_position', { targetname: 'door_open', origin: [0, 0, 120] });

  // the spinner turns about its origin brush
  const sc = [0, 256, 64];
  map.entity('func_oax_mover', { targetname: 'spinner' }, [
    box(...around(sc, [64, 16, 16]), 'base_wall/metalfloor_wall_10'),
    box(...around(sc, [8, 8, 8]), 'common/origin'),
  ]);

  // the button, fired by a timer; its "call" runs door_cycle()
  map.entity('func_button', { targetname: 'button', call: 'door_cycle', angle: -2, lip: 4, wait: -1, speed: 200 }, [
    box([-128, 496, 64], [-96, 512, 96], 'base_wall/metalfloor_wall_10'),
  ]);
  map.entity('trigger_oax_timer', { targetname: 'autobutton', target: 'button', start_on: 1, delay: 2, wait: -1, random: 0 });

  // the counter: three door cycles hide the pillar
  map.entity('trigger_oax_count', { targetname: 'counter', count: 3, call: 'count_done' });
  map.entity('func_static', { targetname: 'pillar' }, [
    box([-416, 288, 0], [-352, 352, 192], 'gothic_block/blocks18c_3'),
  ]);

  map.entity('target_position', { targetname: 'marker', origin: [0, -128, 64] });
  // fired by count_done(): D3 idTarget_SetKeyVal / idTarget_SetShaderParm
  map.entity('target_position', { targetname: 'marker2', origin: [0, 128, 64] });
  map.entity('target_oax_setkeyval', { targetname: 'setter', target: 'marker2', keyval1: 'origin;0 -128 128' });
  map.entity('target_oax_shaderparm', { targetname: 'tint', target: 'pillar', shaderParm0: 0.5 });
  map.entity('func_oax_mover', { targetname: 'lift' }, [
    box([384, -320, 0], [512, -192, 16], 'base_floor/diamond2c'),
  ]);

  // the control input
  const t = PLAYER_TRIGGER;
  map.entity('trigger_multiple', { call: 'player_entered', wait: 1 }, [
    box(...around(t.center, t.half), 'common/trigger'),
  ]);

  map.entity('info_player_deathmatch', { origin: SPAWN, angle: 0 });
  map.entity('light', { origin: [0, 0, 280], light: 1200 });
  map.entity('light', { origin: [-512, -256, 200], light: 800 });
  map.entity('light', { origin: [512, 256, 200], light: 800 });
  return { map, manifest: { features: ['script', 'movers'] }, files: { 'maps/oax_script.script': SCRIPT } };
}
