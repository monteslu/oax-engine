// oax_showcase: every step 7.5 feature together on one map authored with
// the engine's own JS tooling (tests/maps/src/oax_showcase.mjs), not an
// importer: PLAN_step7_5 exit criterion 6, "a second, non-UE1 test map
// exercises every feature". Each check runs on the cart AND the native
// build and sits next to a control that must fail.
//
// A. Surface world: every surface loaded (renderer and collision model hash
//    the lump bytes the file holds), no unknown material, validation finds
//    none buried or floating; standing on the hull (exact placement holds
//    z), control oax_showcase_nohull: the player falls, validation reports
//    floating surfaces; an OAX_COLLISION block holds the player, control
//    cm_noCollisionMeshes 1: he drops to the floor; a surface tint and an
//    oaxTint material draw exactly tint x texel (control: two untinted
//    panels measure 1); additive glow never darkens what is behind it
//    (control: the opaque twin's black corners do); a translucent pane
//    shows the wall through it (ids: glass, then the wall with
//    r_oaxSurfaceIdOpaque) and shifts its colour (control: the bare wall is
//    neutral); a two-sided masked grate has holes and shows from both
//    sides, the one-sided twin vanishes from the back, the unmasked twin
//    has no holes; an unlit rgbGen identity material in a bay no light
//    reaches shows its texel (control: the lit material beside it is black).
// B. Light model: q3, doom3 and physical profiles, a light-mask group (the
//    OAX_SURFACES lightMask field), zone ambient and an oax_sin effect at
//    pinned times, on surface-world floors, against the JS oracle
//    (lib/lightoracle.mjs), each with oracle controls that must fail
//    (oax_overbright 2: the physical light past 1x fails with a 1x ceiling);
//    the engine's derived parameters equal the oracle's; zone names drive
//    g_location (control: no zone, "none").
// C. Navigation: native == cart player traces through the noretrigger
//    teleporter pair (2 teleports; control oax_showcase_classic ping-pongs);
//    every authored link reaches the navmesh, routes and mesh hash identical
//    native vs cart, each item routes through its link kind (controls: no
//    translocator kind, hazards excluded); two bots with g_oaxTranslocator 1
//    touch every item on both builds with identical hash checkpoints
//    (control: rule off, the ledge item is unroutable).
// D. Hooks: setviewpos x y z yaw pitch reads back exactly (control: the
//    stock teleport overshoots); cl_oaxFreezeTime freezes the pulsing light
//    at exactly its frozen time (control: unfrozen frames change);
//    r_oaxSurfaceIdAt names the materials under two pixels; detailFade
//    (near: the detail shows, far: the faded panel equals the plain one,
//    control: the always-detail panel); content isolation (the map's own
//    package's red probe wins, the conflict is reported, the identical file
//    is not; control: the loose variant shows the later package's green).
// Native vs cart: every frame compared (mean difference), placement
// read-back, routes, navmesh hash and the bot match.
//
// OA_SHOWCASE_FRAMES (default 6000 = 96 s at 16 ms) sets the bot match length.

import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { cartShots } from '../lib/cartshot.mjs';
import { nativeShots } from '../lib/nativeshot.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';
import { CLEAN_VIEW } from '../lib/scenes.mjs';
import { runNative, nativeHome, findBaseoa, execNative } from '../lib/native.mjs';
import { writePng } from '../lib/png.mjs';
import { meanDiff } from '../lib/imgstat.mjs';
import { parseSurfIdValue, parseSurfIdDump, materialMask, maskStats } from '../lib/surfids.mjs';
import { profile, light } from '../lib/lightoracle.mjs';
import { TRACE_ROWS, TRACE_COLS } from '../lib/movement.mjs';
import { SIM_SCRIPTS } from '../lib/sim-scripts.mjs';
import { surfacesFromBsp, OSF } from '../../../misc/tools/oax-surfaces.mjs';
import { readBspx } from '../../../misc/tools/bspx.mjs';
import { outDir as mapsOut } from '../../maps/build.mjs';
import {
  BAYS, bay, GY, ALBEDO, OVERBRIGHT, WALL_Y, EXHIBIT, GRATES, GLASS, SURF_TINT, MAT_TINT, FLAT, MAT, BLOCK,
  ARENA_ZONE, PROBE, SAME, PACK_A, PACK_B, GLASS_TEXEL, GLOW_TINT, GLOW_PEAK, GLASS_PARK,
} from '../../maps/src/oax_showcase.mjs';

export const name = 'oax-showcase';
// two shot sessions and a bot match on each build, plus short runs
export const timeoutSec = 3600;

const MAP = 'oax_showcase';
const NOHULL = 'oax_showcase_nohull';
const CLASSIC = 'oax_showcase_classic';
const FRAMES = Number(process.env.OA_SHOWCASE_FRAMES || 6000);
const W = 1280, H = 720, CAM = 500, TAN_X = 1, TAN_Y = 720 / 1280;
const T = ALBEDO / 255;
const OK_MEAN = 1.0, OK_MAX = 6;
const FREEZE_MS = 1500;
const n = (x) => Number(x ?? 0);
const nums = (s) => String(s ?? '').trim().split(/\s+/).map(Number);
const near1 = (a, b, eps) => Math.abs(a - b) <= eps;

// linear frames: light 1 is the texel (docs/lights.md, frame contract)
const SETUP = ['r_toneMap 0', 'r_autoExposure 0', 'r_cameraExposure 1', 'r_gamma 1', 'r_fixedShaderTime 0', 'cl_overrideView ""'];
const V = (x, y, z, pitch, yaw) => `cl_overrideView "${x} ${y} ${z} ${pitch} ${yaw} 0"`;

// ---- places -------------------------------------------------------------------------
const STAND = 24.125;     // origin z standing on a floor at 0 (mins 24 + the trace epsilon)
const PLACE = { x: 0, y: -300, z: STAND, yaw: 90, pitch: 15 };
const PARK = bay('park');
const PARK_AT = `${PARK.x - 300} ${GY} ${STAND} 0 0`;
const BLOCK_AT = { x: PARK.x + (BLOCK.x[0] + BLOCK.x[1]) / 2, y: GY, z: BLOCK.z[1] + STAND };
const EX = bay('exhibit');
const LIGHT_BAYS = ['q3', 'doom3', 'custom', 'mask', 'zone', 'effect', 'dark'];
const ORACLE_SHOTS = [
  ...LIGHT_BAYS.map((b) => [b, 0]), ['effect', 0.5], ['custom', 0.5],
].map(([b, time]) => ({ bay: b, time, name: `bay_${b}_${time}` }));
const centre = (k) => EX.x + (EXHIBIT[k][0] + EXHIBIT[k][1]) / 2;
const TINT_CAM = { x: EX.x - 656, y: WALL_Y - 400, z: 160 };
// screen x of a world x on the north wall seen from a camera facing +y
const screenX = (cam, x, wallY = WALL_Y - 0.5) => Math.round(640 + ((x - cam.x) / (wallY - cam.y)) * 640);
const TINT_PIXELS = [[screenX(TINT_CAM, centre('tsurfTint')), 360], [screenX(TINT_CAM, centre('mtintRed')), 360]];
const FAR_CAM = { x: EX.x + 700, y: GY - 450, z: 160 };
const GRATE_X = EX.x - 36;

