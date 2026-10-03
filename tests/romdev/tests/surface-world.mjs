// Surface world, rendering and loading (step 7.5, docs/map-format.md):
// oax_surfworld draws OAX_SURFACES surfaces (polygons, a concave polygon,
// an indexed mesh, a stock material, masked, translucent, additive,
// tinted, two-sided, a mover's surfaces) lit by unified lighting, over a
// caulk hull, next to an ordinary brush pillar.
//
// Checked:
//   - loading: every visible surface became a world surface (the
//     invisible one skipped), no unknown material, the renderer and the
//     collision model hashed the same lump bytes as the file holds; the
//     map's lights reach the surfaces (light interactions), the arch's
//     light-mask group keeps the group-1 lights off it and nothing breaks
//     a mask; load-time validation finds no surface buried in or floating
//     off the hull;
//   - cart goldens of nine views (room A, the platform and ramp, the glass
//     pane, the grate, the door open, room B, the arch, the glow panel),
//     each a real picture;
//   - areas: from room A with the door closed, room B's surfaces are culled
//     (none reached), and the same camera sees them once the player opens
//     the door;
//   - the native client renders every view like the cart.
// Controls: the same view on oax_surfworld_nosurf (the hull without its
// surfaces) differs from the golden; validation on oax_surfworld_nohull
// (surfaces over a hull 512 units lower, plus one surface inside it)
// reports the floating surfaces and exactly the buried one.

import fs from 'node:fs';
import path from 'node:path';
import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { comparePng, readPng, halfSize, writePng } from '../lib/png.mjs';
import { meanDiff } from '../lib/imgstat.mjs';
import { mapPath } from '../lib/scenes.mjs';
import { surfacesFromBsp, OSF } from '../../../misc/tools/oax-surfaces.mjs';
import { readTga } from '../lib/tga.mjs';
import { outDir as mapsOut } from '../../maps/build.mjs';
import { TINT, TINT_PANELS, ROOM_B, ROOM_C, SHADOW_GRATES } from '../../maps/src/oax_surfworld.mjs';

export const name = 'surface-world';

const MAP = 'oax_surfworld';
const SETUP = ['r_fixedShaderTime 3', 'r_autoExposure 0'];
// colour measurements: linear output (no tone map) and no specular (the
// tint is the diffuse colour's; specular light is not tinted)
const MEASURE = [...SETUP, 'r_toneMap 0', 'r_ulightSpecular 0'];
const TINT_CAM = [700, -200, 190], DARK_CAM = [1240, 0, 95], TOP_CAM = [560, -200, 600];
const measureShots = () => [
  { cmd: IN_B, settle: 80 },
  { cmd: `cl_overrideView "${TINT_CAM.join(' ')} 0 0 0"`, name: 'tint' },
  { cmd: `cl_overrideView "${TOP_CAM.join(' ')} 90 0 0"`, name: 'top', values: 'top' },
  { cmd: 'r_ulightShadows 0', name: 'topoff' },
  { cmd: 'r_ulightShadows 1', settle: 2 },
  { cmd: `setviewpos 1300 0 24 0`, settle: 60 },
  { cmd: `cl_overrideView "${DARK_CAM.join(' ')} 0 0 0"`, name: 'dark' },
];

// mean colour of a world rectangle on the plane x = px (y and z ranges),
// seen from cam looking along +x (yaw 0, pitch 0, cg_fov 90); the inner 70%
function panelMean(img, cam, px, y, z) {
  const f = img.width / 2;
  const P = (yy, zz) => [img.width / 2 - ((yy - cam[1]) / (px - cam[0])) * f, img.height / 2 - ((zz - cam[2]) / (px - cam[0])) * f];
  const a = P(y[1], z[1]), b = P(y[0], z[0]);
  const x0 = Math.round(a[0] + (b[0] - a[0]) * 0.15), x1 = Math.round(b[0] - (b[0] - a[0]) * 0.15);
  const y0 = Math.round(a[1] + (b[1] - a[1]) * 0.15), y1 = Math.round(b[1] - (b[1] - a[1]) * 0.15);
  const s = [0, 0, 0];
  let n = 0;
  for (let yy = y0; yy < y1; yy++) for (let xx = x0; xx < x1; xx++) {
    const i = (yy * img.width + xx) * 4;
    s[0] += img.data[i]; s[1] += img.data[i + 1]; s[2] += img.data[i + 2]; n++;
  }
  return s.map((v) => v / n);
}

