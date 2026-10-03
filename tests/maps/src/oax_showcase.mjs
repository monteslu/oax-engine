// oax_showcase: every step 7.5 feature in one map, authored with the
// engine's own JS tooling (mapwriter + misc/tools/oax-surfaces.mjs), not by
// an importer, so none of the features is UE1-only (PLAN_step7_5 exit
// criterion 6). tests/romdev/tests/oax-showcase.mjs checks each feature on
// the cart and the native build.
//
// Two sealed halls. Everything the player sees is OAX_SURFACES surfaces of
// zero thickness over a hull of caulk brushes (collision, PVS, areas,
// navmesh); the map is unified-lit (no q3map2 -light), oax_overbright 2,
// with no world ambient: light comes from lamps and zone ambients.
//
// The arena (x -1024..1024, y -768..768), the oax_nav_intent layout as
// surfaces over a hull, for navigation from intent:
//   tower        reached only by a noretrigger teleporter pair (each
//                arrival inside the partner trigger: no-retrigger arrival)
//   hazard       a trigger_hurt over a health item (a cost volume)
//   ledge        reachable only by translocator (info_oax_route kind
//                translocator, g_oaxTranslocator); a drop route back
//   platform     a jump pad (additive glow surface on it); a drop route back
//   ladder ledge a func_oax_zone "ladder" volume (masked rungs surface)
//   a func_oax_zone over the whole arena: ambient and the name "Showcase Arena"
//
// The gallery (y around GY), one area, a bay every BAY units:
//   q3 doom3 custom   one physical light each, profiles q3 / doom3 /
//                     physical (table falloff, soft cap, no angular term,
//                     colour over 1, past 1x: oax_overbright 2)
//   mask              a light in light-mask group 2 over a floor whose y > GY
//                     half is in group 2 by the OAX_SURFACES lightMask field
//   zone              no light: a func_oax_zone with ambient and a name over
//                     the y > GY half
//   effect            an oax_sin pulse
//   dark              no light, no ambient: the y > GY floor half is an unlit
//                     (rgbGen identity) material, the other half lit grey
//   exhibit           a zone with ambient 1 1 1 ("Exhibit Hall"): surface
//                     tint and oaxTint pairs, the content isolation probe,
//                     additive and opaque glow panels, detailFade panels, a
//                     translucent pane, two-sided / one-sided / unmasked grates
//   park              where the player waits; a block that exists only as an
//                     OAX_COLLISION mesh (with a render mesh over it)
//
// Content isolation: the map is packed in its own package
// zzz_oaxshow_a.pk3 with a RED probe texture; zzz_oaxshow_b.pk3 (loaded
// later) has a GREEN copy of the same path and an identical copy of
// same.tga. The variant oax_showcase_nohull (loose, arena hull floor 512
// lower) is the control: it shows the green probe, the player falls;
// oax_showcase_classic (loose, no noretrigger) is the teleporter control.

import { MapFile, box } from '../mapwriter.mjs';
import { SurfaceWorld, CollisionMeshes, OSF, uvMatrix } from '../../../misc/tools/oax-surfaces.mjs';
import { tga, image, hash2, tiles, bricks, normalMap } from '../lib/texgen.mjs';
import { encodeTga } from '../../romdev/lib/tga.mjs';

const T = 'textures/oax_show';
const CAULK = 'common/caulk';
const TH = 16;

export const MAT = {
  floor: `${T}/floor`, wall: `${T}/wall`, ceil: `${T}/ceil`, stone: `${T}/stone`, rungs: `${T}/rungs`, pad: `${T}/pad`,
  grey: `${T}/grey`, hallwall: `${T}/hallwall`, unlit: `${T}/unlit`,
  exwall: `${T}/exwall`, tsurfPlain: `${T}/tsurf_plain`, tsurfTint: `${T}/tsurf_tint`, mtintPlain: `${T}/mtint_plain`, mtintRed: `${T}/mtint_red`,
  probe: `${T}/probe`, glow: `${T}/glow`, glowOpaque: `${T}/glow_opaque`,
  detailNone: `${T}/detail_none`, detailAlways: `${T}/detail_always`, detailFade: `${T}/detail_fade`,
  glass: `${T}/glass`, grate2s: `${T}/grate2s`, grate1s: `${T}/grate1s`, grateOpaque: `${T}/grate_opaque`,
};

