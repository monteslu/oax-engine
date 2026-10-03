// oax_showcase_classic: oax_showcase with classic teleporters (no
// noretrigger): the control that must ping-pong. A loose map.
import { build as showcase } from './oax_showcase.mjs';

export function build() {
  return showcase({ variant: 'classic' });
}