// [label, goal, include, exclude, expect] (nav-intent's routes, same arena)
const SPAWN = '0 0 8';
const ROUTES = [
  ['tower (teleporter)', '-704 470 256', null, null, 'teleport'],
  ['hazard item', '384 -512 0', null, null, 'walk'],
  ['hazard item, hazards excluded (control)', '384 -512 0', 1 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100, 2, 'none'],
  ['ledge, no translocator (control)', '880 560 192', null, null, 'none'],
  ['ledge with translocator', '880 560 192', 0x11f3, null, 'translocator'],
  ['platform (jump pad)', '0 700 256', null, null, 'jumppad'],
  ['ladder ledge', '-320 -700 160', null, null, 'ladder'],
];
const KINDS = { 0x10: 'teleport', 0x20: 'jumppad', 0x40: 'ladder', 0x80: 'jump', 0x100: 'drop', 0x1000: 'translocator' };
const navPathCmd = ([, goal, inc, exc]) => `nav_path ${SPAWN} ${goal}${inc != null ? ` ${inc}` : ''}${exc != null ? ` ${exc}` : ''}`;

// the main shot list, the same on both builds
function shots() {
  const list = [
    // D: exact placement, then the stock teleport (control); A: the hull holds him
    { cmd: `setviewpos ${PLACE.x} ${PLACE.y} ${PLACE.z} ${PLACE.yaw} ${PLACE.pitch}`, settle: 60, values: 'place' },
    { cmd: `setviewpos ${PLACE.x + 100} ${PLACE.y} 24 ${PLACE.yaw}`, settle: 60, values: 'placeStock' },
    // B: zone names as locations
    { cmd: `setviewpos ${bay('zone').x} ${GY + 200} ${STAND} 0 0`, settle: 30, values: 'locZone' },
    { cmd: `setviewpos ${EX.x} ${GY - 300} ${STAND} 0 0`, settle: 30, values: 'locExhibit' },
    { cmd: `setviewpos ${PARK_AT}`, settle: 30, values: 'locPark' },
    // A: the collision-mesh block holds the player; control: without collision meshes he drops
    { cmd: `setviewpos ${BLOCK_AT.x} ${BLOCK_AT.y} ${BLOCK_AT.z} 0 0`, settle: 60, values: 'block' },
    { cmd: `cm_noCollisionMeshes 1;setviewpos ${BLOCK_AT.x} ${BLOCK_AT.y} ${BLOCK_AT.z} 0 0`, settle: 60, values: 'blockOff' },
    { cmd: `cm_noCollisionMeshes 0;setviewpos ${PARK_AT}`, settle: 30 },
  ];
  // B: the bays straight down, at pinned shader times
  for (const s of ORACLE_SHOTS) list.push({ cmd: `r_fixedShaderTime ${s.time};${V(bay(s.bay).x, GY, CAM, 90, 0)}`, settle: 20, name: s.name });
  // D: one freeze over the pulsing light; control: unfrozen frames change
  const eff = bay('effect');
  list.push(
    { cmd: `r_fixedShaderTime -1;cl_oaxFreezeTime ${FREEZE_MS};${V(eff.x, GY, CAM, 90, 0)}`, settle: 10, name: 'frozenA', values: 'frozen' },
    { settle: 60, name: 'frozenB' },
    { cmd: 'cl_oaxFreezeTime -1', settle: 4, name: 'liveA' },
    { settle: 25, name: 'liveB' },
    { settle: 25, name: 'liveC' },
    { cmd: 'r_fixedShaderTime 0', settle: 4 },
  );
  // A + D: the exhibits (zone ambient 1)
  list.push(
    { cmd: `${V(TINT_CAM.x, TINT_CAM.y, TINT_CAM.z, 0, 90)};r_oaxSurfaceIdAt "${TINT_PIXELS.flat().join(' ')}"`, settle: 10, name: 'tint', values: 'tintIds' },
    { cmd: 'r_oaxSurfaceIdAt "";r_oaxSurfaceIdDump 1', settle: 4, blob: 'tint' },
    { cmd: `${V(EX.x + 64, WALL_Y - 300, 160, 0, 90)};r_oaxSurfaceIdDump 1`, settle: 6, name: 'probe', blob: 'probe', values: 'probe' },
    ...['detailNone', 'detailAlways', 'detailFade'].map((k) => ({ cmd: V(centre(k), WALL_Y - 30, 160, 0, 90), settle: 6, name: `near_${k}` })),
    { cmd: `${V(FAR_CAM.x, FAR_CAM.y, FAR_CAM.z, 0, 90)};r_oaxSurfaceIdDump 1`, settle: 6, name: 'far', blob: 'far' },
    { cmd: 'r_oaxSurfaceIdOpaque 1;r_oaxSurfaceIdDump 1', settle: 4, blob: 'farOpaque' },
    { cmd: `r_oaxSurfaceIdOpaque 0;${V(GRATE_X, GY - 400, 128, 0, 90)};r_oaxSurfaceIdDump 1`, settle: 6, name: 'gratesFront', blob: 'gratesFront' },
    { cmd: `${V(GRATE_X, GY + 400, 128, 0, 270)};r_oaxSurfaceIdDump 1`, settle: 6, name: 'gratesBack', blob: 'gratesBack' },
    // the same pane where no light or ambient reaches (reported, not asserted: the docs do not say)
    { cmd: `${V(PARK.x + (GLASS_PARK.x[0] + GLASS_PARK.x[1]) / 2, WALL_Y - 200, 160, 0, 90)};r_oaxSurfaceIdDump 1`, settle: 6, name: 'parkPane', blob: 'parkPane' },
    { cmd: 'cl_overrideView ""', settle: 2 },
  );
  // C: routes on the navmesh (the level has long settled)
  ROUTES.forEach((r, i) => list.push({ cmd: navPathCmd(r), settle: 4, values: `route${i}` }));
  return list;
}

// the nohull control: the player falls; the loose map shows the later package's probe
function nohullShots() {
  return [
    { cmd: `setviewpos ${PLACE.x} ${PLACE.y} ${PLACE.z} ${PLACE.yaw} ${PLACE.pitch}`, settle: 300, values: 'fall' },
    { cmd: `setviewpos ${PARK_AT}`, settle: 30 },
    { cmd: `${V(EX.x + 64, WALL_Y - 300, 160, 0, 90)};r_oaxSurfaceIdDump 1`, settle: 6, name: 'probe', blob: 'probe', values: 'probe' },
  ];
}

// ---- B: the light oracle --------------------------------------------------------------

// floor point under a pixel (camera straight down: image up is +x, right is -y)
function floorAt(px, py) {
  const nx = 2 * (px + 0.5) / W - 1, ny = 1 - 2 * (py + 0.5) / H;
  return [ny * TAN_Y * CAM, -nx * TAN_X * CAM];
}

