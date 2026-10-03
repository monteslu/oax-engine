// oax_surfworld: the surface-world test map (step 7.5, docs/map-format.md).
//
// Everything the player sees is OAX_SURFACES surfaces of zero thickness,
// over a separate hull of caulk brushes that gives collision, PVS and
// areas. Two rooms along x joined by a doorway with a door (a func_door of
// caulk whose visible faces are surfaces of its inline model) and an area
// portal:
//   A (west)  floor in four pieces around a platform and a ramp (both hull
//             boxes dressed with surfaces), the divider's face as a concave
//             polygon around the doorway, a two-sided masked grate, a
//             translucent glass pane, a tinted platform, an invisible quad;
//   B (east)  a stock-material floor, a stone arch (an indexed mesh with
//             per-vertex normals and UVs, light-mask group 2, lit only by
//             its own light, and an OAX_COLLISION mesh so it collides), an
//             additive glow panel, and an ordinary textured brush pillar
//             (brush faces and surfaces coexist).
// Unified lighting, no q3map2 -light stage. No AAS: bots and the nav tests
// use the navmesh, built from the hull.
//
// Variants (separate source files reuse build() with options):
//   oax_surfworld_nohull  the same surfaces with the hull floor 512 units
//                         lower (the player falls; the surfaces float), plus
//                         one surface buried in the lowered floor;
//   oax_surfworld_nosurf  the same hull without the surface world (solid
//                         but invisible).

import { MapFile, box, Brush, Face } from '../mapwriter.mjs';
import { SurfaceWorld, CollisionMeshes, OSF, uvMatrix } from '../../../misc/tools/oax-surfaces.mjs';
import { tga, bricks, tiles, normalMap, image, hash2 } from '../lib/texgen.mjs';

// Light values are doubled (x2): the map was authored when unified light drew
// at 2x on screen; since the frame contract (docs/lights.md: light 1 is the
// texture at 1x) the same frame needs twice the light.
const x2 = (c) => c.map((v) => v * 2);

const T = 'textures/oax_surfworld';
export const MAT = {
  floor: `${T}/floor`, wall: `${T}/wall`, ceil: `${T}/ceil`, grate: `${T}/grate`, glass: `${T}/glass`,
  glow: `${T}/glow`, stone: `${T}/stone`, unlit: `${T}/unlit`, stockFloor: 'textures/base_floor/clang_floor', stockWall: 'textures/gothic_wall/oct20c',
};
const CAULK = 'common/caulk';

export const Z0 = 0, Z1 = 320, Y0 = -512, Y1 = 512, TH = 16;
export const ROOM_A = [-1024, -32], ROOM_B = [32, 1024];
export const DOOR_Y = [-64, 64], DOOR_Z = 128;
export const PLATFORM = { x: [-800, -560], y: [-400, -160], z: 64 };
export const RAMP = { x: [-560, -300] };     // from the platform's edge (z 64) down to the floor
export const PILLAR = { x: [400, 496], y: [-48, 48] };
export const ARCH = { x: 700, y: 300, inner: 72, outer: 104, depth: 32, segments: 16 };
// room C: a sealed room no light reaches (a dark zone), for unlit materials
export const ROOM_C = { x: [1200, 1456], y: [-128, 128], z: [0, 192] };
// tint panels on room B's east wall, in pairs symmetric about the light at y -200
export const TINT = 0.77;
export const TINT_PANELS = { wall: { z: [100, 180] }, stone: { z: [200, 280] }, y: { plain: [-330, -230], tinted: [-170, -70] } };
// shadow pair: two grates in room B mirrored about the light at x 560, the
// east one OSF_NOSHADOW (lit, casts nothing)
export const SHADOW_GRATES = { plain: 360, noshadow: 760, y: [-300, -100], z: [0, 128] };
export const SPAWN = { x: -700, y: 200, z: 24, yaw: 0 };

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
  // a grate: metal bars with alpha 0 holes (image only: the engine builds its shader)
  files[`${MAT.grate}.tga`] = tga(64, 64, image(64, 64, (x, y) => {
    const bar = (x % 16) < 4 || (y % 16) < 4;
    const v = 0.35 + 0.25 * hash2(x, y, 7);
    return bar ? [v, v * 0.95, v * 0.9, 1] : [0, 0, 0, 0];
  }));
  files[`${MAT.glass}.tga`] = tga(64, 64, image(64, 64, (x, y) => {
    const v = 0.8 + 0.2 * hash2(x >> 3, y >> 3, 3);
    return [v * 0.7, v * 0.85, v, 1];
  }));
  files[`${MAT.glow}.tga`] = tga(64, 64, image(64, 64, (x, y) => {
    const d = Math.hypot(x - 31.5, y - 31.5) / 32;
    const v = Math.max(0, 1 - d);
    return [v, v * 0.6, v * 0.2, 1];
  }));
  files[`${MAT.stone}.tga`] = tga(128, 128, image(128, 128, (x, y) => {
    const v = 0.45 + 0.3 * hash2(x >> 2, y >> 2, 11) + 0.1 * hash2(x, y, 5);
    return [v, v * 0.97, v * 0.9, 1];
  }));
  return files;
}

