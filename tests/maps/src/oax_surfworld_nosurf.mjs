// oax_surfworld_nosurf: oax_surfworld's hull without its surface world
// (control: solid but invisible; only the brush pillar is drawn).
import { build as surfworld } from './oax_surfworld.mjs';

export function build() {
  return surfworld({ variant: 'nosurf' });
}