// the expected floor colour at bay-local (x, y)
function expect(name, x, y, time, tweak = {}) {
  const b = bay(name);
  if (name === 'dark') {
    // the unlit material (rgbGen identity) shows its texel, the lit grey has nothing
    const unlit = tweak.darkLit ? 0 : ALBEDO;
    return [0, 1, 2].map(() => (y > 0 ? unlit : 0));
  }
  let L = [0, 0, 0];
  if (b.keys) {
    const desc = profile(b.keys);
    if (tweak.desc) tweak.desc(desc);
    const dist = Math.hypot(x, y, b.h);
    const masked = !tweak.ignoreMask && !(desc.mask & (y > 0 && name === 'mask' ? 2 : 1));
    if (!masked) L = light(desc, dist, b.h / dist, { ceiling: tweak.ceiling ?? OVERBRIGHT, time });
  }
  if (b.zone) {
    const amb = tweak.zone !== undefined ? tweak.zone : b.zone.ambient;
    if (amb && y > 0) L = L.map((c, i) => c + Number(String(amb).split(' ')[i]));
  }
  return L.map((c) => Math.min(255, Math.round(255 * T * c)));
}

function oracleCompare(img, name, time, tweak) {
  let sum = 0, max = 0, cnt = 0;
  for (let py = 2; py < H; py += 4) for (let px = 2; px < W; px += 4) {
    const [x, y] = floorAt(px, py);
    if (Math.abs(x) > 490 || Math.abs(y) > 490 || Math.abs(y) < 4) continue;
    const want = expect(name, x, y, time, tweak);
    const o = (py * W + px) * 4;
    for (let c = 0; c < 3; c++) {
      const d = Math.abs(img.data[o + c] - want[c]);
      sum += d; max = Math.max(max, d); cnt++;
    }
  }
  return { mean: sum / cnt, max };
}
const passes = (s) => s.mean <= OK_MEAN && s.max <= OK_MAX;
const fmt = (s) => `mean ${s.mean.toFixed(2)} max ${s.max}`;

const CONTROLS = {
  q3: [['no angular term', { desc: (d) => { d.lambert = false; } }], ['intensity 0.8x', { desc: (d) => { d.intensity *= 0.8; } }]],
  doom3: [['linear falloff', { desc: (d) => { d.falloff = { mode: 'table', points: [[0, 1], [1, 0]] }; } }]],
  custom: [['hard cap', { desc: (d) => { d.knee = 0; } }], ['ceiling 1 (no overbright)', { ceiling: 1 }]],
  mask: [['masks ignored', { ignoreMask: true }]],
  zone: [['no zone ambient', { zone: null }]],
  effect: [['effect at the other time', { otherTime: true }]],
  dark: [['the unlit material lit like the grey (black)', { darkLit: true }]],
};

// r_ulight_phys_lights: "ordinal:profile radius intensity cap knee lambert mask r g b effect;..."
function checkParams(values) {
  const out = [];
  const recs = String(values.r_ulight_phys_lights || '').split(';').filter((r) => r.includes(':'));
  const lit = BAYS.filter((b) => b.keys);
  if (recs.length !== lit.length) out.push(`engine published ${recs.length} physical lights, the map has ${lit.length}`);
  recs.forEach((rec, i) => {
    if (!lit[i]) return;
    const f = rec.split(':')[1].trim().split(/\s+/);
    const d = profile(lit[i].keys);
    // the record's profile is the ULP_* number (tr_ulight.h)
    if (Number(f[0]) !== ['physical', 'ue1', 'q3', 'doom3'].indexOf(d.profile)) out.push(`${lit[i].name}: engine profile ${f[0]}, oracle ${d.profile}`);
    const want = [d.radius, d.intensity, d.cap, d.knee, d.lambert ? 1 : 0, d.mask, ...d.color];
    const got = f.slice(1, 10).map(Number);
    const bad = want.findIndex((w, k) => Math.abs(w - got[k]) > 1e-3 * Math.max(1, Math.abs(w)));
    if (bad >= 0) out.push(`${lit[i].name}: engine parameter ${bad} = ${got[bad]}, oracle ${want[bad]}`);
  });
  return out;
}

// ---- A + D: exhibit measurements -------------------------------------------------------

function centreColour(img, w = 20) {
  let r = 0, g = 0, b = 0, k = 0;
  for (let y = (img.height >> 1) - w; y < (img.height >> 1) + w; y++) {
    for (let x = (img.width >> 1) - w; x < (img.width >> 1) + w; x++) {
      const o = (y * img.width + x) * 4;
      r += img.data[o]; g += img.data[o + 1]; b += img.data[o + 2]; k++;
    }
  }
  return { r: r / k, g: g / k, b: b / k };
}

// min / max luminance over a mask
function lumRange(img, mask) {
  let lo = Infinity, hi = -Infinity;
  for (let i = 0; i < mask.length; i++) {
    if (!mask[i]) continue;
    const o = i * 4;
    const l = 0.299 * img.data[o] + 0.587 * img.data[o + 1] + 0.114 * img.data[o + 2];
    lo = Math.min(lo, l); hi = Math.max(hi, l);
  }
  return { lo, hi };
}

// share of a material's bounding box its own pixels cover (holes lower it)
function coverage(dump, shader) {
  const m = materialMask(dump, shader);
  let x0 = Infinity, y0 = Infinity, x1 = -1, y1 = -1, cnt = 0;
  for (let i = 0; i < m.length; i++) {
    if (!m[i]) continue;
    const x = i % dump.width, y = (i / dump.width) | 0;
    x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); cnt++;
  }
  return cnt ? { px: cnt, cover: cnt / ((x1 - x0 + 1) * (y1 - y0 + 1)) } : { px: 0, cover: 0 };
}

const short = (m) => m.slice('textures/'.length);