const shaders = `
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

${MAT.unlit}
{
	qer_editorimage ${MAT.stone}.tga
	{
		map ${MAT.stone}.tga
		rgbGen identity
	}
}
`;

// ---- the hull -------------------------------------------------------------

export function hull(map, { floorZ = Z0, solids = true } = {}) {
  const [xa, xb] = [ROOM_A[0], ROOM_B[1]];
  map.brush(box([xa - TH, Y0 - TH, floorZ - TH], [xb + TH, Y1 + TH, floorZ], CAULK));
  map.brush(box([xa - TH, Y0 - TH, Z1], [xb + TH, Y1 + TH, Z1 + TH], CAULK));
  map.brush(box([xa - TH, Y0 - TH, floorZ], [xa, Y1 + TH, Z1], CAULK));
  map.brush(box([xb, Y0 - TH, floorZ], [xb + TH, Y1 + TH, Z1], CAULK));
  map.brush(box([xa, Y0 - TH, floorZ], [xb, Y0, Z1], CAULK));
  map.brush(box([xa, Y1, floorZ], [xb, Y1 + TH, Z1], CAULK));
  // the divider with its doorway (down to the floor wherever the floor is)
  map.brush(box([ROOM_A[1], Y0, floorZ], [ROOM_B[0], DOOR_Y[0], Z1], CAULK));
  map.brush(box([ROOM_A[1], DOOR_Y[1], floorZ], [ROOM_B[0], Y1, Z1], CAULK));
  map.brush(box([ROOM_A[1], DOOR_Y[0], DOOR_Z], [ROOM_B[0], DOOR_Y[1], Z1], CAULK));
  if (floorZ < Z0) map.brush(box([ROOM_A[1], DOOR_Y[0], floorZ], [ROOM_B[0], DOOR_Y[1], Z0], CAULK));
  // the door (caulk; its look is surfaces of model *1) and its area portal
  map.entity('func_door', { angle: -1, lip: 8, speed: 400, wait: 2 }, [
    box([ROOM_A[1] + 4, DOOR_Y[0], Z0], [ROOM_B[0] - 4, DOOR_Y[1], DOOR_Z], CAULK),
  ]);
  map.brush(box([ROOM_A[1] + 14, DOOR_Y[0], Z0], [ROOM_B[0] - 14, DOOR_Y[1], DOOR_Z], 'common/areaportal'));
  if (solids) {
    // the platform and the ramp up to it
    map.brush(box([PLATFORM.x[0], PLATFORM.y[0], Z0], [PLATFORM.x[1], PLATFORM.y[1], PLATFORM.z], CAULK));
    const run = RAMP.x[1] - RAMP.x[0], rise = PLATFORM.z;
    map.brush(new Brush([
      new Face([0, 0, -1], -Z0, CAULK),
      new Face([-1, 0, 0], -RAMP.x[0], CAULK),
      new Face([0, -1, 0], -PLATFORM.y[0], CAULK),
      new Face([0, 1, 0], PLATFORM.y[1], CAULK),
      new Face([rise, 0, run], rise * RAMP.x[1] + run * Z0, CAULK),
    ]).check());
  }
  // room C, sealed off, with nothing but an entity to keep q3map2 from filling it
  const c = ROOM_C;
  map.brush(box([c.x[0] - TH, c.y[0] - TH, c.z[0] - TH], [c.x[1] + TH, c.y[1] + TH, c.z[0]], CAULK));
  map.brush(box([c.x[0] - TH, c.y[0] - TH, c.z[1]], [c.x[1] + TH, c.y[1] + TH, c.z[1] + TH], CAULK));
  map.brush(box([c.x[0] - TH, c.y[0] - TH, c.z[0]], [c.x[0], c.y[1] + TH, c.z[1]], CAULK));
  map.brush(box([c.x[1], c.y[0] - TH, c.z[0]], [c.x[1] + TH, c.y[1] + TH, c.z[1]], CAULK));
  map.brush(box([c.x[0], c.y[0] - TH, c.z[0]], [c.x[1], c.y[0], c.z[1]], CAULK));
  map.brush(box([c.x[0], c.y[1], c.z[0]], [c.x[1], c.y[1] + TH, c.z[1]], CAULK));
  map.entity('info_notnull', { origin: [(c.x[0] + c.x[1]) / 2, 0, 96], targetname: 'roomc' });
  // brush faces coexisting with the surface world
  map.brush(box([PILLAR.x[0], PILLAR.y[0], Z0], [PILLAR.x[1], PILLAR.y[1], Z1], { sides: 'gothic_wall/oct20c', top: CAULK, bottom: CAULK }));
}

