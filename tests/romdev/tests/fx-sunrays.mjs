// Sun shafts (the stock r_drawSunRays, latched; tr_postprocess.c RB_SunRays): with
// the sun in view a radial blur of the sun flare is added over the frame.
//
// islandctf (stock map; its sky's sun is at azimuth 65, elevation 45), the player
// placed exactly at a spawn with `setviewpos`, native client started with
// r_drawSunRays 1 (the cart cannot restart its renderer, so native only):
//   - looking at the sun the frame has shafts: over 1% of pixels differ from the
//     plain frame by more than 12, and it is not dark (mean luma over half the plain one's);
//   - looking away the shafts pass does nothing (the frames match);
//   - control: two plain runs of the same view match each other (the comparison reads 0 when
//     nothing changed, so a non-zero above is the shafts).
// The player is placed because the world is culled by the player's area (areamask): a camera
// elsewhere can see an empty world and read as black.

import { nativeShots } from '../lib/nativeshot.mjs';
import { diffFraction, meanLuma } from '../lib/imgstat.mjs';
import { isPicture } from '../lib/fxtest.mjs';

export const name = 'fx-sunrays';

const MAP = 'islandctf';
const SETUP = ['cg_drawGun 0', 'r_autoExposure 0', 'r_fixedShaderTime 5'];
// the player is placed (area culling) and the view pinned to the same eye, as oacontent's tour does
const at = (yaw, pitch) => `setviewpos -528 608 472 ${yaw} ${pitch} 0;cl_overrideView "-528 608 498 ${pitch} ${yaw} 0"`;
const shots = () => [
  { cmd: at(65, -38), settle: 150, name: 'sun' },
  { cmd: at(245, 0), settle: 150, name: 'away' },
];

export async function run() {
  const ctx = { rows: [], failures: [] };
  const plain = nativeShots('fx-sunrays-a', MAP, shots(), { setup: SETUP });
  const plain2 = nativeShots('fx-sunrays-b', MAP, shots(), { setup: SETUP });
  const rays = nativeShots('fx-sunrays-on', MAP, shots(), { setup: SETUP, startArgs: ['+set', 'r_drawSunRays', '1'] });
  isPicture(ctx, 'plain sun view', plain.images.sun);
  const ml0 = meanLuma(plain.images.sun), ml1 = meanLuma(rays.images.sun);
  const shafts = diffFraction(plain.images.sun, rays.images.sun, 12);
  const away = diffFraction(plain.images.away, rays.images.away, 8);
  const control = diffFraction(plain.images.sun, plain2.images.sun, 12);
  ctx.rows.push(`sun view: mean luma ${ml0.toFixed(1)} plain, ${ml1.toFixed(1)} with shafts; ${(shafts * 100).toFixed(2)}% of pixels differ beyond 12; away ${(away * 100).toFixed(3)}%; control (two plain runs) ${(control * 100).toFixed(3)}%`);
  if (!(ml1 > ml0 * 0.5)) ctx.failures.push(`the frame with shafts is dark (${ml1.toFixed(1)} against ${ml0.toFixed(1)})`);
  if (shafts < 0.01) ctx.failures.push('the shafts added nothing visible');
  if (away > 0.002) ctx.failures.push(`looking away the shafts pass changed the frame (${(away * 100).toFixed(3)}%)`);
  if (control > 0.002) ctx.failures.push(`control: two plain runs differ (${(control * 100).toFixed(3)}%), so the comparison is not stable`);
  return { ok: ctx.failures.length === 0, failures: ctx.failures, rows: ctx.rows };
}