// ---- the arena (oax_nav_intent's layout) ----------------------------------------
export const ARENA = { x: [-1024, 1024], y: [-768, 768], z: [0, 512] };
export const NAV = {
  triggerA: { lo: [-736, -544, 0], hi: [-672, -480, 96] },
  triggerB: { lo: [-736, 280, 256], hi: [-672, 344, 352] },
  tower: { lo: [-832, 256, 0], hi: [-576, 512, 256] },
  ledge: { lo: [640, 256, 0], hi: [1024, 768, 192] },
  platform: { lo: [-160, 576, 0], hi: [160, 768, 256] },
  ladderLedge: { lo: [-448, -768, 0], hi: [-192, -576, 160] },
  ladderZone: { lo: [-352, -576, 0], hi: [-288, -536, 224] },
  pad: { lo: [-48, 352, 0], hi: [48, 448, 16] },
  hazard: { lo: [256, -640, 0], hi: [512, -384, 96] },
  items: {
    tower: [-704, 470, 280],
    hazard: [384, -512, 24],
    ledge: [880, 560, 216],
    platform: [0, 700, 280],
    ladder: [-320, -700, 184],
  },
};
export const ARENA_ZONE = { name: 'Showcase Arena', ambient: [0.2, 0.2, 0.2] };

// ---- the gallery ------------------------------------------------------------------
export const GY = 4096;                 // the gallery's centre line
export const HALL = { halfY: 512, z: 768 };
export const BAY = 1536;
export const EXHIBIT_W = 2048;
export const ALBEDO = 128;               // the grey floor
export const OVERBRIGHT = 2;
// light keys as ulight-physical's (docs/lights.md), heights over the floor
export const BAYS = [
  { name: 'q3', h: 80, keys: { oax_profile: 'q3', light: 200 } },
  { name: 'doom3', h: 100, keys: { oax_profile: 'doom3', light_radius: '400 400 400', _color: '1 0.5 0.25' } },
  { name: 'custom', h: 150, keys: { oax_radius: 450, oax_falloff: 'table 0 1 0.3 0.9 0.6 0.4 1 0', oax_intensity: 1.6, oax_cap: 1.2, oax_capKnee: 0.3, oax_angular: 'none', oax_color: '0.9 1 1.1' } },
  { name: 'mask', h: 120, keys: { oax_radius: 450, oax_intensity: 1, oax_mask: 2 } },
  { name: 'zone', h: 0, keys: null, zone: { ambient: '0.3 0.2 0.1', name: 'Amber Bay' } },
  { name: 'effect', h: 120, keys: { oax_radius: 450, oax_intensity: 1, oax_effect: 'oax_sin 0.5 0 0.6 0.39' } },
  { name: 'dark', h: 0, keys: null },
  { name: 'exhibit', h: 0, keys: null, width: EXHIBIT_W, zone: { ambient: '1 1 1', name: 'Exhibit Hall' } },
  { name: 'park', h: 0, keys: null },
];
// bay centres along x, the first at 0
{
  let x = 0;
  BAYS.forEach((b, i) => {
    const w = b.width || BAY;
    if (i) x += ((BAYS[i - 1].width || BAY) + w) / 2;
    b.x = x;
    b.w = w;
  });
}
export const bay = (n) => BAYS.find((b) => b.name === n);
export const HALL_X = [BAYS[0].x - BAYS[0].w / 2, BAYS[BAYS.length - 1].x + BAYS[BAYS.length - 1].w / 2];

