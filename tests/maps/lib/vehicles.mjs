// vehicles.mjs: the vehicle models (phase 8) as MD3 files, their shaders,
// and the info_oax_vehicle spawner entity, for map sources that place
// vehicles (oax_vehicle_test, oax_outdoor_vctf). Our own geometry: boxes
// and cylinders with OpenArena textures; the sizes match the gamecode's
// vehicle table (oa-gamecode code/game/bg_oax_vehicle.c).
//
// Body space as in the game: x forward, y left, z up, origin at the chassis
// center. Triangles are wound clockwise seen from outside (Q3's front face).

export const VEHICLE_TYPES = {
  buggy: { half: [72, 40, 14], wheels: [[52, 44, -6], [52, -44, -6], [-52, 44, -6], [-52, -44, -6]], wheelRadius: 20, rest: 26 },
  hover: { half: [64, 36, 10], thrusters: [[48, 30, -10], [48, -30, -10], [-48, 30, -10], [-48, -30, -10]], hover: 36 },
};

// ---- a tiny mesh builder ----------------------------------------------------

class Mesh {
  constructor(shader) { this.shader = shader; this.v = []; this.n = []; this.st = []; this.tri = []; }
  // one triangle, outward normal `n` for every corner (flat shading), uv per corner
  face(pts, uvs, n) {
    const base = this.v.length;
    for (let k = 0; k < pts.length; k++) { this.v.push(pts[k]); this.n.push(n); this.st.push(uvs[k]); }
    for (let k = 1; k + 1 < pts.length; k++) {
      let a = base, b = base + k, c = base + k + 1;
      // clockwise from outside: (b-a)x(c-a) points inward
      const u = sub(this.v[b], this.v[a]), w = sub(this.v[c], this.v[a]);
      if (dot(cross(u, w), n) > 0) [b, c] = [c, b];
      this.tri.push([a, b, c]);
    }
  }
  box(min, max, uvScale = 1 / 64) {
    const [x0, y0, z0] = min, [x1, y1, z1] = max;
    const quad = (pts, n, ua, va) => this.face(pts, pts.map((p) => [p[ua] * uvScale, -p[va] * uvScale]), n);
    quad([[x1, y0, z0], [x1, y1, z0], [x1, y1, z1], [x1, y0, z1]], [1, 0, 0], 1, 2);
    quad([[x0, y0, z0], [x0, y0, z1], [x0, y1, z1], [x0, y1, z0]], [-1, 0, 0], 1, 2);
    quad([[x0, y1, z0], [x0, y1, z1], [x1, y1, z1], [x1, y1, z0]], [0, 1, 0], 0, 2);
    quad([[x0, y0, z0], [x1, y0, z0], [x1, y0, z1], [x0, y0, z1]], [0, -1, 0], 0, 2);
    quad([[x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]], [0, 0, 1], 0, 1);
    quad([[x0, y0, z0], [x0, y1, z0], [x1, y1, z0], [x1, y0, z0]], [0, 0, -1], 0, 1);
    return this;
  }
  // a convex solid from its 8 corners (a box with a tapered top): faces as
  // corner index quads, normals from the geometry
  hexa(c) {
    const quads = [[1, 2, 6, 5], [0, 4, 7, 3], [3, 7, 6, 2], [0, 1, 5, 4], [4, 5, 6, 7], [0, 3, 2, 1]];
    const center = c.reduce((a, p) => add(a, p), [0, 0, 0]).map((v) => v / 8);
    for (const q of quads) {
      const pts = q.map((i) => c[i]);
      let n = norm(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0])));
      if (dot(n, sub(pts[0], center)) < 0) n = n.map((v) => -v);
      const ax = Math.abs(n[0]) > 0.7 ? [1, 2] : Math.abs(n[1]) > 0.7 ? [0, 2] : [0, 1];
      this.face(pts, pts.map((p) => [p[ax[0]] / 64, -p[ax[1]] / 64]), n);
    }
    return this;
  }
  // a cylinder along y: tread and two caps
  cylinder(r, halfW, sides = 16) {
    for (let k = 0; k < sides; k++) {
      const a0 = (k / sides) * Math.PI * 2, a1 = ((k + 1) / sides) * Math.PI * 2, am = (a0 + a1) / 2;
      const p = (a, y) => [Math.cos(a) * r, y, Math.sin(a) * r];
      this.face([p(a0, -halfW), p(a1, -halfW), p(a1, halfW), p(a0, halfW)],
        [[k / sides * 4, 0], [(k + 1) / sides * 4, 0], [(k + 1) / sides * 4, 0.5], [k / sides * 4, 0.5]], [Math.cos(am), 0, Math.sin(am)]);
      for (const y of [-halfW, halfW]) {
        const n = [0, Math.sign(y), 0];
        const tri = [[0, y, 0], p(a0, y), p(a1, y)];
        this.face(tri, tri.map((q) => [0.5 + q[0] / (2 * r), 0.5 + q[2] / (2 * r)]), n);
      }
    }
    return this;
  }
}

