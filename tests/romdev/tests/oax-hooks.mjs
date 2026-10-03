// The verification hooks and material keywords of step 7.5 D
// (docs/test-hooks.md, docs/materials.md), on the cart AND native, every
// check next to a control that must fail.
//
// Map oax_hooks (tests/maps/src/oax_hooks.mjs), captured at the engine's
// default r_picmip (the run boots with no picmip override, so r_capture
// must report 0) and 1280x720:
// - exact placement: `setviewpos x y z yaw pitch` reads back exactly the
//   requested origin and (quantized) angles, and the rendered eye is that
//   origin plus the view height; native == cart. Control: the stock
//   four-number teleport ends tens of units away.
// - one freeze: with cl_oaxFreezeTime two shots 120 frames apart are
//   identical (sky portal, scrolling shader, rotating mover, light style,
//   item bob, particles in view); control: unfrozen they differ.
// - surface ids: the centre pixel names tint_red, the floor pixel names
//   clang_floor (control: another pixel, another material); the dump
//   agrees with the pixel read and names the panel materials.
// - oaxTint 1 0.5 0.5: tint_red / tint_plain mean colour ratio, measured
//   over the panels' own pixels (from the dump); control: two untinted
//   panels measure ~1.
// - detailFade 128 384: far away the faded panel matches the no-detail
//   panel while the always-detail one does not; near it matches the
//   always-detail panel and not the plain one.
// - sky area models: the sky room's gargoyle (empty light grid) is lit
//   green by oaxSkyAmbient / oaxSkyLight; control: r_oaxSkyModelLight 0
//   leaves it dark.
// - content isolation (oax_iso.mjs): the probe texture is red in oax_iso,
//   whose own package carries a red copy, and green in oax_hooks (loose
//   map, the later package wins); the conflict report names the probe and
//   not the identical same.tga; fs_mappack names the package.
// - cart goldens: tint, near detailFade and far views; controls: another
//   view against each golden fails.

import fs from 'node:fs';
import path from 'node:path';
import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { comparePng, readPng, halfSize, writePng } from '../lib/png.mjs';
import { parseSurfIdValue, parseSurfIdDump, materialMask, maskStats } from '../lib/surfids.mjs';
import { PANELS, PANEL_Y, TINT } from '../../maps/src/oax_hooks.mjs';
import { PACK_A, PACK_B, PROBE, SAME } from '../../maps/src/oax_iso.mjs';
import { PANELS as SW_PANELS, PANEL_Y as SW_PANEL_Y, FADE_END, MAT as SW_MAT, FLOOR_QUADS, FLOOR_EYE } from '../../maps/src/oax_hooks_sw.mjs';

export const name = 'oax-hooks';

const MAP = 'oax_hooks';
const SETUP = ['r_fixedShaderTime -1', 'r_autoExposure 0', 'cl_overrideView ""'];
const V = (v) => `cl_overrideView "${v}"`;
const TOLERANCE = 24;
const MAX_BAD = 0.005;
// a resting spot: floor 0 + the player's mins (24) + the collision epsilon
// (0.125) traces keep from every surface
const PLACE = { x: 0, y: 0, z: 24.125, yaw: 90, pitch: 15 };
const ANGLE_STEP = 360 / 65536;
const VIEW_HEIGHT = 26;
const FREEZE_VIEW = '60 250 150 -25 270 0';
const centre = (name) => (PANELS[name][0] + PANELS[name][1]) / 2;
const TINT_VIEW = `${centre('tint_red')} 150 128 0 90 0`;
const FAR_VIEW = `${centre('detail_always')} -250 128 0 90 0`;
const near = (n) => `${centre(n)} ${PANEL_Y - 56} 128 0 90 0`;
const SKY_VIEW = '0 0 100 -89 0 0';