// the exhibit bay: panels on the north wall (faces at WALL_Y - 0.5, facing -y)
export const WALL_Y = GY + HALL.halfY;
export const PANEL_Z = [64, 256];
export const EXHIBIT = {
  tsurfPlain: [-900, -772], tsurfTint: [-740, -612],
  mtintPlain: [-560, -432], mtintRed: [-400, -272],
  probe: [-200, -72],
  glow: [40, 168], glowOpaque: [200, 328],
  detailNone: [400, 528], detailAlways: [560, 688], detailFade: [720, 848],
};
// a second pane in the dark park bay (park-local x)
export const GLASS_PARK = { x: [420, 550] };
// the additive glow's tint (its colour scale), and its texture's centre texel
export const GLOW_TINT = 0.25;
export const GLOW_PEAK = [249, 174, 75];
export const GLASS_TEXEL = [150, 200, 250];
export const GLASS = { x: [870, 1000], y: WALL_Y - 40, z: PANEL_Z, tint: [0.7, 0.85, 1, 0.35] };
// free-standing grates across the bay centre line (faces at y = GY)
export const GRATES = { grate2s: [-300, -172], grate1s: [-100, 28], grateOpaque: [100, 228], z: [32, 224] };
export const SURF_TINT = [0.6, 0.8, 1];
export const MAT_TINT = [1, 0.6, 0.4];
export const FLAT = { exwall: 100, panel: 160, grey: ALBEDO, hallwall: 64 };
export const DETAIL_FADE_END = 380;
// the collision-mesh block in the park bay
export const BLOCK = { x: [256, 384], y: [-64, 64], z: [0, 48] };   // relative to the park bay centre / GY
export const PROBE = `${MAT.probe}.tga`;
export const SAME = `${T}/same.tga`;
export const PACK_A = 'zzz_oaxshow_a.pk3';
export const PACK_B = 'zzz_oaxshow_b.pk3';

const flatTga = (r, g = r, b = r, n = 16) => tga(n, n, image(n, n, () => [r / 255, g / 255, b / 255, 1]));
function solid(r, g, b, n = 64) {
  const data = Buffer.alloc(n * n * 4);
  for (let i = 0; i < n * n; i++) data.set([r, g, b, 255], i * 4);
  return encodeTga({ width: n, height: n, data });
}

function art() {
  const files = {};
  const N = 256;
  const f = tiles(N, N, { size: 64, grout: 3, base: [0.5, 0.52, 0.55] });
  files[`${MAT.floor}.tga`] = tga(N, N, image(N, N, f.color));
  files[`${MAT.floor}_n.tga`] = tga(N, N, normalMap(N, N, f.height, 5));
  const b = bricks(N, N, { base: [0.5, 0.36, 0.28] });
  files[`${MAT.wall}.tga`] = tga(N, N, image(N, N, b.color));
  files[`${MAT.wall}_n.tga`] = tga(N, N, normalMap(N, N, b.height, 6));
  const c = tiles(N, N, { size: 128, grout: 4, base: [0.42, 0.43, 0.47] });
  files[`${MAT.ceil}.tga`] = tga(N, N, image(N, N, c.color));
  files[`${MAT.stone}.tga`] = tga(128, 128, image(128, 128, (x, y) => {
    const v = 0.45 + 0.3 * hash2(x >> 2, y >> 2, 11) + 0.1 * hash2(x, y, 5);
    return [v, v * 0.97, v * 0.9, 1];
  }));
  // bars every 16 texels, 4 wide, alpha 0 between (rungs and grates)
  const bars = (x, y) => {
    const bar = (x % 16) < 4 || (y % 16) < 4;
    const v = 0.35 + 0.25 * hash2(x, y, 7);
    return bar ? [v, v * 0.95, v * 0.9, 1] : [0, 0, 0, 0];
  };
  files[`${MAT.rungs}.tga`] = tga(64, 64, image(64, 64, bars));
  for (const m of [MAT.grate2s, MAT.grate1s, MAT.grateOpaque]) files[`${m}.tga`] = tga(64, 64, image(64, 64, bars));
  // radial glow, black at the edges (pad, glow panels)
  const glow = (x, y) => { const d = Math.hypot(x - 31.5, y - 31.5) / 32; const v = Math.max(0, 1 - d); return [v, v * 0.7, v * 0.3, 1]; };
  for (const m of [MAT.pad, MAT.glow, MAT.glowOpaque]) files[`${m}.tga`] = tga(64, 64, image(64, 64, glow));
  // flat, so the blend is measurable: pixel = a * texel * tint + (1 - a) * behind
  files[`${MAT.glass}.tga`] = flatTga(...GLASS_TEXEL);
  files[`${MAT.grey}.tga`] = flatTga(FLAT.grey);
  files[`${MAT.hallwall}.tga`] = flatTga(FLAT.hallwall);
  files[`${MAT.exwall}.tga`] = flatTga(FLAT.exwall);
  files[`${T}/panel.tga`] = flatTga(FLAT.panel);
  return files;
}