function checkBuild(build, r, nh, failures, rows) {
  const fail = (m) => failures.push(`${build}: ${m}`);
  const row = (m) => rows.push(`${build}: ${m}`);
  const v = r.valuesAt;
  const end = r.values;

  // ---- A: loading and validation ----
  const bsp = execFileSync('unzip', ['-p', path.join(mapsOut, 'baseoa', PACK_A), `maps/${MAP}.bsp`], { maxBuffer: 64 << 20 });
  const { surfaces } = surfacesFromBsp(bsp);
  const visible = surfaces.surfaces.filter((s) => !(s.flags & OSF.INVISIBLE)).length;
  const lump = readBspx(bsp).lumps.find((l) => l.name === 'OAX_SURFACES').data;
  let h = 2166136261;
  for (const b of lump) h = Math.imul(h ^ b, 16777619) >>> 0;
  const lumpHash = h.toString(16).padStart(8, '0');
  row(`A loaded ${end.r_surfworld_surfaces} of ${visible} surfaces, ${end.r_surfworld_unknown} unknown materials, hash file ${lumpHash} renderer ${end.r_surfworld_hash} collision ${end.cm_surf_hash}; ` +
    `validation ${end.cm_surf_checked} checked, ${end.cm_surf_buried} buried, ${end.cm_surf_floating} floating; collision meshes ${end.cm_coll_meshes} (${end.cm_coll_tris} tris)`);
  if (n(end.r_surfworld_surfaces) !== visible) fail(`the renderer loaded ${end.r_surfworld_surfaces} surfaces, the lump has ${visible}`);
  if (end.r_surfworld_unknown !== '0') fail(`unknown materials: ${end.r_surfworld_unknown_names}`);
  if (end.r_surfworld_hash !== lumpHash || end.cm_surf_hash !== lumpHash) fail('the renderer or the collision model read different lump bytes');
  if (end.cm_surf_buried !== '0' || end.cm_surf_floating !== '0') fail(`validation flagged surfaces: buried ${end.cm_surf_buried_ids} floating ${end.cm_surf_floating_ids}`);
  if (!(n(end.cm_surf_checked) >= 40)) fail(`only ${end.cm_surf_checked} surfaces checked against the hull`);
  if (end.cm_coll_meshes !== '1' || end.cm_coll_tris !== '12') fail(`collision meshes ${end.cm_coll_meshes} / ${end.cm_coll_tris} tris, want 1 / 12`);
  const nv = nh.valuesAt;
  // the arena floor (surface 0) is the one surface whose hull went down
  row(`A control ${NOHULL}: validation ${nh.values.cm_surf_floating} floating (${nh.values.cm_surf_floating_ids})`);
  if (!String(nh.values.cm_surf_floating_ids || '').split(' ').includes('0:-1')) fail(`control: the arena floor over the lowered hull is not reported floating (${nh.values.cm_surf_floating_ids})`);

  // ---- D + A: exact placement on the hull ----
  const req = nums(v.place.g_place_request), got = nums(v.place.g_place), eye = nums(v.place.cl_view);
  row(`D setviewpos ${PLACE.x} ${PLACE.y} ${PLACE.z} yaw ${PLACE.yaw} pitch ${PLACE.pitch}: g_place ${v.place.g_place} after ${v.place.g_place_frames} frames, cl_view ${v.place.cl_view}`);
  if (got.length < 7 || got[1] !== PLACE.x || got[2] !== PLACE.y || got[3] !== PLACE.z) fail(`read-back origin ${got.slice(1, 4)} != requested ${[PLACE.x, PLACE.y, PLACE.z]} (the hull did not hold him there)`);
  if (!near1(got[4], PLACE.pitch, 360 / 65536) || !near1(got[5], PLACE.yaw, 360 / 65536)) fail(`read-back angles ${got.slice(4, 6)}`);
  if (req[3] !== PLACE.z) fail(`g_place_request ${v.place.g_place_request}`);
  if (!near1(eye[2], PLACE.z + 26, 0.01) || !near1(eye[0], PLACE.x, 0.01)) fail(`rendered eye ${v.place.cl_view} is not the placement + view height`);
  const stock = nums(v.placeStock.g_place);
  const off = Math.hypot(stock[1] - (PLACE.x + 100), stock[2] - PLACE.y, stock[3] - 24);
  row(`D control stock setviewpos: ends ${off.toFixed(1)} units from its spot`);
  if (off < 10) fail(`control did not fail: the stock teleport landed on its spot (${off.toFixed(2)})`);
  const fall = nums(nv.fall.g_place);
  row(`A control ${NOHULL}: placed at z ${PLACE.z}, ${nv.fall.g_place_frames} frames later z ${fall[3]}`);
  if (!(fall[3] < -400)) fail(`control: without the arena hull floor the player did not fall (z ${fall[3]})`);

  // ---- A: the collision-mesh block ----
  const blk = nums(v.block.g_place), off2 = nums(v.blockOff.g_place);
  row(`A OAX_COLLISION block: placed at z ${BLOCK_AT.z}, held at z ${blk[3]}; control cm_noCollisionMeshes 1: z ${off2[3]}`);
  if (!near1(blk[3], BLOCK_AT.z, 0.001)) fail(`the collision mesh did not hold the player (z ${blk[3]})`);
  if (off2[3] !== STAND) fail(`control: without collision meshes the player is at z ${off2[3]}, not on the floor (${STAND})`);

  // ---- B: zone names ----
  const locs = [['locZone', 'Amber Bay'], ['locExhibit', 'Exhibit Hall'], ['place', ARENA_ZONE.name], ['locPark', 'none']];
  row(`B locations: ${locs.map(([k, w]) => `${w} -> ${v[k].g_location}`).join(', ')}`);
  for (const [k, w] of locs) if (v[k].g_location !== w) fail(`location at ${k} is "${v[k].g_location}", want "${w}"`);

  // ---- B: the bays against the oracle ----
  for (const s of ORACLE_SHOTS) {
    if (s.bay === 'custom' && s.time) continue;   // the steady control below
    const img = r.images[s.name];
    if (!img) { fail(`no screenshot ${s.name}`); continue; }
    const res = oracleCompare(img, s.bay, s.time);
    const ctl = (CONTROLS[s.bay] || []).map(([what, tweak]) => {
      const c = oracleCompare(img, s.bay, tweak.otherTime ? 0.5 - s.time : s.time, tweak);
      if (passes(c)) fail(`${s.bay} t=${s.time}: control (${what}) matched too (${fmt(c)})`);
      return `${what}: ${fmt(c)}`;
    });
    row(`B ${s.bay}${s.time ? ` t=${s.time}` : ''}: vs oracle ${fmt(res)}; controls ${ctl.join(', ')}`);
    if (!passes(res)) fail(`${s.bay} t=${s.time}: off the oracle (${fmt(res)})`);
  }
  const frameMax = (a, b) => { let m = 0; for (let k = 0; k < a.data.length; k++) m = Math.max(m, Math.abs(a.data[k] - b.data[k])); return m; };
  const steady = frameMax(r.images.bay_custom_0, r.images['bay_custom_0.5']), pulse = frameMax(r.images.bay_effect_0, r.images['bay_effect_0.5']);
  row(`B effect: steady light t=0 vs 0.5 max diff ${steady}; pulse t=0 vs 0.5 max diff ${pulse}`);
  if (steady !== 0) fail(`the steady light changed with time (${steady})`);
  if (pulse < 20) fail(`the pulse did not change with time (${pulse})`);
  const pf = checkParams(end);
  row(`B parameters: r_ulight_phys "${end.r_ulight_phys}"; ${pf.length ? pf.join('; ') : 'every derived parameter equals the oracle'}`);
  pf.forEach((m) => fail(m));
  const zonesWithAmbient = BAYS.filter((b) => b.zone && b.zone.ambient).length + 1;
  const physLights = BAYS.filter((b) => b.keys).length;
  if (!String(end.r_ulight_phys || '').startsWith(`${physLights} ${OVERBRIGHT.toFixed(3)} ${zonesWithAmbient}`)) fail(`r_ulight_phys "${end.r_ulight_phys}", want ${physLights} lights, overbright ${OVERBRIGHT}, ${zonesWithAmbient} ambient zones`);
  row(`B light masks: ${end.r_surfworld_mask_excluded} surface/light pairs kept apart, ${end.r_surfworld_mask_violations} violations; zone ambient on ${end.r_surfworld_zone_ambient} surfaces`);
  if (!(n(end.r_surfworld_mask_excluded) > 0)) fail('light-mask groups kept no light off a surface');
  if (end.r_surfworld_mask_violations !== '0') fail(`light-mask violations: ${end.r_surfworld_mask_violations}`);

  // ---- D: the freeze ----
  const same = frameMax(r.images.frozenA, r.images.frozenB);
  const live = Math.max(frameMax(r.images.liveA, r.images.liveB), frameMax(r.images.liveB, r.images.liveC), frameMax(r.images.liveA, r.images.liveC));
  const fz = oracleCompare(r.images.frozenA, 'effect', FREEZE_MS / 1000), fz0 = oracleCompare(r.images.frozenA, 'effect', 0);
  row(`D cl_oaxFreezeTime ${FREEZE_MS}: frames 60 apart max diff ${same} (cl_view_time ${v.frozen.cl_view_time}); pulse vs oracle at ${FREEZE_MS / 1000}s ${fmt(fz)}, at 0s ${fmt(fz0)}; control unfrozen max diff ${live}`);
  if (same !== 0) fail(`frozen frames differ (${same})`);
  if (v.frozen.cl_view_time !== String(FREEZE_MS)) fail(`frozen scene time ${v.frozen.cl_view_time}, not ${FREEZE_MS}`);
  if (!passes(fz)) fail(`the frozen pulse is not the oracle at ${FREEZE_MS / 1000}s (${fmt(fz)})`);
  if (passes(fz0)) fail('control did not fail: the frozen pulse also matches time 0');
  if (live < 20) fail(`control did not fail: unfrozen frames match (${live})`);

  // ---- D: surface ids at pixels ----
  const a = parseSurfIdValue(v.tintIds.r_surfid0), b = parseSurfIdValue(v.tintIds.r_surfid1);
  row(`D r_oaxSurfaceIdAt ${TINT_PIXELS.map((p) => p.join(',')).join(' ')}: ${a.kind} ${a.shader} / ${b.kind} ${b.shader}`);
  if (a.shader !== MAT.tsurfTint || a.kind !== 'world') fail(`pixel ${TINT_PIXELS[0]} is not ${MAT.tsurfTint} (${v.tintIds.r_surfid0})`);
  if (b.shader !== MAT.mtintRed) fail(`pixel ${TINT_PIXELS[1]} is not ${MAT.mtintRed} (${v.tintIds.r_surfid1})`);
  if (a.id === b.id) fail('control did not fail: two pixels name the same surface');

  // ---- A + D: tints (zone ambient 1: a pixel is texel x tint) ----
  const td = parseSurfIdDump(r.blobs.tint);
  const st = (img, dump, m) => maskStats(img, materialMask(dump, short(m)));
  const sp = st(r.images.tint, td, MAT.tsurfPlain), stn = st(r.images.tint, td, MAT.tsurfTint);
  const mp = st(r.images.tint, td, MAT.mtintPlain), mr = st(r.images.tint, td, MAT.mtintRed);
  const rgb = (s) => [s.r, s.g, s.b];
  const ratio = (x, y) => rgb(x).map((c, i) => c / rgb(y)[i]);
  const rs = ratio(stn, sp), rm = ratio(mr, mp), rc = ratio(mp, sp);
  const f3 = (a3) => a3.map((x) => x.toFixed(3)).join(' ');
  row(`A surface tint ${SURF_TINT.join(' ')}: tinted/plain ${f3(rs)}; D oaxTint ${MAT_TINT.join(' ')}: ${f3(rm)}; control untinted/untinted ${f3(rc)}; plain panel ${f3(rgb(sp))} (texel ${FLAT.panel})`);
  if (!sp.n || !stn.n || !mp.n || !mr.n) fail('a tint panel has no pixels in the dump');
  rs.forEach((x, i) => { if (!near1(x, SURF_TINT[i], 0.02)) fail(`surface tint channel ${i}: ${x.toFixed(3)}, want ${SURF_TINT[i]}`); });
  rm.forEach((x, i) => { if (!near1(x, MAT_TINT[i], 0.02)) fail(`oaxTint channel ${i}: ${x.toFixed(3)}, want ${MAT_TINT[i]}`); });
  if (rc.some((x) => !near1(x, 1, 0.02))) fail(`control: two untinted panels measure ${f3(rc)}`);
  if (rgb(sp).some((c) => !near1(c, FLAT.panel, 2))) fail(`the plain panel draws ${f3(rgb(sp))}, not its texel ${FLAT.panel} under ambient 1`);

  // ---- D: content isolation ----
  const pd = parseSurfIdDump(r.blobs.probe), pn = parseSurfIdDump(nh.blobs.probe);
  const probe = st(r.images.probe, pd, MAT.probe), probeLoose = st(nh.images.probe, pn, MAT.probe);
  const confl = Object.entries(v.probe).filter(([k]) => /^fs_conflict\d+$/.test(k)).map(([, x]) => x);
  const probeConflict = confl.find((x) => x.startsWith(`${PROBE} `));
  row(`D content isolation: probe in ${MAP} (own package) rgb ${f3(rgb(probe))}, in ${NOHULL} (loose) ${f3(rgb(probeLoose))}; fs_mappack ${v.probe.fs_mappack} / ${nh.valuesAt.probe.fs_mappack}; conflict "${probeConflict}"`);
  if (!(probe.n && probe.r > 2 * probe.g)) fail('the map package\'s red probe does not show');
  if (!(probeLoose.n && probeLoose.g > 2 * probeLoose.r)) fail('control did not fail: the loose map does not show the later package\'s green probe');
  if (probeConflict !== `${PROBE} ${PACK_B} ${PACK_A}`) fail(`the conflict report does not name ${PROBE} (${probeConflict})`);
  if (confl.some((x) => x.startsWith(`${SAME} `))) fail(`control did not fail: identical ${SAME} was reported`);
  if (v.probe.fs_mappack !== `${PACK_A} 1`) fail(`fs_mappack ${v.probe.fs_mappack}, not "${PACK_A} 1"`);
  if (nh.valuesAt.probe.fs_mappack !== '-') fail(`${NOHULL} is loose but fs_mappack is ${nh.valuesAt.probe.fs_mappack}`);

  // ---- A: additive glow ----
  const wall = st(r.images.probe, pd, MAT.exwall);
  const gm = materialMask(pd, short(MAT.glow)), om = materialMask(pd, short(MAT.glowOpaque));
  const ga = lumRange(r.images.probe, gm), go = lumRange(r.images.probe, om);
  // the brightest pixel per channel: the wall plus tint x the centre texel
  const peak = [0, 1, 2].map((c) => { let m = 0; for (let i = 0; i < gm.length; i++) if (gm[i]) m = Math.max(m, r.images.probe.data[i * 4 + c]); return m; });
  const peakWant = GLOW_PEAK.map((t, c) => rgb(wall)[c] + GLOW_TINT * t);
  row(`A additive glow (tint ${GLOW_TINT}) over a wall of ${wall.lum.toFixed(1)}: lum ${ga.lo.toFixed(1)}..${ga.hi.toFixed(1)}, peak ${peak.join(' ')} (expected wall + tint x texel ${f3(peakWant)}); control opaque twin ${go.lo.toFixed(1)}..${go.hi.toFixed(1)}`);
  if (!(ga.lo >= wall.lum - 1)) fail('the additive glow darkens the wall');
  if (peak.some((c, i) => !near1(c, peakWant[i], 4))) fail(`the additive glow peaks at ${peak.join(' ')}, not wall + tint x texel ${f3(peakWant)}`);
  if (!(go.lo <= wall.lum - 60)) fail('control did not fail: the opaque glow does not hide the wall');

  // ---- D: detailFade ----
  const nearC = Object.fromEntries(['detailNone', 'detailAlways', 'detailFade'].map((k) => { const c = centreColour(r.images[`near_${k}`]); return [k, c.g / Math.max(1, c.r)]; }));
  const fd = parseSurfIdDump(r.blobs.far);
  const dn = st(r.images.far, fd, MAT.detailNone), dfa = st(r.images.far, fd, MAT.detailFade), dal = st(r.images.far, fd, MAT.detailAlways);
  const gr = (x) => x.g / Math.max(1, x.r);
  row(`D detailFade 0 380: near (30) g/r none ${nearC.detailNone.toFixed(2)}, always ${nearC.detailAlways.toFixed(2)}, fade ${nearC.detailFade.toFixed(2)}; far (~960) g/r none ${gr(dn).toFixed(3)}, fade ${gr(dfa).toFixed(3)}, control always ${gr(dal).toFixed(3)}`);
  if (!(nearC.detailFade > 4 && nearC.detailAlways > 4)) fail('near: the detail stage does not show');
  if (!(nearC.detailNone < 1.5)) fail('control did not fail: near, the plain panel is green');
  if (!dn.n || !dfa.n || !dal.n) fail('a detail panel has no pixels in the far dump');
  if (!near1(gr(dfa), gr(dn), 0.03) || !near1(dfa.lum, dn.lum, 2)) fail('far: the faded panel still shows the detail stage');
  if (!(gr(dal) > 2 * gr(dn))) fail('control did not fail: far, the always-detail panel looks plain');

  // ---- A: the translucent pane ----
  const fo = parseSurfIdDump(r.blobs.farOpaque);
  const gmask = materialMask(fd, short(MAT.glass));
  let behind = 0, gpx = 0;
  const wallIds = new Set([...fo.surfs].filter(([, s]) => s.shader === MAT.exwall).map(([id]) => id));
  for (let i = 0; i < gmask.length; i++) if (gmask[i]) { gpx++; if (wallIds.has(fo.ids[i])) behind++; }
  const gs = maskStats(r.images.far, gmask), ws = st(r.images.far, fd, MAT.exwall);
  const br = (x) => x.b / Math.max(1, x.r);
  row(`A translucent pane: ${gpx} px, ${(100 * behind / Math.max(1, gpx)).toFixed(1)}% name the wall with r_oaxSurfaceIdOpaque 1; b/r through the pane ${br(gs).toFixed(3)} (rgb ${f3(rgb(gs))}), control bare wall ${br(ws).toFixed(3)}`);
  if (!(gpx > 500)) fail(`the pane has only ${gpx} pixels`);
  if (!(behind / Math.max(1, gpx) > 0.95)) fail('with r_oaxSurfaceIdOpaque 1 the pane\'s pixels do not name the wall behind it');
  // docs/map-format.md: opacity tint[3], colour texel x tint, unlit (ambient 1 here: the same)
  const alpha = GLASS.tint[3];
  const want = GLASS_TEXEL.map((t, i) => alpha * t * GLASS.tint[i] + (1 - alpha) * rgb(ws)[i]);
  row(`A translucent pane: expected a * texel * tint + (1 - a) * wall = ${f3(want)}, drawn ${f3(rgb(gs))}`);
  if (rgb(gs).some((c, i) => !near1(c, want[i], 3))) fail(`the pane draws ${f3(rgb(gs))}, not the documented blend ${f3(want)}`);
  if (!(br(gs) > 1.05)) fail('the pane does not tint what is behind it');
  if (!near1(br(ws), 1, 0.02)) fail(`control: the bare wall is not neutral (b/r ${br(ws).toFixed(3)})`);
  if (!(gs.lum > 0.3 * ws.lum)) fail('the pane hides the wall (it is not translucent)');
  {
    const pd2 = parseSurfIdDump(r.blobs.parkPane);
    const pp = st(r.images.parkPane, pd2, MAT.glass), pw = st(r.images.parkPane, pd2, MAT.hallwall);
    row(`A translucent pane in the park bay (no light, no ambient): rgb ${f3(rgb(pp))} (${pp.n} px) over a wall of ${f3(rgb(pw))}`);
  }

  // ---- A: grates (two-sided masked, one-sided masked, unmasked) ----
  const gf = parseSurfIdDump(r.blobs.gratesFront), gb = parseSurfIdDump(r.blobs.gratesBack);
  const cov = (d) => Object.fromEntries([MAT.grate2s, MAT.grate1s, MAT.grateOpaque].map((m) => [m, coverage(d, short(m))]));
  const cf = cov(gf), cb = cov(gb);
  const c3 = (c) => [MAT.grate2s, MAT.grate1s, MAT.grateOpaque].map((m) => `${short(m).split('/')[1]} ${c[m].cover.toFixed(3)} (${c[m].px} px)`).join(', ');
  row(`A grates, front: ${c3(cf)}; back: ${c3(cb)}`);
  for (const [side, c] of [['front', cf], ['back', cb]]) {
    if (!(c[MAT.grate2s].cover > 0.3 && c[MAT.grate2s].cover < 0.65)) fail(`${side}: the two-sided masked grate covers ${c[MAT.grate2s].cover.toFixed(3)} of its box (holes expected)`);
    if (!(c[MAT.grateOpaque].cover > 0.97)) fail(`control did not fail (${side}): the unmasked grate covers only ${c[MAT.grateOpaque].cover.toFixed(3)}`);
  }
  if (!(cf[MAT.grate1s].cover > 0.3)) fail('front: the one-sided grate does not show');
  if (cb[MAT.grate1s].px !== 0) fail(`control did not fail: the one-sided grate shows from the back (${cb[MAT.grate1s].px} px)`);
  if (!near1(cf[MAT.grate2s].cover, cb[MAT.grate2s].cover, 0.08)) fail('the two-sided grate differs between its sides');

  // ---- C: routes ----
  const routes = [];
  ROUTES.forEach((rt, i) => {
    const rv = v[`route${i}`] || {};
    const p = nums(rv.nav_path);
    const partial = !!((p[1] || 0) & 1);
    const linksIdx = rv.nav_path_links && rv.nav_path_links !== '-' ? rv.nav_path_links.split(' ') : [];
    const kinds = linksIdx.map((k) => String(rv[`g_nav_link_${k}`] || '?').split(' ')[0]);
    const kind = !p[0] || partial ? 'none' : (kinds[0] || 'walk');
    routes.push(`${rv.nav_path} | ${rv.nav_path_links}`);
    row(`C route ${rt[0]}: ${p[0] || 0} points${partial ? ' (partial)' : ''}, links [${kinds.join(', ')}] -> ${kind}`);
    if (kind !== rt[4]) fail(`route ${rt[0]}: expected ${rt[4]}, got ${kind}`);
  });
  const lv = v[`route${ROUTES.length - 1}`];
  const authored = Object.keys(lv).filter((k) => /^g_nav_link_\d+$/.test(k)).map((k) => lv[k].split(' ')[0]);
  const [ok, total] = String(lv.sv_nav_links).split('/').map(Number);
  row(`C navmesh ${lv.sv_nav_polys} polys, hash ${lv.sv_nav_hash}, links ${lv.sv_nav_links} (${authored.join(' ')}), open: ${lv.sv_nav_links_open}, hazards ${lv.g_nav_hazards}`);
  if (!(total >= 8 && ok === total)) fail(`links: ${lv.sv_nav_links} connected (open: ${lv.sv_nav_links_open})`);
  for (const k of ['teleport', 'jumppad', 'ladder', 'translocator', 'drop']) if (!authored.includes(k)) fail(`links: no ${k} link authored`);
  return { routes, navHash: lv.sv_nav_hash };
}