function shots() {
  return [
    { cmd: `setviewpos ${PLACE.x} ${PLACE.y} ${PLACE.z} ${PLACE.yaw} ${PLACE.pitch}`, settle: 30, values: 'place' },
    { cmd: `setviewpos 100 0 24 ${PLACE.yaw}`, settle: 30, values: 'placeStock' },
    { cmd: `cl_oaxFreezeTime 1000;${V(FREEZE_VIEW)}`, settle: 10, name: 'frozenA', values: 'frozen' },
    { settle: 120, name: 'frozenB' },
    { cmd: 'cl_oaxFreezeTime -1', settle: 2, name: 'liveA' },
    { settle: 120, name: 'liveB' },
    { cmd: `cl_oaxFreezeTime 1000;r_oaxSurfaceIdAt "640 360 380 360";${V(TINT_VIEW)}`, settle: 10, name: 'tint', values: 'tintIds' },
    { cmd: 'r_oaxSurfaceIdAt "";r_oaxSurfaceIdDump 1', settle: 4, blob: 'tint' },
    { cmd: V(FAR_VIEW), settle: 10, name: 'far' },
    { cmd: 'r_oaxSurfaceIdDump 1', settle: 4, blob: 'far' },
    { cmd: V(near('detail_fade')), settle: 10, name: 'nearFade' },
    { cmd: V(near('detail_always')), settle: 10, name: 'nearAlways' },
    { cmd: V(near('detail_none')), settle: 10, name: 'nearNone' },
    { cmd: V(SKY_VIEW), settle: 10, name: 'skyOn' },
    { cmd: 'r_oaxSkyModelLight 0', settle: 10, name: 'skyOff' },
    { cmd: `r_oaxSkyModelLight 1;${V('380 0 128 0 0 0')}`, settle: 10, name: 'probe', values: 'end' },
  ];
}
const ISO_SHOTS = [{ cmd: V('-160 0 64 60 0 0'), settle: 10, name: 'probeIso', values: 'iso' }];

// detailFade on the other paths (oax_hooks_sw: unified lighting,
// surface-world surfaces, a tinted surface-world variant, a brush face)
const SW_DEPTHS = [30, 95, 190, 285, 380, 600];
const swCentre = (k) => (SW_PANELS[k][0] + SW_PANELS[k][1]) / 2;
function swShots() {
  const list = Object.keys(SW_PANELS).map((k) => ({ cmd: V(`${swCentre(k)} ${SW_PANEL_Y - 30} 128 0 90 0`), settle: 8, name: `swNear_${k}` }));
  for (const d of SW_DEPTHS) list.push({ cmd: V(`${swCentre('fade')} ${SW_PANEL_Y - d} 128 0 90 0`), settle: 8, name: `swDepth${d}` });
  for (const k of Object.keys(FLOOR_QUADS)) {
    const x = (FLOOR_QUADS[k][0] + FLOOR_QUADS[k][1]) / 2;
    list.push({ cmd: V(`${x} ${FLOOR_EYE.y} ${FLOOR_EYE.z} ${FLOOR_EYE.pitch} 90 0`), settle: 8, name: `swFloor_${k}` });
  }
  list.push({ cmd: `r_oaxSurfaceIdDump 1;${V('0 -900 128 0 90 0')}`, settle: 8, name: 'swFar', blob: 'swFar' });
  return list;
}
// r_toneMap 0: the frame is linear in light, so the fade curve reads directly
const SW_SETUP = [...SETUP, 'cl_oaxFreezeTime 1000', 'r_toneMap 0'];