const diffuse = (name, tex, extra = '') => `${name}
{
	qer_editorimage ${tex}
${extra}	diffusemap ${tex}
	specularmap _black
}
`;
const detailStage = (fade) => `	{
		map $whiteimage
		rgbGen const ( 0 1 0 )
		blendFunc GL_DST_COLOR GL_SRC_COLOR
${fade ? `		detailFade 0 ${DETAIL_FADE_END}\n` : ''}	}
`;
const withStages = (name, tex, stages) => `${name}
{
	qer_editorimage ${tex}
	diffusemap ${tex}
	specularmap _black
${stages}}
`;
const PANEL_TGA = `${T}/panel.tga`;

const shaders = `// oax_showcase test map shaders (tests/maps/src/oax_showcase.mjs)
${MAT.floor}
{
	qer_editorimage ${MAT.floor}.tga
	diffusemap ${MAT.floor}.tga
	bumpmap ${MAT.floor}_n.tga
}

${MAT.wall}
{
	qer_editorimage ${MAT.wall}.tga
	diffusemap ${MAT.wall}.tga
	bumpmap ${MAT.wall}_n.tga
}

${MAT.ceil}
{
	qer_editorimage ${MAT.ceil}.tga
	diffusemap ${MAT.ceil}.tga
}

${diffuse(MAT.grey, `${MAT.grey}.tga`)}
${diffuse(MAT.hallwall, `${MAT.hallwall}.tga`)}
${diffuse(MAT.exwall, `${MAT.exwall}.tga`)}
${diffuse(MAT.tsurfPlain, PANEL_TGA)}
${diffuse(MAT.tsurfTint, PANEL_TGA)}
${diffuse(MAT.mtintPlain, PANEL_TGA)}
${diffuse(MAT.mtintRed, PANEL_TGA, `	oaxTint ${MAT_TINT.join(' ')}\n`)}
${diffuse(MAT.detailNone, PANEL_TGA)}
${withStages(MAT.detailAlways, PANEL_TGA, detailStage(false))}
${withStages(MAT.detailFade, PANEL_TGA, detailStage(true))}
${MAT.unlit}
{
	qer_editorimage ${MAT.grey}.tga
	{
		map ${MAT.grey}.tga
		rgbGen identity
	}
}
`;

// ---- surfaces ------------------------------------------------------------------------

function planarUV(n, size = 128) {
  const ax = n.map(Math.abs);
  const s = 1 / size;
  if (ax[2] >= ax[0] && ax[2] >= ax[1]) return uvMatrix({ u: [s, 0, 0], v: [0, -s, 0] });
  if (ax[0] >= ax[1]) return uvMatrix({ u: [0, s, 0], v: [0, 0, -s] });
  return uvMatrix({ u: [s, 0, 0], v: [0, 0, -s] });
}

// an axial rectangle on plane `axis` = at, spanning lo..hi on the other two
// axes (in order), facing `want` (wound counter-clockwise seen from it)
function rect(sw, axis, at, lo, hi, want, material, extra = {}) {
  const o = [0, 1, 2].filter((k) => k !== axis);
  const P = (u, v) => { const p = [0, 0, 0]; p[axis] = at; p[o[0]] = u; p[o[1]] = v; return p; };
  let pts = [P(lo[0], lo[1]), P(hi[0], lo[1]), P(hi[0], hi[1]), P(lo[0], hi[1])];
  // Newell normal of the points as listed; reverse when it faces away
  let n = [0, 0, 0];
  for (let i = 0; i < 4; i++) {
    const a = pts[i], b = pts[(i + 1) % 4];
    n = [n[0] + (a[1] - b[1]) * (a[2] + b[2]), n[1] + (a[2] - b[2]) * (a[0] + b[0]), n[2] + (a[0] - b[0]) * (a[1] + b[1])];
  }
  if (n[0] * want[0] + n[1] * want[1] + n[2] * want[2] < 0) pts = pts.reverse();
  return sw.polygon({ material, points: pts, uv: extra.uv || planarUV(want, extra.uvSize || 128), ...extra });
}

