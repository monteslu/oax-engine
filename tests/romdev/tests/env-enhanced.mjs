// The enhanced content's environment keys (renderergl2 tr_oax_env.c), set by a
// map sidecar on a stock-style map: oax_sundisc draws the sun in the sky,
// oax_bloom turns bloom on while r_oaxBloom is 2 (the default).
//
// oax_skyenv: a room with a sky ceiling (q3map_sun azimuth 90, elevation 70)
// and a sidecar with oax_sundisc 0.12 and oax_bloom 1.
//   - enhanced (default): looking along the sun the frame holds the sun disc
//     (more than a few pixels differ from the environment-off frame with
//     bloom out of the way), and the bloom chain runs (r_bloom_passes >= 3);
//   - com_oaxEnhanced 0: no disc, no bloom passes (the map as it came);
//   - r_oaxBloom 0 turns the bloom passes off with the sidecar on (the
//     switch works; the control for "passes only run when asked");
//   - r_oaxEnv 0 removes the sun disc (the other switch);
//   - native and cart agree on the disc.

import { shootCart, shootNative, readPng } from '../lib/ulight.mjs';
import { comparePng, halfSize } from '../lib/png.mjs';
import { diffFraction } from '../lib/imgstat.mjs';

export const name = 'env-enhanced';

const MAP = 'oax_skyenv';
const VIEW = '0 0 64 -70 90 0';
const PLAYER = '-384 -384 32 45';

export async function run({ out }) {
  const failures = [], rows = [];
  const on = await shootCart(MAP, [VIEW], { name: 'env-on', out, player: PLAYER, pre: 'r_autoExposure 0' });
  const off = await shootCart(MAP, [VIEW], { name: 'env-off', out, player: PLAYER, pre: 'r_autoExposure 0', preload: 'set com_oaxEnhanced 0' });
  const nobloom = await shootCart(MAP, [VIEW], { name: 'env-nobloom', out, player: PLAYER, pre: 'r_autoExposure 0;r_oaxBloom 0' });
  const noenv = await shootCart(MAP, [VIEW], { name: 'env-noenv', out, player: PLAYER, pre: 'r_autoExposure 0;r_oaxEnv 0' });
  const nat = shootNative(MAP, [VIEW], { name: 'env-native', out, player: PLAYER, pre: 'r_autoExposure 0' });
  const nat0 = shootNative(MAP, [VIEW], { name: 'env-native-nobloom', out, player: PLAYER, pre: 'r_autoExposure 0;r_oaxBloom 0' });
  const a = readPng(on.files[0]), b = readPng(off.files[0]), c = readPng(noenv.files[0]), d = readPng(nobloom.files[0]);
  // the disc alone: bloom off with the sidecar on, against the environment off
  const disc = diffFraction(d, c, 24);
  const control = diffFraction(c, b, 24);       // environment off against sidecar off: the same frame
  const bloomed = diffFraction(a, d, 24);
  rows.push(`pixels over 24: disc (bloom off, sidecar on vs environment off) ${(disc * 100).toFixed(3)}%; environment off vs sidecar off ${(control * 100).toFixed(3)}%; bloom (on vs off) ${(bloomed * 100).toFixed(3)}%`);
  rows.push(`bloom passes: enhanced ${on.values.r_bloom_passes}, sidecar off ${off.values.r_bloom_passes}, r_oaxBloom 0 ${nobloom.values.r_bloom_passes}`);
  if (disc < 0.0003) failures.push('the sun disc is not in the frame');
  if (control > 0.0005) failures.push('control: with the environment off the frame differs from the sidecar-off one');
  if (bloomed < 0.002) failures.push('bloom changed nothing in the frame');
  if (!(Number(on.values.r_bloom_passes) >= 3)) failures.push(`oax_bloom did not run the bloom chain (${on.values.r_bloom_passes})`);
  if (Number(off.values.r_bloom_passes) !== 0) failures.push(`bloom runs without the sidecar (${off.values.r_bloom_passes})`);
  if (Number(nobloom.values.r_bloom_passes) !== 0) failures.push(`r_oaxBloom 0 still runs bloom (${nobloom.values.r_bloom_passes})`);
  // the disc frame (bloom off) must match; the bloomed frame is compared by fx-bloom's parity,
  // here it only has to agree where it matters (few pixels far off)
  const n0 = comparePng(halfSize(readPng(nat0.files[0])), halfSize(d), { tolerance: 32 });
  rows.push(`cart vs native, disc frame: mean ${n0.meanDiff.toFixed(2)}, ${(n0.badFraction * 100).toFixed(2)}% beyond 32`);
  if (n0.meanDiff > 4 || n0.badFraction > 0.03) failures.push('cart differs from native (disc frame)');
  const n = comparePng(halfSize(readPng(nat.files[0])), halfSize(a), { tolerance: 32 });
  rows.push(`cart vs native, bloomed frame: mean ${n.meanDiff.toFixed(2)}, ${(n.badFraction * 100).toFixed(2)}% beyond 32`);
  if (n.badFraction > 0.03) failures.push('cart differs from native (bloomed frame)');
  return { ok: failures.length === 0, failures, rows };
}