function checkSurfaceWorld(build, r, failures, rows) {
  const fail = (m) => failures.push(`${build}: ${m}`);
  // near (view depth 30): every path draws the green detail stage
  const green = (img) => { const c = centreColour(img, 20); return { ...c, gr: c.g / Math.max(1, c.r) }; };
  const near = Object.fromEntries(Object.keys(SW_PANELS).map((k) => [k, green(r.images[`swNear_${k}`])]));
  rows.push(`${build}: surface world + unified lighting, depth 30: green g/r ${Object.entries(near).map(([k, c]) => `${k} ${c.gr.toFixed(1)}`).join(', ')}`);
  for (const k of ['always', 'fade', 'fadeTinted', 'fadeVertex', 'fadeBrush']) if (!(near[k].gr > 4)) fail(`depth 30: the ${k} panel's detail stage does not show (g/r ${near[k].gr.toFixed(2)})`);
  if (!(near.plain.gr < 1.5)) fail('control did not fail: the plain panel is green');
  // a small quad seen face on: per-vertex and per-pixel fades agree
  if (!near1(near.fadeVertex.gr / near.fade.gr, 1, 0.15)) fail(`small quad: per-vertex g/r ${near.fadeVertex.gr.toFixed(2)} vs per-pixel ${near.fade.gr.toFixed(2)}`);
  // a big floor quad whose corners are all beyond the end (by view depth;
  // the near ones behind the eye): per vertex, UE1's way, the pixel 42
  // deep under the eye gets no detail; per pixel it gets nearly full
  const fv = green(r.images.swFloor_vertex), fp = green(r.images.swFloor_pixel);
  rows.push(`${build}: big floor quad, centre pixel ~42 deep: g/r per vertex ${fv.gr.toFixed(2)}, per pixel ${fp.gr.toFixed(2)}`);
  if (!(fv.gr < 1.5)) fail(`big quad, per vertex: the detail shows under the eye (g/r ${fv.gr.toFixed(2)})`);
  if (!(fp.gr > 4)) fail(`control did not fail: big quad, per pixel: no detail under the eye (g/r ${fp.gr.toFixed(2)})`);
  // linear in view depth, full at 0, none at FADE_END (UE1): the 2x
  // modulate leaves red at depth/end of the undetailed value
  // (a 60-pixel crop: texture filtering changes a little with distance)
  const rd = Object.fromEntries(SW_DEPTHS.map((d) => [d, centreColour(r.images[`swDepth${d}`], 30).r]));
  const ref = rd[600];
  rows.push(`${build}: fade panel red by depth: ${SW_DEPTHS.map((d) => `${d}: ${(rd[d] / ref).toFixed(3)} (${Math.min(1, d / FADE_END).toFixed(3)})`).join(', ')}`);
  for (const d of SW_DEPTHS.slice(0, 4)) if (!near1(rd[d] / ref, d / FADE_END, 0.03)) fail(`depth ${d}: red ${(rd[d] / ref).toFixed(3)} of undetailed, expected ${(d / FADE_END).toFixed(3)} (linear fade)`);
  if (!near1(rd[380] / ref, 1, 0.02)) fail(`depth ${FADE_END}: the stage still shows (${(rd[380] / ref).toFixed(3)})`);
  // far: faded panels look like the plain one; control: always-detail does not
  const d = parseSurfIdDump(r.blobs.swFar);
  const st = (m) => maskStats(r.images.swFar, materialMask(d, m.slice('textures/'.length)));
  const pl = st(SW_MAT.plain), fa = st(SW_MAT.fade), al = st(SW_MAT.always);
  const gr = (x) => x.g / Math.max(1, x.r);
  rows.push(`${build}: far (depth 1150): g/r plain ${gr(pl).toFixed(3)}, fade (3 panels, ${fa.n} px) ${gr(fa).toFixed(3)}, always ${gr(al).toFixed(3)}`);
  if (!pl.n || !fa.n || !al.n) fail('a surface-world panel has no pixels in the far dump');
  if (!near1(gr(fa), gr(pl), 0.08)) fail('far: the faded panels still show the detail stage');
  if (!(gr(al) > 2 * gr(pl))) fail('control did not fail: far, the always-detail panel looks plain');
}

const nums = (s) => String(s || '').trim().split(/\s+/).map(Number);
const near1 = (a, b, eps) => Math.abs(a - b) <= eps;
function centreColour(img, w = 40) {
  let r = 0, g = 0, b = 0, n = 0;
  for (let y = (img.height >> 1) - w; y < (img.height >> 1) + w; y++) {
    for (let x = (img.width >> 1) - w; x < (img.width >> 1) + w; x++) {
      const o = (y * img.width + x) * 4;
      r += img.data[o]; g += img.data[o + 1]; b += img.data[o + 2]; n++;
    }
  }
  return { r: r / n, g: g / n, b: b / n };
}

