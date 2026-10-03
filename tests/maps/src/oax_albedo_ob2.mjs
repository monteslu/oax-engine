// oax_albedo_ob2: oax_albedo with oax_overbright 2 (see oax_albedo.mjs).
import { buildAlbedo } from './oax_albedo.mjs';

export function build() {
  return buildAlbedo({ overbright: 2 });
}