// mean colour of a floor rectangle (x and y ranges) seen straight down from
// cam (pitch 90, yaw 0: image up is +x, image right is -y), the inner 70%
function floorMean(img, cam, x, y) {
  const f = img.width / 2, h = cam[2];
  const sx = (yy) => img.width / 2 + ((cam[1] - yy) / h) * f, sy = (xx) => img.height / 2 - ((xx - cam[0]) / h) * f;
  const xa = sx(y[1]), xb = sx(y[0]), ya = sy(x[1]), yb = sy(x[0]);
  const x0 = Math.round(xa + (xb - xa) * 0.15), x1 = Math.round(xb - (xb - xa) * 0.15);
  const y0 = Math.round(ya + (yb - ya) * 0.15), y1 = Math.round(yb - (yb - ya) * 0.15);
  let t = 0, n = 0;
  for (let yy = y0; yy < y1; yy++) for (let xx = x0; xx < x1; xx++) {
    const i = (yy * img.width + xx) * 4;
    t += img.data[i] + img.data[i + 1] + img.data[i + 2]; n++;
  }
  return t / n / 3;
}

// shadow pair: the floor behind each grate (away from the light between
// them) with shadows on over shadows off: OSF_NOSHADOW leaves it unchanged
// (within 2%); control: the plain grate's shadow darkens its patch
function shadowChecks(label, images, values, failures, rows) {
  const span = SHADOW_GRATES.y, light = 560;
  const behind = (gx) => (gx < light ? [gx - 120, gx - 30] : [gx + 30, gx + 120]);
  const r = {};
  for (const k of ['plain', 'noshadow']) {
    const x = behind(SHADOW_GRATES[k]);
    r[k] = floorMean(images.top, TOP_CAM, x, span) / floorMean(images.topoff, TOP_CAM, x, span);
  }
  rows.push(`${label} shadows: the floor behind the plain grate at ${r.plain.toFixed(3)} of its unshadowed value, behind the OSF_NOSHADOW grate ${r.noshadow.toFixed(3)}; no-shadow surfaces ${values.r_surfworld_noshadow}, lit ${values.r_surfworld_noshadow_lit}`);
  if (!(Math.abs(r.noshadow - 1) <= 0.02)) failures.push(`${label}: the OSF_NOSHADOW grate shadows the floor (${r.noshadow.toFixed(3)})`);
  if (!(r.plain < 0.9)) failures.push(`${label}: control: the plain grate casts no shadow (${r.plain.toFixed(3)})`);
  if (values.r_surfworld_noshadow !== '1' || values.r_surfworld_noshadow_lit !== '1') failures.push(`${label}: the no-shadow grate is not lit (${values.r_surfworld_noshadow_lit} of ${values.r_surfworld_noshadow})`);
}

