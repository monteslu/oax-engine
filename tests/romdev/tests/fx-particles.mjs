// Stateless GPU particles (phase 6): particle decls (particles/*.prt,
// DOOM-3 syntax) evaluated in the vertex shader from a static index buffer,
// drawn for func_oax_emitter entities and for cgame-spawned systems.
//
// Checks, each next to a control that must fail:
// - the oax_fx emitters draw (r_fx_drawn, r_particles_drawn > 0), and the
//   renderer advertises "particles";
// - frozen time renders identically: with r_fixedShaderTime pinned, a frame
//   30 frames later is the same frame (0 differing pixels); control: another
//   pinned time moves the particles;
// - r_oaxParticles 0 removes them (r_particles_drawn 0, the pixels change)
//   and cg_oaxParticles 0 gives the same frame (the cgame stops adding);
// - a one-shot system spawned by the cgame (`oaxfx oax/impact_sparks ...`,
//   the decl the weapon impacts use) draws, then ends by itself: the cgame
//   frees it (cg_fx_live back to 0) and the frame is the emitter frame again;
// - native draws the same particles as the cart (frozen time); control: the
//   cart frame without particles;
// - a cart golden; control: the frame without particles.

import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { diffFraction } from '../lib/imgstat.mjs';
import { MAP, SETUP, golden, goldenControl, nativeMatchesCart, isPicture, num } from '../lib/fxtest.mjs';

export const name = 'fx-particles';

const CAM = 'cl_overrideView "20 -330 90 4 90 0"';

function shotList() {
  return [
    { cmd: CAM, name: 'a', values: 'a' },
    { settle: 30, name: 'b' },
    { cmd: 'r_fixedShaderTime 5.25', name: 'c' },
    { cmd: 'r_fixedShaderTime 5;r_oaxParticles 0', name: 'off', values: 'off' },
    { cmd: 'r_oaxParticles 1;cg_oaxParticles 0', name: 'cgoff' },
    { cmd: 'cg_oaxParticles 1;oaxfx oax/impact_sparks 20 -150 24 0 0 1', settle: 4, name: 'spark', values: 'spark' },
    { settle: 60, name: 'sparkdone', values: 'sparkdone' },
  ];
}

function checkBuild(build, r, ctx) {
  const { rows, failures } = ctx;
  const v = r.valuesAt;
  const im = r.images;
  isPicture(ctx, `${build} a`, im.a);
  rows.push(`${build}: r_fx_drawn ${v.a.r_fx_drawn}, particle stages ${v.a.r_particle_stages}, particles ${v.a.r_particles_drawn}; off: ${v.off.r_particles_drawn}`);
  if (!(v.a.r_oax_features || '').split(/\s+/).includes('particles')) failures.push(`${build}: the renderer does not advertise particles`);
  if (num(v.a.r_fx_drawn) < 3 || num(v.a.r_particles_drawn) < 50) failures.push(`${build}: the emitters do not draw`);
  if (num(v.off.r_particles_drawn) !== 0) failures.push(`${build}: r_oaxParticles 0 still draws particles`);

  const frozen = diffFraction(im.a, im.b, 0);
  const moved = diffFraction(im.a, im.c, 8);
  rows.push(`${build}: frozen time 30 frames apart ${(frozen * 100).toFixed(4)}% differ; another pinned time ${(moved * 100).toFixed(2)}%`);
  if (frozen > 0) failures.push(`${build}: frozen time does not render identically (${(frozen * 100).toFixed(4)}%)`);
  if (moved < 0.002) failures.push(`${build}: control did not fail: another time renders the same particles`);

  const off = diffFraction(im.a, im.off, 8);
  const cgoff = diffFraction(im.off, im.cgoff, 0);
  rows.push(`${build}: particles off changes ${(off * 100).toFixed(2)}%; cg_oaxParticles 0 vs r_oaxParticles 0 ${(cgoff * 100).toFixed(4)}% differ`);
  if (off < 0.005) failures.push(`${build}: turning particles off changes too little`);
  if (cgoff > 0) failures.push(`${build}: cg_oaxParticles 0 does not give the particle-free frame`);

  const spark = diffFraction(im.a, im.spark, 8);
  const done = diffFraction(im.a, im.sparkdone, 0);
  rows.push(`${build}: one-shot sparks: live ${v.spark.cg_fx_live} -> ${v.sparkdone.cg_fx_live}, frame change ${(spark * 100).toFixed(3)}%, after it ended ${(done * 100).toFixed(4)}% differ`);
  if (num(v.spark.cg_fx_live) < 1 || spark < 0.0005) failures.push(`${build}: the one-shot system did not draw`);
  if (num(v.sparkdone.cg_fx_live) !== 0) failures.push(`${build}: the one-shot system was not freed after it ended`);
  if (done > 0) failures.push(`${build}: the frame after the one-shot ended is not the emitter frame`);
}

export async function run({ goldens, out, update }) {
  const ctx = { goldens, out, update, rows: [], failures: [] };
  const cart = await cartShots('fx-particles', MAP, shotList(), { setup: SETUP, out });
  checkBuild('cart', cart, ctx);
  golden(ctx, 'fx_particles', cart.images.a);
  goldenControl(ctx, 'fx_particles', cart.images.off, 'the frame without particles');

  const native = nativeShots('fx-particles', MAP, shotList(), { setup: SETUP });
  checkBuild('native', native, ctx);
  nativeMatchesCart(ctx, 'emitter frame', native.images.a, cart.images.a, cart.images.off);
  return { ok: ctx.failures.length === 0, failures: ctx.failures, rows: ctx.rows };
}