const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm = (a) => { const l = Math.hypot(...a) || 1; return a.map((v) => v / l); };

// ---- MD3 ---------------------------------------------------------------------

function cstr(buf, off, s, len) { buf.fill(0, off, off + len); buf.write(s.slice(0, len - 1), off, 'latin1'); }

// Q3 normal encoding: lat = atan2(y, x), lng = acos(z), 256 steps per turn
function encodeNormal(n) {
  const [x, y, z] = norm(n);
  if (x === 0 && y === 0) return z > 0 ? 0 : (128 << 0);
  const lng = Math.round(Math.acos(Math.max(-1, Math.min(1, z))) * 255 / (2 * Math.PI)) & 0xff;
  const lat = Math.round(Math.atan2(y, x) * 255 / (2 * Math.PI)) & 0xff;
  return (lat << 8) | lng;
}

export function writeMD3(name, meshes) {
  const surfs = meshes.map((m, i) => {
    const nv = m.v.length, nt = m.tri.length;
    const size = 108 + 68 + nt * 12 + nv * 8 + nv * 8;
    const b = Buffer.alloc(size);
    b.write('IDP3', 0, 'latin1');
    cstr(b, 4, `s${i}`, 64);
    let o = 68;
    const ofsShaders = 108, ofsTris = ofsShaders + 68, ofsSt = ofsTris + nt * 12, ofsXyz = ofsSt + nv * 8;
    for (const v of [0, 1, 1, nv, nt, ofsTris, ofsShaders, ofsSt, ofsXyz, size]) { b.writeInt32LE(v, o); o += 4; }
    cstr(b, ofsShaders, m.shader, 64);
    b.writeInt32LE(0, ofsShaders + 64);
    m.tri.forEach((t, k) => t.forEach((v, j) => b.writeInt32LE(v, ofsTris + k * 12 + j * 4)));
    m.st.forEach((st, k) => { b.writeFloatLE(st[0], ofsSt + k * 8); b.writeFloatLE(st[1], ofsSt + k * 8 + 4); });
    m.v.forEach((v, k) => {
      for (let j = 0; j < 3; j++) b.writeInt16LE(Math.round(v[j] * 64), ofsXyz + k * 8 + j * 2);
      b.writeUInt16LE(encodeNormal(m.n[k]), ofsXyz + k * 8 + 6);
    });
    return b;
  });
  const all = meshes.flatMap((m) => m.v);
  const mins = [0, 1, 2].map((j) => Math.min(...all.map((v) => v[j])));
  const maxs = [0, 1, 2].map((j) => Math.max(...all.map((v) => v[j])));
  const radius = Math.max(...all.map((v) => Math.hypot(...v)));
  const head = Buffer.alloc(108), frame = Buffer.alloc(56);
  head.write('IDP3', 0, 'latin1');
  head.writeInt32LE(15, 4);
  cstr(head, 8, name, 64);
  const ofsFrames = 108, ofsSurfaces = ofsFrames + 56;
  const end = ofsSurfaces + surfs.reduce((a, s) => a + s.length, 0);
  let o = 72;
  for (const v of [0, 1, 0, surfs.length, 0, ofsFrames, ofsSurfaces, ofsSurfaces, end]) { head.writeInt32LE(v, o); o += 4; }
  mins.forEach((v, j) => frame.writeFloatLE(v, j * 4));
  maxs.forEach((v, j) => frame.writeFloatLE(v, 12 + j * 4));
  frame.writeFloatLE(radius, 36);
  cstr(frame, 40, 'frame0', 16);
  return Buffer.concat([head, frame, ...surfs]);
}

// ---- the models -------------------------------------------------------------------

