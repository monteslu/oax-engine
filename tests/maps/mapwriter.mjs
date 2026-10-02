// mapwriter.mjs: write Quake III .map files from JS, for the engine's test
// maps. Brushes are convex solids given as outward planes; boxes and rooms
// cover most test geometry.
//
// Plane points follow q3map2's rule: for points p1, p2, p3 the plane normal
// is cross(p3 - p1, p2 - p1), and it must point OUT of the solid. A brush
// self-check intersects every plane triple and fails if a face has fewer
// than 3 vertices on the hull (a degenerate or inverted brush).

const EPS = 0.01;

const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const scale = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const len = (a) => Math.sqrt(dot(a, a));
const norm = (a) => scale(a, 1 / len(a));

function fmt(n) {
  const r = Math.round(n * 1000) / 1000;
  return Object.is(r, -0) ? '0' : String(r);
}

// Three points on the plane {n, d} (n.x = d) in q3map2's winding.
function planePoints(n, d) {
  const p1 = scale(n, d);
  // a tangent basis (a, b) with cross(b, a) = n
  const helper = Math.abs(n[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0];
  const a = norm(cross(n, helper));
  const b = cross(a, n);           // cross(b, a) = cross(cross(a, n), a) = n
  const s = 64;
  return [p1, add(p1, scale(a, s)), add(p1, scale(b, s))];
}

export class Face {
  constructor(normal, dist, texture = 'common/caulk', { xoff = 0, yoff = 0, rot = 0, sx = 0.5, sy = 0.5 } = {}) {
    this.n = norm(normal);
    this.d = dist / len(normal);
    this.texture = texture;
    this.tex = { xoff, yoff, rot, sx, sy };
  }

  toString() {
    const [p1, p2, p3] = planePoints(this.n, this.d);
    // check the rule held: cross(p3 - p1, p2 - p1) points along n
    const got = cross(sub(p3, p1), sub(p2, p1));
    if (dot(got, this.n) <= 0) throw new Error('mapwriter: plane winding came out inverted');
    const pt = (p) => `( ${p.map(fmt).join(' ')} )`;
    const t = this.tex;
    return `${pt(p1)} ${pt(p2)} ${pt(p3)} ${this.texture} ${fmt(t.xoff)} ${fmt(t.yoff)} ${fmt(t.rot)} ${fmt(t.sx)} ${fmt(t.sy)} 0 0 0`;
  }
}

export class Brush {
  constructor(faces) {
    this.faces = faces;
  }

  // Vertices of the convex hull: every plane triple's intersection that is
  // inside all other planes.
  vertices() {
    const out = [];
    const f = this.faces;
    for (let i = 0; i < f.length; i++) for (let j = i + 1; j < f.length; j++) for (let k = j + 1; k < f.length; k++) {
      const a = f[i], b = f[j], c = f[k];
      const det = dot(a.n, cross(b.n, c.n));
      if (Math.abs(det) < 1e-9) continue;
      const p = scale(add(add(scale(cross(b.n, c.n), a.d), scale(cross(c.n, a.n), b.d)), scale(cross(a.n, b.n), c.d)), 1 / det);
      if (f.every((g) => dot(g.n, p) - g.d <= EPS)) out.push(p);
    }
    return out;
  }

  check() {
    const verts = this.vertices();
    for (const face of this.faces) {
      const on = verts.filter((p) => Math.abs(dot(face.n, p) - face.d) <= EPS);
      if (on.length < 3) throw new Error(`mapwriter: degenerate brush face (normal ${face.n.map(fmt).join(' ')})`);
    }
    return this;
  }

  toString() {
    return `{\n${this.faces.map((f) => f.toString()).join('\n')}\n}`;
  }
}

// An axis-aligned box. tex: one texture name, or {top, bottom, sides, px, nx, py, ny}.
export function box(mins, maxs, tex = 'common/caulk', opts = {}) {
  const t = typeof tex === 'string' ? {} : tex;
  const def = typeof tex === 'string' ? tex : (t.sides || 'common/caulk');
  const pick = (k, side) => t[k] || (side ? t.sides : null) || def;
  return new Brush([
    new Face([0, 0, 1], maxs[2], pick('top'), opts),
    new Face([0, 0, -1], -mins[2], pick('bottom'), opts),
    new Face([1, 0, 0], maxs[0], pick('px', true), opts),
    new Face([-1, 0, 0], -mins[0], pick('nx', true), opts),
    new Face([0, 1, 0], maxs[1], pick('py', true), opts),
    new Face([0, -1, 0], -mins[1], pick('ny', true), opts),
  ]).check();
}

// A hollow room: six wall slabs around [mins, maxs] (the inside), each
// `wall` thick. Walls overlap at the corners so the room never leaks.
export function room(mins, maxs, tex, wall = 16) {
  const t = typeof tex === 'string' ? { floor: tex, ceiling: tex, walls: tex } : tex;
  const [x0, y0, z0] = mins, [x1, y1, z1] = maxs, w = wall;
  return [
    box([x0 - w, y0 - w, z0 - w], [x1 + w, y1 + w, z0], { top: t.floor, sides: 'common/caulk', bottom: 'common/caulk' }),
    box([x0 - w, y0 - w, z1], [x1 + w, y1 + w, z1 + w], { bottom: t.ceiling, sides: 'common/caulk', top: 'common/caulk' }),
    box([x0 - w, y0 - w, z0], [x0, y1 + w, z1], { px: t.walls, sides: 'common/caulk' }),
    box([x1, y0 - w, z0], [x1 + w, y1 + w, z1], { nx: t.walls, sides: 'common/caulk' }),
    box([x0, y0 - w, z0], [x1, y0, z1], { py: t.walls, sides: 'common/caulk' }),
    box([x0, y1, z0], [x1, y1 + w, z1], { ny: t.walls, sides: 'common/caulk' }),
  ];
}

export class Entity {
  constructor(classname, keys = {}, brushes = []) {
    this.keys = { classname, ...keys };
    this.brushes = brushes;
  }

  toString() {
    const kv = Object.entries(this.keys).map(([k, v]) => {
      const val = Array.isArray(v) ? v.map(fmt).join(' ') : typeof v === 'number' ? fmt(v) : String(v);
      if (/["\n]/.test(val) || /["\n]/.test(k)) throw new Error(`mapwriter: key ${k} has a quote or newline`);
      return `"${k}" "${val}"`;
    });
    return `{\n${kv.join('\n')}\n${this.brushes.map(String).join('\n')}${this.brushes.length ? '\n' : ''}}`;
  }
}

export class MapFile {
  constructor(worldKeys = {}) {
    this.world = new Entity('worldspawn', worldKeys);
    this.entities = [];
  }

  brush(...brushes) {
    this.world.brushes.push(...brushes.flat());
    return this;
  }

  entity(classname, keys = {}, brushes = []) {
    const e = new Entity(classname, keys, brushes.flat());
    this.entities.push(e);
    return e;
  }

  toString() {
    return [this.world, ...this.entities].map(String).join('\n') + '\n';
  }
}

// A bezier patch (patchDef2): rows x cols control points [x, y, z, s, t],
// both counts odd and >= 3. q3map2 makes it a curved surface; with a solid
// shader it also collides (and the physics module tessellates it).
export class Patch {
  constructor(grid, texture = 'base_floor/clang_floor') {
    const rows = grid.length, cols = grid[0].length;
    if (rows < 3 || cols < 3 || !(rows & 1) || !(cols & 1) || grid.some((r) => r.length !== cols)) {
      throw new Error('mapwriter: a patch needs an odd grid of at least 3 x 3');
    }
    this.grid = grid;
    this.texture = texture;
  }

  toString() {
    const rows = this.grid.map((row) => `( ${row.map((p) => `( ${p.map(fmt).join(' ')} )`).join(' ')} )`);
    return `{\npatchDef2\n{\n${this.texture}\n( ${this.grid.length} ${this.grid[0].length} 0 0 0 )\n(\n${rows.join('\n')}\n)\n}\n}`;
  }
}