function checkBuild(build, r, iso, failures, rows) {
  const fail = (m) => failures.push(`${build}: ${m}`);
  const v = r.valuesAt;

  // capture profile: the engine default
  rows.push(`${build}: r_capture ${v.end.r_capture} (width height picmip, engine default picmip)`);
  if (v.end.r_capture !== '1280 720 0') fail(`capture is not 1280x720 at r_picmip 0 (${v.end.r_capture})`);

  // exact placement
  const req = nums(v.place.g_place_request), got = nums(v.place.g_place), eye = nums(v.place.cl_view);
  rows.push(`${build}: setviewpos ${PLACE.x} ${PLACE.y} ${PLACE.z} yaw ${PLACE.yaw} pitch ${PLACE.pitch}: g_place ${v.place.g_place}, cl_view ${v.place.cl_view}, frames ${v.place.g_place_frames}`);
  if (got.length < 7 || got[1] !== PLACE.x || got[2] !== PLACE.y || got[3] !== PLACE.z) fail(`read-back origin ${got.slice(1, 4)} != requested ${[PLACE.x, PLACE.y, PLACE.z]}`);
  if (!near1(got[4], PLACE.pitch, ANGLE_STEP) || !near1(got[5], PLACE.yaw, ANGLE_STEP)) fail(`read-back angles ${got.slice(4, 6)} != requested pitch ${PLACE.pitch} yaw ${PLACE.yaw}`);
  if (req[3] !== PLACE.z) fail(`g_place_request ${v.place.g_place_request}`);
  if (!near1(eye[0], PLACE.x, 0.01) || !near1(eye[1], PLACE.y, 0.01) || !near1(eye[2], PLACE.z + VIEW_HEIGHT, 0.01)
    || !near1(eye[3], PLACE.pitch, 0.01) || !near1(eye[4], PLACE.yaw, 0.01)) fail(`rendered eye ${v.place.cl_view} is not the placement + view height`);
  const stock = nums(v.placeStock.g_place);
  const off = Math.hypot(stock[1] - 100, stock[2], stock[3] - 24);
  rows.push(`${build}: control: stock setviewpos 100 0 24 ${PLACE.yaw} ends at ${stock.slice(1, 4).map((x) => x.toFixed(1))}, ${off.toFixed(1)} units away`);
  if (off < 10) fail(`control did not fail: the stock teleport landed on its spot (${off.toFixed(2)} away)`);

  // one freeze
  const fz = comparePng(r.images.frozenA, r.images.frozenB, { tolerance: 0 });
  const lv = comparePng(r.images.liveA, r.images.liveB, { tolerance: 0 });
  rows.push(`${build}: frozen shots 120 frames apart: ${(fz.badFraction * 100).toFixed(3)}% differ (cl_view_time ${v.frozen.cl_view_time}); control unfrozen: ${(lv.badFraction * 100).toFixed(1)}%`);
  if (fz.badFraction !== 0) fail(`frozen shots differ (${(fz.badFraction * 100).toFixed(3)}%)`);
  if (v.frozen.cl_view_time !== '1000') fail(`frozen scene time ${v.frozen.cl_view_time}, not 1000`);
  if (lv.badFraction < 0.01) fail(`control did not fail: unfrozen shots match (${(lv.badFraction * 100).toFixed(3)}%)`);

  // surface ids at pixels
  const c = parseSurfIdValue(v.tintIds.r_surfid0), f = parseSurfIdValue(v.tintIds.r_surfid1);
  rows.push(`${build}: surface id (640,360): ${c.kind} ${c.index} ${c.shader}; control (380,360): ${f.kind} ${f.index} ${f.shader}`);
  if (c.shader !== 'textures/oax_hooks/tint_red' || c.kind !== 'world') fail(`centre pixel is not tint_red (${v.tintIds.r_surfid0})`);
  if (f.shader !== 'textures/oax_hooks/tint_plain') fail(`the (380,360) pixel is not tint_plain (${v.tintIds.r_surfid1})`);
  if (f.shader === c.shader || f.id === c.id) fail('control did not fail: two pixels name the same surface');

  // the dump
  const td = parseSurfIdDump(r.blobs.tint);
  const dumpCentre = td.surfs.get(td.ids[360 * td.width + 640]);
  rows.push(`${build}: dump ${td.width}x${td.height}, ${td.surfs.size} surfaces, centre ${dumpCentre?.shader}${td.truncated ? ' (truncated)' : ''}`);
  if (dumpCentre?.shader !== c.shader) fail(`dump centre ${dumpCentre?.shader} != pixel read ${c.shader}`);

  // oaxTint
  const st = (img, dump, mat) => maskStats(img, materialMask(dump, mat));
  const plain = st(r.images.tint, td, 'oax_hooks/tint_plain'), red = st(r.images.tint, td, 'oax_hooks/tint_red'), none = st(r.images.tint, td, 'oax_hooks/detail_none');
  // chromaticity relative to red, so the light falling differently on each
  // panel cancels: (tinted g/r) / (plain g/r) = tint g / tint r
  const ratio = (a, b) => [1, (a.g / a.r) / (b.g / b.r), (a.b / a.r) / (b.b / b.r)];
  const tr = ratio(red, plain), cr = ratio(none, plain);
  rows.push(`${build}: oaxTint ${TINT.join(' ')}: red/plain (g/r, b/r) ${tr.slice(1).map((x) => x.toFixed(3)).join(' ')} (${red.n}/${plain.n} px); control none/plain ${cr.slice(1).map((x) => x.toFixed(3)).join(' ')} (${none.n} px)`);
  if (!plain.n || !red.n || !none.n) fail('a tint panel has no pixels in the dump');
  for (let i = 1; i < 3; i++) if (!near1(tr[i], TINT[i] / TINT[0], 0.06)) fail(`tint channel ${i} ratio ${tr[i].toFixed(3)} not ${TINT[i] / TINT[0]}`);
  if (cr.some((x) => !near1(x, 1, 0.08))) fail(`control: untinted panels measure ${cr.map((x) => x.toFixed(3))}`);
  if (Math.abs(cr[1] - tr[1]) < 0.25) fail('control did not fail: the measurement cannot tell tinted from untinted');

  // detailFade, far
  const fd = parseSurfIdDump(r.blobs.far);
  const dn = st(r.images.far, fd, 'oax_hooks/detail_none'), da = st(r.images.far, fd, 'oax_hooks/detail_always'), df = st(r.images.far, fd, 'oax_hooks/detail_fade');
  const rel = (a, b) => Math.abs(a - b) / Math.max(1, b);
  rows.push(`${build}: far (${(PANEL_Y + 250)} units): mean/std none ${dn.lum.toFixed(1)}/${dn.std.toFixed(1)}, fade ${df.lum.toFixed(1)}/${df.std.toFixed(1)}, always ${da.lum.toFixed(1)}/${da.std.toFixed(1)}`);
  if (!dn.n || !da.n || !df.n) fail('a detail panel has no pixels in the far dump');
  if (rel(df.lum, dn.lum) > 0.06 || rel(df.std, dn.std) > 0.15) fail(`far: the faded panel (${df.lum.toFixed(1)}) does not match the no-detail one (${dn.lum.toFixed(1)})`);
  if (rel(da.lum, dn.lum) < 0.15) fail(`control did not fail: far, the always-detail panel matches the no-detail one`);
  // near: each panel fills the frame (texture alignment differs per
  // panel, so compare statistics, not pixels)
  const all = new Uint8Array(r.images.nearFade.width * r.images.nearFade.height).fill(1);
  const nf = maskStats(r.images.nearFade, all), na = maskStats(r.images.nearAlways, all), nn = maskStats(r.images.nearNone, all);
  rows.push(`${build}: near (56 units): mean/std none ${nn.lum.toFixed(1)}/${nn.std.toFixed(1)}, fade ${nf.lum.toFixed(1)}/${nf.std.toFixed(1)}, always ${na.lum.toFixed(1)}/${na.std.toFixed(1)}`);
  if (rel(nf.lum, na.lum) > 0.06 || rel(nf.std, na.std) > 0.15) fail('near: the faded panel does not match the always-detail one');
  if (rel(nf.lum, nn.lum) < 0.15) fail('control did not fail: near, the faded panel looks like the no-detail one');

  // sky area model
  const on = r.images.skyOn, offImg = r.images.skyOff;
  const mask = new Uint8Array(on.width * on.height);
  for (let i = 0; i < mask.length; i++) {
    const o = i * 4;
    if (Math.abs(on.data[o] - offImg.data[o]) + Math.abs(on.data[o + 1] - offImg.data[o + 1]) + Math.abs(on.data[o + 2] - offImg.data[o + 2]) > 30) mask[i] = 1;
  }
  const mOn = maskStats(on, mask), mOff = maskStats(offImg, mask);
  rows.push(`${build}: sky room model: ${mOn.n} px change; lit rgb ${mOn.r.toFixed(0)} ${mOn.g.toFixed(0)} ${mOn.b.toFixed(0)}, control r_oaxSkyModelLight 0: ${mOff.r.toFixed(0)} ${mOff.g.toFixed(0)} ${mOff.b.toFixed(0)}`);
  if (mOn.n < 50) fail('the sky room model did not change with r_oaxSkyModelLight');
  if (!(mOn.g > 1.4 * mOn.r && mOn.g > 1.4 * mOn.b)) fail('the sky room model is not lit by the map\'s green sky light');
  if (!(mOff.lum < 0.6 * mOn.lum)) fail('control did not fail: unlit, the sky room model is as bright');

  // content isolation
  const ph = centreColour(r.images.probe), pi = centreColour(iso.images.probeIso);
  const vals = Object.entries(v.end).filter(([k]) => /^fs_conflict\d+$/.test(k)).map(([, x]) => x);
  const probeConflict = vals.find((x) => x.startsWith(`${PROBE} `));
  rows.push(`${build}: probe in oax_hooks (loose map) rgb ${ph.r.toFixed(0)} ${ph.g.toFixed(0)} ${ph.b.toFixed(0)}, in oax_iso (own package) ${pi.r.toFixed(0)} ${pi.g.toFixed(0)} ${pi.b.toFixed(0)}; fs_mappack ${v.end.fs_mappack} / ${iso.valuesAt.iso.fs_mappack}`);
  rows.push(`${build}: fs_conflicts ${v.end.fs_conflicts} (official ${v.end.fs_conflicts_official}); probe: ${probeConflict}`);
  if (!(pi.r > 2 * pi.g)) fail('oax_iso: its own package\'s red probe does not show');
  if (!(ph.g > 2 * ph.r)) fail('control did not fail: oax_hooks does not show the later package\'s green probe');
  if (probeConflict !== `${PROBE} ${PACK_B} ${PACK_A}`) fail(`the conflict report does not name ${PROBE} (${probeConflict})`);
  if (vals.some((x) => x.startsWith(`${SAME} `))) fail(`control did not fail: identical ${SAME} was reported`);
  if (iso.valuesAt.iso.fs_mappack !== `${PACK_A} 1`) fail(`fs_mappack in oax_iso is ${iso.valuesAt.iso.fs_mappack}, not "${PACK_A} 1"`);
  if (v.end.fs_mappack !== '-') fail(`oax_hooks is loose but fs_mappack is ${v.end.fs_mappack}`);
}

