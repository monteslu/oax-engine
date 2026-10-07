// oax_smooth_flat: oax_smooth without oax_smoothnormals (the control).
import { build as buildSmooth } from './oax_smooth.mjs';

export function build() {
  return buildSmooth({ smooth: false });
}