// the visible faces of a hull box: `faces` lists which of top,px,nx,py,ny
function dressBox(sw, lo, hi, faces, material, extra = {}) {
  const f = new Set(faces);
  if (f.has('top')) rect(sw, 2, hi[2], [lo[0], lo[1]], [hi[0], hi[1]], [0, 0, 1], material, extra);
  if (f.has('px')) rect(sw, 0, hi[0], [lo[1], lo[2]], [hi[1], hi[2]], [1, 0, 0], material, extra);
  if (f.has('nx')) rect(sw, 0, lo[0], [lo[1], lo[2]], [hi[1], hi[2]], [-1, 0, 0], material, extra);
  if (f.has('py')) rect(sw, 1, hi[1], [lo[0], lo[2]], [hi[0], hi[2]], [0, 1, 0], material, extra);
  if (f.has('ny')) rect(sw, 1, lo[1], [lo[0], lo[2]], [hi[0], hi[2]], [0, -1, 0], material, extra);
}

// the inside of a closed box (floor, ceiling, four walls)
function hullRoom(map, [x0, y0, z0], [x1, y1, z1], { floorZ = z0 } = {}) {
  map.brush(box([x0 - TH, y0 - TH, floorZ - TH], [x1 + TH, y1 + TH, floorZ], CAULK));
  map.brush(box([x0 - TH, y0 - TH, z1], [x1 + TH, y1 + TH, z1 + TH], CAULK));
  map.brush(box([x0 - TH, y0 - TH, floorZ], [x0, y1 + TH, z1], CAULK));
  map.brush(box([x1, y0 - TH, floorZ], [x1 + TH, y1 + TH, z1], CAULK));
  map.brush(box([x0, y0 - TH, floorZ], [x1, y0, z1], CAULK));
  map.brush(box([x0, y1, floorZ], [x1, y1 + TH, z1], CAULK));
}

// the collision-mesh block: a closed box of 12 triangles, counter-clockwise from outside
export function blockMesh() {
  const p = bay('park');
  const lo = [p.x + BLOCK.x[0], GY + BLOCK.y[0], BLOCK.z[0]], hi = [p.x + BLOCK.x[1], GY + BLOCK.y[1], BLOCK.z[1]];
  const v = [];
  for (let i = 0; i < 8; i++) v.push([i & 1 ? hi[0] : lo[0], i & 2 ? hi[1] : lo[1], i & 4 ? hi[2] : lo[2]]);
  // faces as quads (outward): -z, +z, -x, +x, -y, +y
  const quads = [[0, 2, 3, 1], [4, 5, 7, 6], [0, 4, 6, 2], [1, 3, 7, 5], [0, 1, 5, 4], [2, 6, 7, 3]];
  const idx = [];
  for (const [a, b, c, d] of quads) idx.push(a, b, c, a, c, d);
  return { positions: v, indexes: idx, lo, hi };
}