const GOLDENS = ['tint', 'nearFade', 'far'];
const GOLDEN_CONTROL = { tint: 'far', nearFade: 'nearNone', far: 'tint' };

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const gdir = path.join(goldens, 'oax');
  fs.mkdirSync(gdir, { recursive: true });

  // picmip null: the engine default (no boot override)
  const cart = await cartShots('oax-hooks', MAP, shots(), { setup: SETUP, out, picmip: null });
  const cartIso = await cartShots('oax-hooks-iso', 'oax_iso', ISO_SHOTS, { setup: SETUP, out, picmip: null });
  checkBuild('cart', cart, cartIso, failures, rows);
  const cartSw = await cartShots('oax-hooks-sw', 'oax_hooks_sw', swShots(), { setup: SW_SETUP, out, picmip: null });
  checkSurfaceWorld('cart', cartSw, failures, rows);

  const native = nativeShots('oax-hooks', MAP, shots(), { setup: SETUP, picmip: null });
  const nativeIso = nativeShots('oax-hooks-iso', 'oax_iso', ISO_SHOTS, { setup: SETUP, picmip: null });
  checkBuild('native', native, nativeIso, failures, rows);
  const nativeSw = nativeShots('oax-hooks-sw', 'oax_hooks_sw', swShots(), { setup: SW_SETUP, picmip: null });
  checkSurfaceWorld('native', nativeSw, failures, rows);

  // placement read-back: native == cart
  for (const k of ['g_place', 'g_place_request']) {
    rows.push(`native vs cart ${k}: ${native.valuesAt.place[k] === cart.valuesAt.place[k] ? 'identical' : 'DIFFER'}`);
    if (native.valuesAt.place[k] !== cart.valuesAt.place[k]) failures.push(`${k} differs: native ${native.valuesAt.place[k]}, cart ${cart.valuesAt.place[k]}`);
  }

  // cart goldens
  for (const g of GOLDENS) {
    const file = path.join(gdir, `hooks_${g}.png`);
    const half = halfSize(cart.images[g]);
    writePng(path.join(out, `oax-hooks_${g}.half.png`), half);
    if (update || !fs.existsSync(file)) {
      writePng(file, half);
      rows.push(`cart golden ${g} ${update ? 'updated' : 'created'}`);
      continue;
    }
    const r = comparePng(readPng(file), half, { tolerance: TOLERANCE, diffPath: path.join(out, `oax-hooks_${g}.diff.png`) });
    const ctl = comparePng(readPng(file), halfSize(cart.images[GOLDEN_CONTROL[g]]), { tolerance: TOLERANCE });
    rows.push(`cart golden ${g}: ${(r.badFraction * 100).toFixed(3)}% over tolerance; control (${GOLDEN_CONTROL[g]} view) ${(ctl.badFraction * 100).toFixed(1)}%`);
    if (!r.sameSize || r.badFraction > MAX_BAD) failures.push(`cart golden ${g}: ${(r.badFraction * 100).toFixed(2)}% differ`);
    if (ctl.badFraction <= MAX_BAD) failures.push(`cart golden ${g}: control did not fail`);
  }
  return { ok: failures.length === 0, failures, rows };
}