const SH = {
  body: 'models/oax/vehicles/body',
  dark: 'models/oax/vehicles/dark',
  hover: 'models/oax/vehicles/hover',
  glow: 'models/oax/vehicles/glow',
};

function buggy() {
  const [hx, hy, hz] = VEHICLE_TYPES.buggy.half;
  const body = new Mesh(SH.body), dark = new Mesh(SH.dark);
  // tub: a box with a sloped nose
  body.hexa([[-hx, -hy, -hz], [hx, -hy, -hz + 4], [hx, hy, -hz + 4], [-hx, hy, -hz],
    [-hx, -hy, hz - 4], [hx - 20, -hy, hz], [hx - 20, hy, hz], [-hx, hy, hz - 4]]);
  // rear deck for the gunner
  body.box([-hx, -hy + 4, hz - 4], [-20, hy - 4, hz + 2]);
  // roll cage around the driver: four posts and a frame
  for (const [x, y] of [[-14, 34], [-14, -34], [34, 34], [34, -34]]) dark.box([x - 2, y - 2, hz], [x + 2, y + 2, hz + 40]);
  dark.box([-16, -36, hz + 40], [36, -32, hz + 44]);
  dark.box([-16, 32, hz + 40], [36, 36, hz + 44]);
  dark.box([-16, -36, hz + 40], [-12, 36, hz + 44]);
  dark.box([32, -36, hz + 40], [36, 36, hz + 44]);
  // bumpers and seat
  dark.box([hx - 4, -hy, -hz], [hx + 6, hy, -hz + 10]);
  dark.box([-hx - 6, -hy, -hz], [-hx + 2, hy, -hz + 10]);
  dark.box([-6, 4, hz], [10, 28, hz + 8]);
  return writeMD3('models/oax/vehicles/buggy.md3', [body, dark]);
}

function wheel() {
  const t = VEHICLE_TYPES.buggy;
  return writeMD3('models/oax/vehicles/wheel.md3', [new Mesh(SH.dark).cylinder(t.wheelRadius, 7, 16)]);
}

function hover() {
  const [hx, hy, hz] = VEHICLE_TYPES.hover.half;
  const hull = new Mesh(SH.hover), glow = new Mesh(SH.glow), dark = new Mesh(SH.dark);
  // a wedge: low nose, high tail
  hull.hexa([[-hx, -hy, -hz], [hx, -hy + 12, -hz], [hx, hy - 12, -hz], [-hx, hy, -hz],
    [-hx, -hy, hz], [hx, -hy + 12, -hz + 6], [hx, hy - 12, -hz + 6], [-hx, hy, hz]]);
  // tail fin and seat
  dark.box([-hx, -3, hz], [-hx + 28, 3, hz + 26]);
  dark.box([-20, -10, hz], [4, 10, hz + 8]);
  // thruster pads under the hull
  for (const [x, y] of VEHICLE_TYPES.hover.thrusters) glow.box([x - 10, y - 8, -hz - 4], [x + 10, y + 8, -hz]);
  return writeMD3('models/oax/vehicles/hover.md3', [hull, dark, glow]);
}

export const VEHICLE_SHADERS = `// oax vehicle models (tests/maps/lib/vehicles.mjs)
${SH.body}
{
	{
		map textures/base_floor/diamond2c.jpg
		rgbGen lightingDiffuse
	}
}

${SH.dark}
{
	{
		map textures/base_floor/clangdark.jpg
		rgbGen lightingDiffuse
	}
}

${SH.hover}
{
	{
		map textures/base_floor/metaltechfloor01final.jpg
		rgbGen lightingDiffuse
	}
}

${SH.glow}
{
	cull none
	{
		map textures/base_light/geolight_glow.jpg
		rgbGen identity
	}
}
`;

// the files a map spec adds so its vehicles have models
export function vehicleFiles() {
  return {
    'models/oax/vehicles/buggy.md3': buggy(),
    'models/oax/vehicles/wheel.md3': wheel(),
    'models/oax/vehicles/hover.md3': hover(),
    'scripts/oax_vehicles.shader': VEHICLE_SHADERS,
  };
}

// a spawner: origin is the chassis center (put it a little above the ground)
export function addVehicle(map, type, origin, angle = 0, wait = 20) {
  map.entity('info_oax_vehicle', { origin, angle, type, wait });
}
