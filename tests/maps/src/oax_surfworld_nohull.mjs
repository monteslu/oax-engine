// oax_surfworld_nohull: oax_surfworld's surfaces with the hull floor 512
// units lower and no platform or ramp solids (control: the player falls
// through the surfaces; validation reports them floating), plus one
// surface buried in the lowered floor (validation reports it buried).
import { build as surfworld } from './oax_surfworld.mjs';

export function build() {
  return surfworld({ variant: 'nohull' });
}