// ---- C: teleporter traces and bots (nav-intent's harness) -------------------------------

const TELE = { x: -704, y: -622.97, z: STAND, yaw: 90 };
const TOWER_TOP = 256;

function nativeRows(file) {
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  const start = lines.find((l) => l.startsWith('# start '));
  return {
    start: start ? Number(start.split(' ')[2]) : null,
    rows: lines.filter((l) => !l.startsWith('#')).map((l) => l.split(' ').map((x, k) => (k >= 1 && k <= 6 ? Math.fround(Number(x)) : Number(x)))),
  };
}

function fromStart(rows, t0) {
  if (t0 == null || t0 < 0) return null;
  const xs = rows.filter((r) => r[0] >= t0);
  if (!xs.length || !xs.some((r) => Math.hypot(r[4], r[5]) > 0.5 || Math.hypot(r[1] - xs[0][1], r[2] - xs[0][2]) > 0.5)) return null;
  const out = new Map();
  for (const r of rows) if (r[0] >= t0 && !out.has(r[0] - t0)) out.set(r[0] - t0, r);
  return out;
}

function compareTraces(a, b) {
  let common = 0, firstDiff = null, maxErr = 0;
  for (const [dt, x] of a) {
    const y = b.get(dt);
    if (!y) continue;
    common++;
    for (let k = 1; k <= 7; k++) {
      const e = Math.abs(x[k] - y[k]);
      if (e > maxErr) maxErr = e;
      if (e > 0 && !firstDiff) firstDiff = `t+${dt}ms col ${k}: native ${x[k]} vs cart ${y[k]}`;
    }
  }
  return { common, firstDiff, maxErr };
}