function arenaSurfaces(sw) {
  const [x0, x1] = ARENA.x, [y0, y1] = ARENA.y, [z0, z1] = ARENA.z;
  rect(sw, 2, z0, [x0, y0], [x1, y1], [0, 0, 1], MAT.floor);
  rect(sw, 2, z1, [x0, y0], [x1, y1], [0, 0, -1], MAT.ceil);
  rect(sw, 0, x0, [y0, z0], [y1, z1], [1, 0, 0], MAT.wall);
  rect(sw, 0, x1, [y0, z0], [y1, z1], [-1, 0, 0], MAT.wall);
  rect(sw, 1, y0, [x0, z0], [x1, z1], [0, 1, 0], MAT.wall);
  rect(sw, 1, y1, [x0, z0], [x1, z1], [0, -1, 0], MAT.wall);
  // the solids' faces that are not against a wall
  dressBox(sw, NAV.tower.lo, NAV.tower.hi, ['top', 'px', 'nx', 'py', 'ny'], MAT.stone, { uvSize: 64 });
  dressBox(sw, NAV.ledge.lo, NAV.ledge.hi, ['top', 'nx', 'ny'], MAT.stone, { uvSize: 64 });
  dressBox(sw, NAV.platform.lo, NAV.platform.hi, ['top', 'px', 'nx', 'ny'], MAT.stone, { uvSize: 64 });
  dressBox(sw, NAV.ladderLedge.lo, NAV.ladderLedge.hi, ['top', 'px', 'nx', 'py'], MAT.stone, { uvSize: 64 });
  // the jump pad: an additive glow half a unit over the floor
  const p = NAV.pad;
  rect(sw, 2, 0.5, [p.lo[0], p.lo[1]], [p.hi[0], p.hi[1]], [0, 0, 1], MAT.pad,
    { flags: OSF.ADDITIVE | OSF.DETAIL, uv: uvMatrix({ u: [1 / 96, 0, 0], uOffset: 0.5, v: [0, -1 / 96, 0], vOffset: 400 / 96 + 0.5 }) });
  // ladder rungs: two-sided masked, half a unit off the ledge face
  const lz = NAV.ladderZone;
  rect(sw, 1, NAV.ladderLedge.hi[1] + 0.5, [lz.lo[0], 0], [lz.hi[0], NAV.ladderLedge.hi[2]], [0, 1, 0], MAT.rungs,
    { flags: OSF.TWOSIDED | OSF.MASKED | OSF.DETAIL, uvSize: 64 });
}

function gallerySurfaces(sw) {
  const y0 = GY - HALL.halfY, y1 = GY + HALL.halfY, z1 = HALL.z;
  // end walls
  rect(sw, 0, HALL_X[0], [y0, 0], [y1, z1], [1, 0, 0], MAT.hallwall);
  rect(sw, 0, HALL_X[1], [y0, 0], [y1, z1], [-1, 0, 0], MAT.hallwall);
  for (const b of BAYS) {
    const bx0 = b.x - b.w / 2, bx1 = b.x + b.w / 2;
    const wallMat = b.name === 'exhibit' ? MAT.exwall : MAT.hallwall;
    // per bay walls and ceiling, so each takes its own bay's zone ambient
    rect(sw, 1, y0, [bx0, 0], [bx1, z1], [0, 1, 0], wallMat);
    rect(sw, 1, y1, [bx0, 0], [bx1, z1], [0, -1, 0], wallMat);
    rect(sw, 2, z1, [bx0, y0], [bx1, y1], [0, 0, -1], wallMat);
    // the floor, split at GY where a half differs
    if (b.name === 'mask') {
      rect(sw, 2, 0, [bx0, y0], [bx1, GY], [0, 0, 1], MAT.grey);
      rect(sw, 2, 0, [bx0, GY], [bx1, y1], [0, 0, 1], MAT.grey, { lightMask: 2 });
    } else if (b.name === 'zone') {
      rect(sw, 2, 0, [bx0, y0], [bx1, GY], [0, 0, 1], MAT.grey);
      rect(sw, 2, 0, [bx0, GY], [bx1, y1], [0, 0, 1], MAT.grey);
    } else if (b.name === 'dark') {
      rect(sw, 2, 0, [bx0, y0], [bx1, GY], [0, 0, 1], MAT.grey);
      rect(sw, 2, 0, [bx0, GY], [bx1, y1], [0, 0, 1], MAT.unlit);
    } else {
      rect(sw, 2, 0, [bx0, y0], [bx1, y1], [0, 0, 1], MAT.grey);
    }
  }
  // the exhibits
  const ex = bay('exhibit');
  const py = WALL_Y - 0.5, facing = [0, -1, 0];
  const panel = (k, material, extra = {}) => rect(sw, 1, py, [ex.x + EXHIBIT[k][0], PANEL_Z[0]], [ex.x + EXHIBIT[k][1], PANEL_Z[1]], facing, material, { uvSize: 64, ...extra });
  panel('tsurfPlain', MAT.tsurfPlain);
  panel('tsurfTint', MAT.tsurfTint, { tint: [...SURF_TINT, 1] });
  panel('mtintPlain', MAT.mtintPlain);
  panel('mtintRed', MAT.mtintRed);
  panel('probe', MAT.probe);
  // one glow texture repeat over each glow panel
  const glowUV = (k) => uvMatrix({ u: [1 / 128, 0, 0], uOffset: -(ex.x + EXHIBIT[k][0]) / 128, v: [0, 0, -1 / 192], vOffset: PANEL_Z[1] / 192 });
  panel('glow', MAT.glow, { flags: OSF.ADDITIVE | OSF.DETAIL, tint: [GLOW_TINT, GLOW_TINT, GLOW_TINT, 1], uv: glowUV('glow') });
  panel('glowOpaque', MAT.glowOpaque, { uv: glowUV('glowOpaque') });
  panel('detailNone', MAT.detailNone);
  panel('detailAlways', MAT.detailAlways);
  panel('detailFade', MAT.detailFade);
  rect(sw, 1, GLASS.y, [ex.x + GLASS.x[0], GLASS.z[0]], [ex.x + GLASS.x[1], GLASS.z[1]], facing, MAT.glass,
    { flags: OSF.TWOSIDED | OSF.TRANSLUCENT | OSF.DETAIL, tint: GLASS.tint, uvSize: 64 });
  const grate = (k, material, flags) => rect(sw, 1, GY, [ex.x + GRATES[k][0], GRATES.z[0]], [ex.x + GRATES[k][1], GRATES.z[1]], facing, material, { flags: flags | OSF.DETAIL, uvSize: 64 });
  grate('grate2s', MAT.grate2s, OSF.TWOSIDED | OSF.MASKED);
  grate('grate1s', MAT.grate1s, OSF.MASKED);
  grate('grateOpaque', MAT.grateOpaque, OSF.TWOSIDED);
  // the same pane in the park bay, where no light or ambient reaches
  const pk = bay('park');
  rect(sw, 1, GLASS.y, [pk.x + GLASS_PARK.x[0], GLASS.z[0]], [pk.x + GLASS_PARK.x[1], GLASS.z[1]], facing, MAT.glass,
    { flags: OSF.TWOSIDED | OSF.TRANSLUCENT | OSF.DETAIL, tint: GLASS.tint, uvSize: 64 });
  // the park bay's block: a render mesh over the collision mesh
  const blk = blockMesh();
  const st = blk.positions.map((p) => [(p[0] + p[2]) / 64, (p[1] + p[2]) / 64]);
  sw.mesh({ material: MAT.stone, positions: blk.positions, indexes: blk.indexes, st, flags: OSF.DETAIL, sourceId: 500 });
}