// tint pairs: the tinted panel's colour over the plain one's equals the
// tint, per channel, within 2%; unlit: an unlit material in room C (no
// light reaches it) shows its texture's mean colour within 3%, a lit
// material next to it stays dark (ambient only)
function colourChecks(label, images, stoneMean, failures, rows) {
  const px = ROOM_B[1] - 0.5;
  for (const k of ['wall', 'stone']) {
    const z = TINT_PANELS[k].z;
    const plain = panelMean(images.tint, TINT_CAM, px, TINT_PANELS.y.plain, z);
    const tinted = panelMean(images.tint, TINT_CAM, px, TINT_PANELS.y.tinted, z);
    const ratio = tinted.map((v, c) => v / plain[c]);
    const err = Math.max(...ratio.map((r) => Math.abs(r / TINT - 1)));
    rows.push(`${label} tint ${TINT} on ${k}: tinted/plain ${ratio.map((r) => r.toFixed(3)).join(' ')} (worst ${(err * 100).toFixed(2)}% off; plain ${plain.map((v) => v.toFixed(1)).join(' ')})`);
    if (!(err <= 0.02)) failures.push(`${label}: the ${k} panel's tint draws ${ratio.map((r) => r.toFixed(3)).join(' ')}, not ${TINT}`);
    // control: the measurement sees the tint at all (an untinted ratio of 1 must fail)
    if (Math.max(...ratio.map((r) => Math.abs(r - 1))) < 0.1) failures.push(`${label}: control: the ${k} tint pair is not different`);
    if (!(Math.min(...plain) > 20)) failures.push(`${label}: the plain ${k} panel is too dark to measure`);
  }
  const unlit = panelMean(images.dark, DARK_CAM, ROOM_C.x[1] - 0.5, [-100, -10], [40, 150]);
  const lit = panelMean(images.dark, DARK_CAM, ROOM_C.x[1] - 0.5, [10, 100], [40, 150]);
  const uerr = Math.max(...unlit.map((v, c) => Math.abs(v / stoneMean[c] - 1)));
  rows.push(`${label} unlit material in a dark room: ${unlit.map((v) => v.toFixed(1)).join(' ')} (texture mean ${stoneMean.map((v) => v.toFixed(1)).join(' ')}, ${(uerr * 100).toFixed(2)}% off); lit material beside it ${lit.map((v) => v.toFixed(1)).join(' ')}`);
  if (!(uerr <= 0.03)) failures.push(`${label}: the unlit material does not show its texture colour (${unlit.map((v) => v.toFixed(1)).join(' ')})`);
  if (!(Math.max(...lit.map((v, c) => v / stoneMean[c])) < 0.25)) failures.push(`${label}: control: the lit material in the dark room is not dark`);
}
const TOLERANCE = 24;
const MIN_COLORS = 1500;
const IN_A = 'setviewpos -900 300 24 0';
const AT_DOOR = 'setviewpos -80 0 24 0';
const IN_B = 'setviewpos 600 -300 24 180';
const VIEWS = {
  a1: '-1000 -480 220 18 35 0',
  plat: '-250 -280 150 25 200 0',
  glass: '-420 120 80 0 90 0',
  grate: '-60 246 70 0 180 0',
  bview: '100 -450 200 12 40 0',
  b1: '100 -450 200 12 40 0',
  arch: '500 300 90 5 0 0',
  glow: '875 300 200 0 90 0',
  door: '-400 0 70 0 0 0',
};
const GOLDEN = ['a1', 'plat', 'glass', 'grate', 'door', 'b1', 'arch', 'glow', 'bopen'];

const view = (n) => ({ cmd: `cl_overrideView "${VIEWS[n]}"`, name: n, values: n });

function shotList() {
  return [
    { cmd: IN_A, settle: 80 },
    view('a1'), view('plat'), view('glass'), view('grate'),
    { ...view('bview'), name: 'bclosed', values: 'bclosed' },
    { cmd: AT_DOOR, settle: 90 },
    { ...view('bview'), name: 'bopen', values: 'bopen' },
    view('door'),
    { cmd: IN_B, settle: 200 },
    view('b1'), view('arch'), view('glow'),
  ];
}

export function uniqueColors(img) {
  const s = new Set();
  for (let i = 0; i < img.data.length; i += 4) s.add((img.data[i] << 16) | (img.data[i + 1] << 8) | img.data[i + 2]);
  return s.size;
}