async function cartScript(label, map, script, at) {
  const s = new Session(label);
  try {
    await s.load(undefined, 1, { picmip: null });
    await s.command(`${CLEAN_VIEW};g_doWarmup 0;timelimit 0;fraglimit 0;set sv_gameSeed 7;devmap ${map}`);
    await s.stepUntil('conn_state', (x) => x === CA_ACTIVE, 4000, 20);
    await s.command(CLEAN_VIEW);
    await s.step(60);
    await s.command(`setviewpos ${at.x} ${at.y} ${at.z} ${at.yaw} 0`);
    await s.step(60);
    const v0 = await readValues(s);
    const before = await s.read('trace_count');
    await s.command(`padscript padscripts/${script}.pad`);
    await s.step(SIM_SCRIPTS[script].reduce((k, st) => k + st.frames, 0) + 30);
    const after = await s.read('trace_count');
    if (after - before <= 0 || after - before > TRACE_ROWS) throw new Error(`${label}: ${after - before} trace rows`);
    const flat = await s.read('trace');
    const rows = [];
    for (let i = before; i < after; i++) {
      const o = (i % TRACE_ROWS) * TRACE_COLS;
      const r = flat.slice(o, o + TRACE_COLS);
      rows.push([r[8], ...[r[1], r[2], r[3], r[4], r[5], r[6]].map(Math.fround), r[7]]);
    }
    return { rows: fromStart(rows, await s.read('pad_start_time')), v0, v: await readValues(s) };
  } finally {
    await s.shutdown();
  }
}