// ---- the surfaces -------------------------------------------------------------

const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

// world-aligned planar texture projection, `size` units per repeat
function planarUV(n, size = 128) {
  const ax = [Math.abs(n[0]), Math.abs(n[1]), Math.abs(n[2])];
  const s = 1 / size;
  if (ax[2] >= ax[0] && ax[2] >= ax[1]) return uvMatrix({ u: [s, 0, 0], v: [0, -s, 0] });
  if (ax[0] >= ax[1]) return uvMatrix({ u: [0, s, 0], v: [0, 0, -s] });
  return uvMatrix({ u: [s, 0, 0], v: [0, 0, -s] });
}

// a polygon facing `want` (reversed if wound the other way)
function poly(sw, points, want, material, extra = {}) {
  let n = [0, 0, 0];
  for (let i = 0; i < points.length; i++) {
    const a = points[i], b = points[(i + 1) % points.length];
    n = [n[0] + (a[1] - b[1]) * (a[2] + b[2]), n[1] + (a[2] - b[2]) * (a[0] + b[0]), n[2] + (a[0] - b[0]) * (a[1] + b[1])];
  }
  const pts = dot(n, want) < 0 ? [...points].reverse() : points;
  return sw.polygon({ material, points: pts, uv: planarUV(want, extra.uvSize || 128), ...extra });
}

// an axial rectangle on plane axis = at, spanning [lo, hi] on the other two
function rect(sw, axis, at, lo, hi, want, material, extra) {
  const P = (u, v) => {
    const p = [0, 0, 0];
    const o = [0, 1, 2].filter((k) => k !== axis);
    p[axis] = at; p[o[0]] = u; p[o[1]] = v;
    return p;
  };
  return poly(sw, [P(lo[0], lo[1]), P(hi[0], lo[1]), P(hi[0], hi[1]), P(lo[0], hi[1])], want, material, extra);
}

