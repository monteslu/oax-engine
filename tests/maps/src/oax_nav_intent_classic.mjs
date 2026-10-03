// oax_nav_intent_classic: oax_nav_intent with classic teleporters (no
// "noretrigger"). Each arrival sits inside the partner trigger, so a player
// ping-pongs between them: the control for the noretrigger key, and the
// teleporter links are left out of the navmesh (an arrival in a classic
// trigger would re-trigger).

import { buildNavIntent } from './oax_nav_intent.mjs';

export function build() {
  return { map: buildNavIntent({ noretrigger: false, message: 'oax test: navigation from intent (classic teleporters)' }), aas: false, manifest: { features: ['nav_intent'] } };
}