function nativeScript(label, map, script, at) {
  const file = `${label}.txt`;
  const home = runNative(label, map, [
    'exec padscripts/padbinds.cfg', 'bot_enable 0', 'wait 120',
    `setviewpos ${at.x} ${at.y} ${at.z} ${at.yaw} 0`, 'wait 200',
    'debugvalues before.txt',
    `trace ${file}`,
    `padscript padscripts/${script}.pad "wait 8; trace stop; debugvalues after.txt; quit"`,
  ], { quit: false, picmip: null });
  const tfile = path.join(home, 'baseoa', file);
  if (!fs.existsSync(tfile)) throw new Error(`native ${label}: no trace written (see ${home}/native.log)`);
  const nr = nativeRows(tfile);
  const read = (f) => parseDebugValues(fs.readFileSync(path.join(home, 'baseoa', f), 'utf8'));
  return { rows: fromStart(nr.rows, nr.start), v0: read('before.txt'), v: read('after.txt') };
}

// the local player watches as a spectator (nav-intent: a player in the
// match is shot at, and his commands follow each build's own timing)
function matchLine(tl) {
  return ['setu team s', 'set sv_gameSeed 7', `set g_oaxTranslocator ${tl}`, 'bot_enable 1', 'g_gametype 0', 'g_doWarmup 0', 'timelimit 0', 'fraglimit 0',
    `devmap ${MAP}`, 'addbot Sarge 3', 'addbot Grism 3'].join(';');
}

const checkpoints = (v) => Object.fromEntries(Object.entries(v).filter(([k]) => /^g_navbot_hash_\d+$/.test(k)).map(([k, x]) => [Number(k.split('_').pop()), x]));

async function cartMatch(label, tl, frames) {
  const s = new Session(label);
  try {
    await s.load(undefined, 1, { picmip: null });
    await s.command(matchLine(tl));
    await s.stepUntil('conn_state', (x) => x === CA_ACTIVE, 4000, 20);
    for (let done = 0; done < frames; done += 750) await s.step(Math.min(750, frames - done));
    return await readValues(s);
  } finally {
    await s.shutdown();
  }
}

function nativeMatch(tl, frames) {
  const home = nativeHome('oax-showcase-match');
  fs.writeFileSync(path.join(home, 'baseoa', 'showcase.cfg'), ['fixedtime 16', matchLine(tl), `wait ${frames * 2}`, 'debugvalues showcase_native.txt', 'quit'].join('\n') + '\n');
  execNative([
    '+set', 'fs_basepath', path.dirname(findBaseoa()), '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
    '+set', 'r_mode', '-1', '+set', 'r_customwidth', '640', '+set', 'r_customheight', '360', '+set', 'r_fullscreen', '0',
    '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0',
    '+set', 'com_introplayed', '1', '+set', 'com_maxfps', '0', '+set', 'fixedtime', '16', '+exec', 'showcase.cfg',
  ], { home, timeout: 1200000 });
  return parseDebugValues(fs.readFileSync(path.join(home, 'baseoa', 'showcase_native.txt'), 'utf8'));
}