// the arch: a half ring in the y-z plane, `depth` along x; render mesh
// (front, back, outer, inner faces; per-vertex normals and UVs) and the
// closed collision shell (plus the two feet)
export function archGeometry() {
  const { x, y, inner, outer, depth, segments } = ARCH;
  const x0 = x - depth / 2, x1 = x + depth / 2;
  const P = [], N = [], ST = [], I = [];
  const ring = (r, th) => [y + r * Math.cos(th), r * Math.sin(th)];
  const add = (p, n, st) => { P.push(p); N.push(n); ST.push(st); return P.length - 1; };
  const quad = (a, b, c, d) => I.push(a, b, c, a, c, d);
  for (const [xf, nx] of [[x1, 1], [x0, -1]]) {
    const base = P.length;
    for (let i = 0; i <= segments; i++) {
      const th = (Math.PI * i) / segments;
      const [yi, zi] = ring(inner, th), [yo, zo] = ring(outer, th);
      add([xf, yi, zi], [nx, 0, 0], [yi / 64, -zi / 64]);
      add([xf, yo, zo], [nx, 0, 0], [yo / 64, -zo / 64]);
    }
    for (let i = 0; i < segments; i++) {
      const a = base + i * 2, b = a + 1, c = a + 3, d = a + 2;   // inner_i, outer_i, outer_i+1, inner_i+1
      if (nx > 0) quad(a, d, c, b); else quad(a, b, c, d);
    }
  }
  for (const [r, sgn] of [[outer, 1], [inner, -1]]) {
    const base = P.length;
    for (let i = 0; i <= segments; i++) {
      const th = (Math.PI * i) / segments;
      const [yy, zz] = ring(r, th);
      const n = [0, Math.cos(th) * sgn, Math.sin(th) * sgn];
      add([x0, yy, zz], n, [0, (r * th) / 64]);
      add([x1, yy, zz], n, [depth / 64, (r * th) / 64]);
    }
    for (let i = 0; i < segments; i++) {
      const a = base + i * 2, b = a + 1, c = a + 3, d = a + 2;   // x0_i, x1_i, x1_i+1, x0_i+1
      if (sgn > 0) quad(a, d, c, b); else quad(a, b, c, d);
    }
  }
  // orient every render triangle along its vertex normals
  for (let t = 0; t < I.length; t += 3) {
    const [a, b, c] = [I[t], I[t + 1], I[t + 2]];
    const fn = cross(sub(P[b], P[a]), sub(P[c], P[a]));
    if (dot(fn, N[a]) < 0) { I[t + 1] = c; I[t + 2] = b; }
  }
  // the collision shell: the same triangles plus the feet (facing down)
  const CI = [...I];
  const CP = [...P];
  for (const th of [0, Math.PI]) {
    const [yi] = ring(inner, th), [yo] = ring(outer, th);
    const k = CP.length;
    CP.push([x0, yi, 0], [x1, yi, 0], [x1, yo, 0], [x0, yo, 0]);
    const tri = [[k, k + 1, k + 2], [k, k + 2, k + 3]];
    for (const [a, b, c] of tri) {
      const fn = cross(sub(CP[b], CP[a]), sub(CP[c], CP[a]));
      CI.push(...(fn[2] < 0 ? [a, b, c] : [a, c, b]));
    }
  }
  return { positions: P, normals: N, st: ST, indexes: I, collision: { positions: CP, indexes: CI } };
}