export function build({ variant = 'full' } = {}) {
  const nohull = variant === 'nohull';
  const classic = variant === 'classic';
  const map = new MapFile({
    message: `oax test: step 7.5 showcase${nohull ? ' (arena hull lowered)' : classic ? ' (classic teleporters)' : ''}`,
    oax_lighting: 'unified',
    oax_ambient: '0 0 0',
    oax_shadowmode: 'maps',
    oax_overbright: OVERBRIGHT,
    _keepLights: 1,
  });

  // ---- arena hull and entities ----
  hullRoom(map, [ARENA.x[0], ARENA.y[0], ARENA.z[0]], [ARENA.x[1], ARENA.y[1], ARENA.z[1]], { floorZ: nohull ? -512 : 0 });
  for (const k of ['tower', 'ledge', 'platform', 'ladderLedge']) {
    const s = NAV[k];
    map.brush(box(nohull ? [s.lo[0], s.lo[1], -512] : s.lo, s.hi, CAULK));
  }
  map.entity('func_oax_zone', { name: ARENA_ZONE.name, ambient: ARENA_ZONE.ambient.join(' ') }, [box([ARENA.x[0], ARENA.y[0], ARENA.z[0]], [ARENA.x[1], ARENA.y[1], ARENA.z[1]], 'common/trigger')]);
  map.entity('trigger_teleport', { target: 'tele_up', ...(classic ? {} : { noretrigger: 1 }) }, [box(NAV.triggerA.lo, NAV.triggerA.hi, 'common/trigger')]);
  map.entity('trigger_teleport', { target: 'tele_down', ...(classic ? {} : { noretrigger: 1 }) }, [box(NAV.triggerB.lo, NAV.triggerB.hi, 'common/trigger')]);
  map.entity('misc_teleporter_dest', { targetname: 'tele_up', origin: [-704, 312, 280], angle: 90 });
  map.entity('misc_teleporter_dest', { targetname: 'tele_down', origin: [-704, -512, 24], angle: 270 });
  map.entity('item_armor_combat', { origin: NAV.items.tower });
  map.entity('trigger_hurt', { dmg: 5 }, [box(NAV.hazard.lo, NAV.hazard.hi, 'common/trigger')]);
  map.entity('item_health_large', { origin: NAV.items.hazard });
  map.entity('weapon_railgun', { origin: NAV.items.ledge });
  map.entity('info_oax_route', { origin: [200, 512, 8], target: 'ledge_top', kind: 'translocator' });
  map.entity('info_oax_route', { origin: [760, 512, 200], targetname: 'ledge_top' });
  map.entity('info_oax_route', { origin: [700, 400, 200], target: 'ledge_down', kind: 'drop' });
  map.entity('info_oax_route', { origin: [560, 400, 8], targetname: 'ledge_down' });
  map.entity('trigger_push', { target: 'pad_apex' }, [box(NAV.pad.lo, NAV.pad.hi, 'common/trigger')]);
  map.entity('target_position', { targetname: 'pad_apex', origin: [0, 660, 330] });
  map.entity('weapon_rocketlauncher', { origin: NAV.items.platform });
  map.entity('info_oax_route', { origin: [130, 610, 264], target: 'pad_down', kind: 'drop' });
  map.entity('info_oax_route', { origin: [200, 480, 8], targetname: 'pad_down' });
  // the ladder wins over the arena zone where they overlap (higher priority)
  map.entity('func_oax_zone', { ladder: 200, priority: 5 }, [box(NAV.ladderZone.lo, NAV.ladderZone.hi, 'common/trigger')]);
  map.entity('item_health_mega', { origin: NAV.items.ladder });
  map.entity('info_oax_route', { origin: [-250, -600, 168], target: 'ladder_down', kind: 'drop' });
  map.entity('info_oax_route', { origin: [-250, -480, 8], targetname: 'ladder_down' });
  for (const [x, y, a] of [[0, 0, 0], [-400, 0, 0], [400, 0, 180], [0, -300, 90]]) map.entity('info_player_deathmatch', { origin: [x, y, 32], angle: a });
  for (const [x, y] of [[-600, -400], [-600, 400], [0, 0], [600, -400], [600, 400], [0, 600]]) map.entity('light', { origin: [x, y, 420], light: 700 });

  // ---- gallery hull and entities ----
  hullRoom(map, [HALL_X[0], GY - HALL.halfY, 0], [HALL_X[1], GY + HALL.halfY, HALL.z]);
  for (const b of BAYS) {
    if (b.keys) map.entity('light', { origin: [b.x, GY, b.h], ...b.keys });
    if (b.zone) {
      const lo = b.name === 'zone' ? [b.x - b.w / 2, GY, 0] : [b.x - b.w / 2, GY - HALL.halfY, 0];
      map.entity('func_oax_zone', b.zone, [box(lo, [b.x + b.w / 2, GY + HALL.halfY, HALL.z], 'common/trigger')]);
    }
  }

  const sw = new SurfaceWorld();
  arenaSurfaces(sw);
  gallerySurfaces(sw);
  const coll = new CollisionMeshes();
  const blk = blockMesh();
  coll.mesh({ positions: blk.positions, indexes: blk.indexes, thickness: 4, sourceId: 500 });

  const files = { ...art(), 'scripts/oax_showcase.shader': shaders };
  const same = solid(40, 60, 200);
  const spec = {
    map, light: 'none', aas: false, files, surfaces: sw, collision: coll,
    manifest: { features: ['ulight', 'surfaces', 'collision', 'nav_intent'], lighting: 'unified' },
    // the probe image lives only in the packages; the build's material check needs it
    compileFiles: { [PROBE]: solid(255, 255, 255) },
  };
  if (variant === 'full') {
    spec.packages = [
      { name: PACK_A, map: true, files: { [PROBE]: solid(230, 30, 30), [SAME]: same } },
      { name: PACK_B, files: { [PROBE]: solid(30, 230, 30), [SAME]: same } },
    ];
  }
  return spec;
}