export async function run({ out }) {
  const failures = [];
  const rows = [];

  // ---- the shot sessions (A, B, D; C routes) ----
  const cart = await cartShots('oax-showcase', MAP, shots(), { setup: SETUP, out, picmip: null });
  const cartNh = await cartShots('oax-showcase-nohull', NOHULL, nohullShots(), { setup: SETUP, out, picmip: null });
  const cr = checkBuild('cart', cart, cartNh, failures, rows);
  // fail fast: the native runs and the bot matches take many minutes
  if (failures.length && !process.env.OA_SHOWCASE_ALL) {
    rows.push('stopped after the cart shot checks failed (OA_SHOWCASE_ALL=1 runs everything)');
    return { ok: false, failures, rows };
  }
  const nat = nativeShots('oax-showcase', MAP, shots(), { setup: SETUP, picmip: null });
  const natNh = nativeShots('oax-showcase-nohull', NOHULL, nohullShots(), { setup: SETUP, picmip: null });
  const nr = checkBuild('native', nat, natNh, failures, rows);

  // native vs cart: every frame, the placement, the routes, the navmesh
  const diffs = [];
  for (const [k, img] of Object.entries(cart.images)) {
    // the unfrozen frames are the freeze's control: their scene time depends on each build's frame pacing
    if (/^live/.test(k)) continue;
    const b = nat.images[k];
    if (!b) { failures.push(`native wrote no ${k} screenshot`); continue; }
    writePng(path.join(out, `oax-showcase_${k}.native.png`), b);
    diffs.push([k, meanDiff(img, b)]);
  }
  diffs.sort((a, b) => b[1] - a[1]);
  rows.push(`native vs cart: ${diffs.length} frames, worst mean difference ${diffs.slice(0, 4).map(([k, d]) => `${k} ${d.toFixed(2)}`).join(', ')}`);
  for (const [k, d] of diffs) if (d > 2) failures.push(`native vs cart: ${k} differs (mean ${d.toFixed(2)})`);
  for (const k of ['g_place', 'g_place_request']) {
    if (nat.valuesAt.place[k] !== cart.valuesAt.place[k]) failures.push(`${k} differs: native ${nat.valuesAt.place[k]}, cart ${cart.valuesAt.place[k]}`);
  }
  const sameRoutes = cr.routes.every((x, i) => x === nr.routes[i]);
  rows.push(`native vs cart: placement ${nat.valuesAt.place.g_place === cart.valuesAt.place.g_place ? 'identical' : 'DIFFERENT'}, navmesh hash ${nr.navHash} / ${cr.navHash}, ${ROUTES.length} routes ${sameRoutes ? 'identical' : 'DIFFERENT'}`);
  if (nr.navHash !== cr.navHash) failures.push(`navmesh hash native ${nr.navHash} vs cart ${cr.navHash}`);
  if (!sameRoutes) failures.push('routes differ native vs cart');

  // ---- C: teleporter traces ----
  {
    const natT = nativeScript('showtele', MAP, 'nav_tele', TELE);
    const cartT = await cartScript('oax-showcase-tele', MAP, 'nav_tele', TELE);
    if (!natT.rows || !cartT.rows) {
      failures.push(`teleporter: no movement in the ${!natT.rows ? 'native' : 'cart'} trace`);
    } else {
      const { common, firstDiff, maxErr } = compareTraces(natT.rows, cartT.rows);
      const ports = n(cartT.v.g_teleports_0) - n(cartT.v0.g_teleports_0), nports = n(natT.v.g_teleports_0) - n(natT.v0.g_teleports_0);
      const onTower = [...cartT.rows.values()].filter((r) => r[3] > TOWER_TOP + 20).length;
      const last = [...cartT.rows.values()].pop();
      rows.push(`C teleporter pair (noretrigger): ${common} common command times (native ${natT.rows.size}, cart ${cartT.rows.size}), max difference ${maxErr}; teleports cart ${ports} native ${nports}; ${onTower} rows on the tower; ends at ${last[1].toFixed(1)} ${last[2].toFixed(1)} ${last[3].toFixed(1)}`);
      if (common < natT.rows.size || common < 100) failures.push(`teleporter: only ${common} of native's ${natT.rows.size} command times on the cart`);
      if (firstDiff) failures.push(`teleporter: native and cart differ at ${firstDiff}`);
      if (ports !== 2 || nports !== 2) failures.push(`teleporter: expected exactly 2 teleports, cart ${ports} native ${nports}`);
      if (!(onTower > 20)) failures.push(`teleporter: the player never stood on the tower (${onTower} rows)`);
    }
    const ctl = await cartScript('oax-showcase-classic', CLASSIC, 'nav_tele', TELE);
    const cports = n(ctl.v.g_teleports_0) - n(ctl.v0.g_teleports_0);
    rows.push(`C control classic teleporters (${CLASSIC}): ${cports} teleports in the same script; pingpong links left out ${ctl.v.g_nav_links_pingpong}`);
    if (!(cports > 10)) failures.push(`control: classic teleporters did not ping-pong (${cports} teleports)`);
  }

  // ---- C: bots ----
  {
    const v = await cartMatch('oax-showcase-bots', 1, FRAMES);
    const nv = nativeMatch(1, FRAMES);
    fs.writeFileSync(path.join(out, 'oax-showcase-bots.json'), JSON.stringify({ cart: v, native: nv }, null, 1));
    for (const [label, x] of [['cart', v], ['native', nv]]) {
      const kinds = Object.entries(KINDS).filter(([bit]) => n(x.g_navbot_link_kinds) & Number(bit)).map(([, k]) => k);
      rows.push(`C bots ${label}, g_oaxTranslocator 1, ${n(x.g_level_time) / 1000}s: items touched ${x.g_items_reached}/${x.g_items_total}, routable ${x.g_items_routable}/${x.g_items_routed_total}; links taken ${x.g_navbot_links_taken} done ${x.g_navbot_links_done} failed ${x.g_navbot_links_failed}; kinds done: ${kinds.join(' ')}`);
      if (!(n(x.g_items_total) === 5 && x.g_items_reached === x.g_items_total)) failures.push(`bots ${label}: items touched ${x.g_items_reached}/${x.g_items_total} (missing ${x.g_items_missing})`);
      if (n(x.g_items_routable) !== n(x.g_items_routed_total)) failures.push(`bots ${label}: unroutable items ${x.g_items_unroutable}`);
      for (const k of ['teleport', 'jumppad', 'ladder', 'drop', 'translocator']) if (!kinds.includes(k)) failures.push(`bots ${label}: no ${k} link completed`);
    }
    const cc = checkpoints(v), nc = checkpoints(nv);
    const common = Object.keys(cc).filter((t) => nc[t] !== undefined).map(Number).sort((a, b) => a - b);
    const diff = common.filter((t) => cc[t] !== nc[t]);
    rows.push(`C bots native vs cart: ${common.length} hash checkpoints in common, ${diff.length} differ${diff.length ? ` (first at ${diff[0]}s)` : ''}`);
    if (common.length < 5) failures.push(`bots: only ${common.length} checkpoints in common`);
    if (diff.length) failures.push(`bots: the match differs native vs cart from ${diff[0]}s`);
    if (new Set(common.map((t) => cc[t])).size < common.length - 1) failures.push('control: the bot hash did not move');

    const off = await cartMatch('oax-showcase-bots-off', 0, 600);
    rows.push(`C control g_oaxTranslocator 0: routable ${off.g_items_routable}/${off.g_items_routed_total} (unroutable ${off.g_items_unroutable}); throws ${off.g_tl_throws ?? 0}`);
    if (!String(off.g_items_unroutable || '').includes('weapon_railgun')) failures.push('control: the ledge item was routable with the translocator rule off');
    if (n(off.g_items_routable) !== 4) failures.push(`control: ${off.g_items_routable} items routable with the rule off, want 4`);
  }
  return { ok: failures.length === 0, failures, rows };
}