export function surfaces({ buried = false } = {}) {
  const sw = new SurfaceWorld();
  const up = [0, 0, 1], down = [0, 0, -1];
  const [ax0, ax1] = ROOM_A, [bx0, bx1] = ROOM_B;
  const pl = PLATFORM;

  // room A floor, in four pieces around the platform and ramp footprint
  rect(sw, 2, Z0, [ax0, Y0], [ax1, pl.y[0]], up, MAT.floor);
  rect(sw, 2, Z0, [ax0, pl.y[1]], [ax1, Y1], up, MAT.floor);
  rect(sw, 2, Z0, [ax0, pl.y[0]], [pl.x[0], pl.y[1]], up, MAT.floor);
  rect(sw, 2, Z0, [RAMP.x[1], pl.y[0]], [ax1, pl.y[1]], up, MAT.floor);
  // room A walls and ceiling
  rect(sw, 0, ax0, [Y0, Z0], [Y1, Z1], [1, 0, 0], MAT.wall);
  rect(sw, 1, Y0, [ax0, Z0], [ax1, Z1], [0, 1, 0], MAT.wall);
  rect(sw, 1, Y1, [ax0, Z0], [ax1, Z1], [0, -1, 0], MAT.wall);
  rect(sw, 2, Z1, [ax0, Y0], [ax1, Y1], down, MAT.ceil);
  // the divider's two faces: concave polygons around the doorway
  for (const [xx, n] of [[ax1, [-1, 0, 0]], [bx0, [1, 0, 0]]]) {
    poly(sw, [[xx, Y0, Z0], [xx, DOOR_Y[0], Z0], [xx, DOOR_Y[0], DOOR_Z], [xx, DOOR_Y[1], DOOR_Z], [xx, DOOR_Y[1], Z0], [xx, Y1, Z0], [xx, Y1, Z1], [xx, Y0, Z1]], n, MAT.wall);
  }
  // the doorway's reveals
  rect(sw, 1, DOOR_Y[0], [ax1, Z0], [bx0, DOOR_Z], [0, 1, 0], MAT.wall);
  rect(sw, 1, DOOR_Y[1], [ax1, Z0], [bx0, DOOR_Z], [0, -1, 0], MAT.wall);
  rect(sw, 2, DOOR_Z, [ax1, DOOR_Y[0]], [bx0, DOOR_Y[1]], down, MAT.wall);
  rect(sw, 2, Z0, [ax1, DOOR_Y[0]], [bx0, DOOR_Y[1]], up, MAT.floor);

  // the platform (top, three sides tinted) and the ramp
  rect(sw, 2, pl.z, [pl.x[0], pl.y[0]], [pl.x[1], pl.y[1]], up, MAT.floor);
  const tint = [1, 0.45, 0.4, 1];
  rect(sw, 0, pl.x[0], [pl.y[0], Z0], [pl.y[1], pl.z], [-1, 0, 0], MAT.wall, { tint });
  rect(sw, 1, pl.y[0], [pl.x[0], Z0], [pl.x[1], pl.z], [0, -1, 0], MAT.wall, { tint });
  rect(sw, 1, pl.y[1], [pl.x[0], Z0], [pl.x[1], pl.z], [0, 1, 0], MAT.wall, { tint });
  const rn = [pl.z, 0, RAMP.x[1] - RAMP.x[0]];
  poly(sw, [[RAMP.x[0], pl.y[0], pl.z], [RAMP.x[1], pl.y[0], Z0], [RAMP.x[1], pl.y[1], Z0], [RAMP.x[0], pl.y[1], pl.z]], rn, MAT.floor);
  poly(sw, [[RAMP.x[0], pl.y[0], Z0], [RAMP.x[1], pl.y[0], Z0], [RAMP.x[0], pl.y[0], pl.z]], [0, -1, 0], MAT.wall, { tint });
  poly(sw, [[RAMP.x[0], pl.y[1], Z0], [RAMP.x[1], pl.y[1], Z0], [RAMP.x[0], pl.y[1], pl.z]], [0, 1, 0], MAT.wall, { tint });

  // free-standing detail: a two-sided masked grate, a translucent pane
  rect(sw, 0, -200, [150, Z0], [342, 128], [1, 0, 0], MAT.grate, { flags: OSF.TWOSIDED | OSF.MASKED | OSF.DETAIL, uvSize: 64 });
  rect(sw, 1, 300, [-520, Z0], [-320, 144], [0, -1, 0], MAT.glass, { flags: OSF.TWOSIDED | OSF.TRANSLUCENT | OSF.DETAIL, tint: [0.7, 0.85, 1, 0.35] });
  // never drawn
  rect(sw, 2, 200, [-900, -300], [-800, -200], down, MAT.wall, { flags: OSF.INVISIBLE });

  // room B: stock-material floor, walls, ceiling
  rect(sw, 2, Z0, [bx0, Y0], [bx1, Y1], up, MAT.stockFloor, { uvSize: 256 });
  rect(sw, 0, bx1, [Y0, Z0], [Y1, Z1], [-1, 0, 0], MAT.wall);
  rect(sw, 1, Y0, [bx0, Z0], [bx1, Z1], [0, 1, 0], MAT.wall);
  rect(sw, 1, Y1, [bx0, Z0], [bx1, Z1], [0, -1, 0], MAT.wall);
  rect(sw, 2, Z1, [bx0, Y0], [bx1, Y1], down, MAT.ceil);
  // an additive glow panel half a unit off the +y wall
  rect(sw, 1, Y1 - 0.5, [800, 150], [950, 250], [0, -1, 0], MAT.glow,
    { flags: OSF.ADDITIVE, uv: uvMatrix({ u: [1 / 150, 0, 0], uOffset: -800 / 150, v: [0, 0, -1 / 100], vOffset: 2.5 }) });
  // the arch: a mesh in light-mask group 2
  const arch = archGeometry();
  sw.mesh({ material: MAT.stone, positions: arch.positions, indexes: arch.indexes, st: arch.st, normals: arch.normals, flags: OSF.DETAIL, lightMask: 2, sourceId: 900 });

  // shadow pair: the same grate with and without OSF_NOSHADOW
  for (const [k, extra] of [['plain', 0], ['noshadow', OSF.NOSHADOW]]) {
    rect(sw, 0, SHADOW_GRATES[k], [SHADOW_GRATES.y[0], SHADOW_GRATES.z[0]], [SHADOW_GRATES.y[1], SHADOW_GRATES.z[1]], [1, 0, 0], MAT.grate,
      { flags: OSF.TWOSIDED | OSF.MASKED | OSF.DETAIL | extra, uvSize: 64, sourceId: k === 'plain' ? 1484 : 1485 });
  }
  // tint pairs: the same material plain and tinted, lit alike
  for (const [mat, k] of [[MAT.wall, 'wall'], [MAT.stone, 'stone']]) {
    const z = TINT_PANELS[k].z;
    rect(sw, 0, bx1 - 0.5, [TINT_PANELS.y.plain[0], z[0]], [TINT_PANELS.y.plain[1], z[1]], [-1, 0, 0], mat, { uvSize: 64 });
    rect(sw, 0, bx1 - 0.5, [TINT_PANELS.y.tinted[0], z[0]], [TINT_PANELS.y.tinted[1], z[1]], [-1, 0, 0], mat, { uvSize: 64, tint: [TINT, TINT, TINT, 1] });
  }
  // room C (no light reaches it): an unlit material and a lit one
  {
    const c = ROOM_C;
    rect(sw, 2, c.z[0], [c.x[0], c.y[0]], [c.x[1], c.y[1]], up, MAT.floor);
    rect(sw, 2, c.z[1], [c.x[0], c.y[0]], [c.x[1], c.y[1]], down, MAT.ceil);
    rect(sw, 0, c.x[0], [c.y[0], c.z[0]], [c.y[1], c.z[1]], [1, 0, 0], MAT.wall);
    rect(sw, 1, c.y[0], [c.x[0], c.z[0]], [c.x[1], c.z[1]], [0, 1, 0], MAT.wall);
    rect(sw, 1, c.y[1], [c.x[0], c.z[0]], [c.x[1], c.z[1]], [0, -1, 0], MAT.wall);
    rect(sw, 0, c.x[1], [c.y[0], c.z[0]], [c.y[1], c.z[1]], [-1, 0, 0], MAT.wall);
    rect(sw, 0, c.x[1] - 0.5, [-100, 40], [-10, 150], [-1, 0, 0], MAT.unlit, { uvSize: 64 });
    rect(sw, 0, c.x[1] - 0.5, [10, 40], [100, 150], [-1, 0, 0], MAT.stone, { uvSize: 64 });
  }

  // the door: surfaces of inline model *1 (they move with it)
  const dx = [ROOM_A[1] + 4, ROOM_B[0] - 4];
  const door = { model: 1, uvSize: 64 };
  rect(sw, 0, dx[0], [DOOR_Y[0], Z0], [DOOR_Y[1], DOOR_Z], [-1, 0, 0], MAT.stockWall, door);
  rect(sw, 0, dx[1], [DOOR_Y[0], Z0], [DOOR_Y[1], DOOR_Z], [1, 0, 0], MAT.stockWall, door);
  rect(sw, 2, DOOR_Z, [dx[0], DOOR_Y[0]], [dx[1], DOOR_Y[1]], up, MAT.stockWall, door);
  rect(sw, 2, Z0, [dx[0], DOOR_Y[0]], [dx[1], DOOR_Y[1]], down, MAT.stockWall, door);

  if (buried) {
    // inside the lowered floor slab (the nohull variant): binds to no leaf
    rect(sw, 2, -520, [-200, -200], [-100, -100], up, MAT.floor, { sourceId: 777 });
  }
  return { sw, arch };
}

