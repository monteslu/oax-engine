// oax_showcase_nohull: oax_showcase with the arena's hull floor and solids
// 512 units lower (control: the player falls through the surfaces and
// validation reports them floating). It is a loose map, not in a package of
// its own, so it shows the later package's copy of the probe texture.
import { build as showcase } from './oax_showcase.mjs';

export function build() {
  return showcase({ variant: 'nohull' });
}