function fnv(buf) {
  let h = 2166136261;
  for (const b of buf) h = Math.imul(h ^ b, 16777619) >>> 0;
  return h.toString(16).padStart(8, '0');
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const gdir = path.join(goldens, 'surfworld');
  fs.mkdirSync(gdir, { recursive: true });

  // what the file holds
  const bsp = fs.readFileSync(mapPath(MAP));
  const { surfaces } = surfacesFromBsp(bsp);
  if (!surfaces) return { ok: false, failures: [`${MAP} has no OAX_SURFACES lump`], rows };
  const visible = surfaces.surfaces.filter((s) => !(s.flags & OSF.INVISIBLE)).length;
  const { lumps } = await import('../../../misc/tools/bspx.mjs').then((m) => m.readBspx(bsp));
  const lumpHash = fnv(lumps.find((l) => l.name === 'OAX_SURFACES').data);

  const cart = await cartShots('surface-world', MAP, shotList(), { setup: SETUP, out });
  const v = cart.valuesAt.a1;

  // ---- loading -----------------------------------------------------------------
  rows.push(`loaded: ${v.r_surfworld_surfaces} surfaces of ${surfaces.surfaces.length} (${v.r_surfworld_skipped} skipped), ${v.r_surfworld_variants} shader variants, ${v.r_surfworld_unknown} unknown materials; ` +
    `hash file ${lumpHash} renderer ${v.r_surfworld_hash} collision ${v.cm_surf_hash}`);
  if (Number(v.r_surfworld_surfaces) !== visible) failures.push(`the renderer loaded ${v.r_surfworld_surfaces} surfaces, the lump has ${visible} visible`);
  if (Number(v.r_surfworld_skipped) !== surfaces.surfaces.length - visible) failures.push('invisible surfaces were not skipped');
  if (v.r_surfworld_unknown !== '0') failures.push(`unknown materials: ${v.r_surfworld_unknown_names}`);
  if (v.r_surfworld_hash !== lumpHash || v.cm_surf_hash !== lumpHash) failures.push('the renderer or the collision model read different lump bytes');
  rows.push(`lighting: ${v.r_surfworld_lit} surfaces lit, ${v.r_surfworld_interactions} light interactions, ${v.r_surfworld_mask_excluded} kept apart by light-mask groups, ${v.r_surfworld_mask_violations} mask violations`);
  // every surface but room C's 9 (no light reaches room C) and a few the lights' volumes miss
  if (!(Number(v.r_surfworld_lit) >= visible - 9 - 6)) failures.push(`only ${v.r_surfworld_lit} surfaces lit by the unified lights`);
  if (!(Number(v.r_surfworld_mask_excluded) > 0)) failures.push('light-mask groups kept no light off a surface');
  if (v.r_surfworld_mask_violations !== '0') failures.push('a light lights a surface outside its mask groups');
  rows.push(`validation: ${v.cm_surf_checked} checked, ${v.cm_surf_buried} buried, ${v.cm_surf_floating} floating; collision meshes ${v.cm_coll_meshes} (${v.cm_coll_tris} triangles)`);
  if (v.cm_surf_buried !== '0' || v.cm_surf_floating !== '0') failures.push(`validation flagged surfaces: buried ${v.cm_surf_buried_ids} floating ${v.cm_surf_floating_ids}`);
  if (!(Number(v.cm_surf_checked) >= 20)) failures.push(`only ${v.cm_surf_checked} surfaces checked against the hull`);

  // ---- goldens -------------------------------------------------------------------
  for (const n of GOLDEN) {
    const img = cart.images[n];
    if (!img) { failures.push(`${n}: no screenshot`); continue; }
    const colors = uniqueColors(img);
    const golden = path.join(gdir, `surfworld_${n}.png`);
    const half = halfSize(img);
    if (update || !fs.existsSync(golden)) {
      writePng(golden, half);
      rows.push(`${n}: ${colors} colours, golden ${update ? 'updated' : 'created'}`);
    } else {
      const r = comparePng(readPng(golden), half, { tolerance: TOLERANCE, diffPath: path.join(out, `surface-world_${n}.diff.png`) });
      rows.push(`${n}: ${colors} colours, ${cart.valuesAt[n]?.r_surfworld_drawn} surfaces reached, golden ${(r.badFraction * 100).toFixed(3)}% over tolerance`);
      if (!r.sameSize || r.badFraction > 0.005) failures.push(`golden ${n}: ${(r.badFraction * 100).toFixed(2)}% differ`);
    }
    if (colors < MIN_COLORS) failures.push(`${n}: only ${colors} colours`);
  }

  // ---- areas ------------------------------------------------------------------------
  const closed = Number(cart.valuesAt.bclosed.r_surfworld_drawn), open = Number(cart.valuesAt.bopen.r_surfworld_drawn);
  rows.push(`areas: room B's camera from a player in room A: ${closed} surfaces reached with the door closed, ${open} with it open`);
  if (closed !== 0) failures.push(`the closed door's area portal let ${closed} room-B surfaces through`);
  if (!(open > 5)) failures.push(`with the door open the camera reached only ${open} surfaces`);

  // ---- native ---------------------------------------------------------------------------
  const nat = nativeShots('surface-world', MAP, shotList(), { setup: SETUP });
  let worst = 0;
  for (const n of GOLDEN) {
    const a = nat.images[n], b = cart.images[n];
    if (!a || !b) { failures.push(`native ${n}: no screenshot`); continue; }
    writePng(path.join(out, `surface-world_${n}.native.png`), a);
    const m = meanDiff(a, b);
    worst = Math.max(worst, m);
    if (m > 4) failures.push(`native ${n} differs from the cart (mean ${m.toFixed(2)})`);
  }
  rows.push(`native vs cart: worst mean difference ${worst.toFixed(2)} over ${GOLDEN.length} views; native values: ${nat.values.r_surfworld_surfaces} surfaces, hash ${nat.values.r_surfworld_hash}, validation ${nat.values.cm_surf_buried}/${nat.values.cm_surf_floating}`);
  if (nat.values.r_surfworld_surfaces !== v.r_surfworld_surfaces || nat.values.r_surfworld_hash !== v.r_surfworld_hash) failures.push('native loaded a different surface world');
  if (Number(nat.valuesAt.bclosed?.r_surfworld_drawn) !== 0) failures.push('native: the closed door did not cull room B');

  // ---- colours: tints and unlit materials -------------------------------------------------
  {
    const tga = readTga(path.join(mapsOut, 'baseoa', 'textures', 'oax_surfworld', 'stone.tga'));
    const stoneMean = [0, 1, 2].map((c) => { let t = 0; for (let i = c; i < tga.data.length; i += 4) t += tga.data[i]; return t / (tga.data.length / 4); });
    const cm = await cartShots('surface-world-colour', MAP, measureShots(), { setup: MEASURE, out });
    colourChecks('cart', cm.images, stoneMean, failures, rows);
    shadowChecks('cart', cm.images, cm.valuesAt.top, failures, rows);
    const nm = nativeShots('surface-world-colour', MAP, measureShots(), { setup: MEASURE });
    if (!nm.images.tint || !nm.images.dark) failures.push('native colour shots missing');
    else {
      colourChecks('native', nm.images, stoneMean, failures, rows);
      if (!nm.images.top || !nm.images.topoff) failures.push('native shadow shots missing');
      else shadowChecks('native', nm.images, nm.valuesAt.top || {}, failures, rows);
    }
  }

  // ---- controls --------------------------------------------------------------------------
  const ns = await cartShots('surface-world-nosurf', 'oax_surfworld_nosurf', [{ cmd: IN_A, settle: 80 }, view('a1')], { setup: SETUP, out });
  const ctl = comparePng(readPng(path.join(gdir, 'surfworld_a1.png')), halfSize(ns.images.a1), { tolerance: TOLERANCE });
  rows.push(`control surfaces removed: a1 ${(ctl.badFraction * 100).toFixed(1)}% differs from the golden (${ns.valuesAt.a1.r_surfworld_surfaces || 0} surfaces loaded)`);
  if (ctl.badFraction < 0.3) failures.push('control: the golden matches the map without its surfaces');
  const nh = await cartShots('surface-world-nohull', 'oax_surfworld_nohull', [{ cmd: 'setviewpos -900 300 -480 0', settle: 40, values: 'v' }], { setup: SETUP, out });
  const hv = nh.valuesAt.v;
  rows.push(`control hull lowered: validation ${hv.cm_surf_checked} checked, ${hv.cm_surf_floating} floating, ${hv.cm_surf_buried} buried (${hv.cm_surf_buried_ids})`);
  if (!(Number(hv.cm_surf_floating) >= 10)) failures.push(`control: only ${hv.cm_surf_floating} surfaces reported floating over the lowered hull`);
  if (hv.cm_surf_buried !== '1' || !/:777$/.test(hv.cm_surf_buried_ids)) failures.push(`control: the buried surface was not the one reported (${hv.cm_surf_buried_ids})`);

  return { ok: failures.length === 0, failures, rows };
}
