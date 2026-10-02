// oax_scripted: the phase 4 exit map. Movers, triggers and a GUI working
// together through a map script, which must play identically on the native
// build and the cart (tests/romdev/tests/scripted-identity.mjs).
//
// The oax_gui room and door panel, plus:
// - lift: a func_oax_mover the script drives (moveToPos with accel/decel),
// - counter: a trigger_oax_count the script fires twice; at its count it
//   calls count_done(), which hides the marker,
// - marker: a func_static the counter hides.
// The panel's LOCK button runs oax_gui_lock(): the lift goes up, waits,
// comes down, and fires the counter after each leg. OPEN still opens the
// stock func_door through the panel's target.

import { box } from '../mapwriter.mjs';
import { build as buildGui } from './oax_gui.mjs';

export const LIFT = { origin: [300, 150, 16], up: [300, 150, 112] };

const SCRIPT = `/*
 * oax_scripted.script: the phase 4 exit map (tests/maps/src/oax_scripted.mjs)
 */

void count_done() {
	sys.println( "oax_scripted: counter reached" );
	$marker.hide();
}

void oax_gui_lock() {
	$lift.time( 1 );
	$lift.accelTime( 0.25 );
	$lift.decelTime( 0.25 );
	$lift.moveToPos( '${LIFT.up.join(' ')}' );
	sys.waitFor( $lift );
	sys.trigger( $counter );
	sys.wait( 0.5 );
	$lift.moveToPos( '${LIFT.origin.join(' ')}' );
	sys.waitFor( $lift );
	sys.trigger( $counter );
}

void main() {
}
`;

const around = (o, h) => [[o[0] - h[0], o[1] - h[1], o[2] - h[2]], [o[0] + h[0], o[1] + h[1], o[2] + h[2]]];

export function build() {
  const spec = buildGui();
  spec.map.world.keys.message = 'oax test: scripted movers, triggers and a GUI';
  spec.map.entity('func_oax_mover', { targetname: 'lift', mode: 'trigger_control' }, [
    box(...around(LIFT.origin, [48, 48, 8]), 'base_wall/metalfloor_wall_10'),
    box(...around(LIFT.origin, [8, 8, 8]), 'common/origin'),
  ]);
  spec.map.entity('trigger_oax_count', { targetname: 'counter', count: 2, call: 'count_done' });
  spec.map.entity('func_static', { targetname: 'marker' }, [box([380, -150, 0], [412, -118, 64], 'base_wall/basewall01')]);
  spec.manifest = { features: ['gui', 'script', 'movers'] };
  spec.files['maps/oax_scripted.script'] = SCRIPT;
  return spec;
}