export function build({ variant = 'full' } = {}) {
  const map = new MapFile({
    message: `oax test: surface world${variant === 'full' ? '' : ` (${variant})`}`,
    oax_lighting: 'unified',
    oax_ambient: x2([0.08, 0.08, 0.09]).join(' '),
    oax_shadowmode: 'maps',
    _keepLights: 1,
  });
  hull(map, variant === 'nohull' ? { floorZ: -512, solids: false } : {});

  map.entity('info_player_deathmatch', { origin: [SPAWN.x, SPAWN.y, SPAWN.z], angle: SPAWN.yaw });
  map.entity('info_player_deathmatch', { origin: [800, -300, 24], angle: 180 });
  // lights: group 1 (default) in both rooms, group 2 for the arch only
  map.entity('light', { origin: [-560, 0, 280], light_radius: [760, 760, 600], _color: x2([2.4, 2.3, 2.1]) });
  map.entity('light', { origin: [-880, 320, 180], light_radius: [420, 420, 400], _color: x2([1.8, 1.4, 1]) });
  map.entity('light', { origin: [560, -200, 280], light_radius: [760, 760, 600], _color: x2([2.1, 2.2, 2.4]) });
  map.entity('light', { origin: [ARCH.x - 140, ARCH.y - 40, 150], light_radius: [360, 360, 360], _color: x2([2.6, 2.1, 1.4]), light_mask: 2 });

  const files = { ...art(), 'scripts/oax_surfworld.shader': shaders };
  const spec = { map, light: 'none', aas: false, files, manifest: { features: ['ulight', 'surfaces', 'collision'], lighting: 'unified' } };
  if (variant !== 'nosurf') {
    const { sw, arch } = surfaces({ buried: variant === 'nohull' });
    spec.surfaces = sw;
    spec.expectUnbound = variant === 'nohull' ? 1 : 0;
    const coll = new CollisionMeshes();
    coll.mesh({ positions: arch.collision.positions, indexes: arch.collision.indexes, thickness: 4, sourceId: 900 });
    spec.collision = coll;
  }
  return spec;
}
