// oax_ue1_calib_curve: the control for oax_ue1_calib. The same rooms and
// lamps lit by the ue1 profile as it was fitted to the reference display-curve shots
// (a line to 0.89 of the radius, a ceiling at 115 level / brightness of the
// peak, gain 0.02778), written as oax_* overrides. It must fail the linear
// calibration that oax_ue1_calib passes.

import { buildCalib } from './oax_ue1_calib.mjs';
import { ue1Level } from '../../romdev/lib/lightoracle.mjs';

export const CURVE_GAIN = 0.02778;

export function build() {
  return buildCalib('oax_ue1_calib_curve', (l) => {
    const B = l.props.LightBrightness ?? 64;
    return { oax_falloff: 'table 0 1 0.89 0', oax_intensity: CURVE_GAIN * B, oax_cap: 115 * CURVE_GAIN * ue1Level(B) };
  });
}
