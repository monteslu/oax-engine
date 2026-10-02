// oaxterrain.mjs: a JS port of the OAX_TERRAIN reader and the deterministic
// foliage placement (code/qcommon/oax_terrain.c), float32 step by step, so
// tests can predict exactly how many grass and tree instances the engine
// places (renderer) and how many trunks collide (collision).

import fs from 'node:fs';
import { readBspx } from '../../../misc/tools/bspx.mjs';

const f = Math.fround;

export function readTerrainLump(bspFile) {
  const { lumps } = readBspx(fs.readFileSync(bspFile));
  const l = lumps.find((x) => x.name === 'OAX_TERRAIN');
  if (!l) return null;
  const b = Buffer.from(l.data);
  if (b.toString('latin1', 0, 4) !== 'OTRN') throw new Error('bad OAX_TERRAIN');
  const n = b.readInt32LE(8);
  const out = [];
  let at = 16;
  for (let k = 0; k < n; k++) {
    const r = b.subarray(at);
    const t = {
      size: r.readInt32LE(0),
      origin: [r.readFloatLE(4), r.readFloatLE(8), r.readFloatLE(12)],
      cellSize: r.readFloatLE(16), sx: r.readInt32LE(20), sy: r.readInt32LE(24),
      heightScale: r.readFloatLE(28), bottom: r.readFloatLE(32),
      seed: r.readInt32LE(320), numFoliage: r.readInt32LE(324), foliage: [],
    };
    for (let i = 0; i < t.numFoliage; i++) {
      const p = 328 + i * 116;
      t.foliage.push({
        kind: r.readInt32LE(p), channel: r.readInt32LE(p + 68) & 3, density: r.readFloatLE(p + 72),
        sizeMin: r.readFloatLE(p + 76), sizeMax: r.readFloatLE(p + 80), fadeStart: r.readFloatLE(p + 84), fadeEnd: r.readFloatLE(p + 88),
        collideRadius: r.readFloatLE(p + 92), collideHeight: r.readFloatLE(p + 96), maxSlope: r.readFloatLE(p + 100),
      });
    }
    const ho = r.readInt32LE(792), dofs = r.readInt32LE(800);
    t.heights = new Uint16Array(t.sx * t.sy);
    for (let i = 0; i < t.sx * t.sy; i++) t.heights[i] = r.readUInt16LE(ho + i * 2);
    t.density = new Uint8Array(r.subarray(dofs, dofs + t.sx * t.sy * 4));
    out.push(t);
    at += (t.size + 3) & ~3;
  }
  return out;
}

function sampleZ(t, i, j) {
  i = Math.max(0, Math.min(t.sx - 1, i));
  j = Math.max(0, Math.min(t.sy - 1, j));
  return f(t.origin[2] + f(t.heights[j * t.sx + i] * t.heightScale));
}

function hash32(x) {
  x >>>= 0;
  x ^= x >>> 16;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x ^= x >>> 15;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x ^= x >>> 16;
  return x >>> 0;
}

// the normal z of the triangle under (u, v) in cell (i, j)
function normalZ(t, i, j, u, v) {
  const x0 = f(t.origin[0] + f(i * t.cellSize)), y0 = f(t.origin[1] + f(j * t.cellSize));
  const x1 = f(t.origin[0] + f((i + 1) * t.cellSize)), y1 = f(t.origin[1] + f((j + 1) * t.cellSize));
  const za = sampleZ(t, i, j), zb = sampleZ(t, i + 1, j), zc = sampleZ(t, i, j + 1), zd = sampleZ(t, i + 1, j + 1);
  const tri = u >= v ? [[x0, y0, za], [x1, y0, zb], [x1, y1, zd]] : [[x0, y0, za], [x1, y1, zd], [x0, y1, zc]];
  const e1 = [f(tri[1][0] - tri[0][0]), f(tri[1][1] - tri[0][1]), f(tri[1][2] - tri[0][2])];
  const e2 = [f(tri[2][0] - tri[0][0]), f(tri[2][1] - tri[0][1]), f(tri[2][2] - tri[0][2])];
  const n = [f(f(e1[1] * e2[2]) - f(e1[2] * e2[1])), f(f(e1[2] * e2[0]) - f(e1[0] * e2[2])), f(f(e1[0] * e2[1]) - f(e1[1] * e2[0]))];
  const l = f(Math.sqrt(f(f(f(n[0] * n[0]) + f(n[1] * n[1])) + f(n[2] * n[2]))));
  return l > 0 ? f(n[2] / l) : 1;
}

// the number of instances of foliage type fi in cell (i, j)
export function cellFoliageCount(t, fi, i, j) {
  const fd = t.foliage[fi], c = fd.channel, D = t.density, sx = t.sx;
  const sum = D[(j * sx + i) * 4 + c] + D[(j * sx + i + 1) * 4 + c] + D[((j + 1) * sx + i) * 4 + c] + D[((j + 1) * sx + i + 1) * 4 + c];
  if (!sum || !(fd.density > 0)) return 0;
  let state = hash32((t.seed >>> 0) ^ hash32((Math.imul(fi, 73856093) ^ Math.imul(i, 19349663) ^ Math.imul(j, 83492791)) >>> 0));
  const rand = () => { state = hash32((state + 0x9e3779b9) >>> 0); return f((state >>> 8) * f(1 / 16777216)); };
  const expected = f(f(f(sum) * f(1 / 1020)) * fd.density);
  let n = Math.trunc(expected);
  if (rand() < f(expected - n)) n++;
  n = Math.min(n, 16);
  let count = 0;
  for (let k = 0; k < n; k++) {
    const u = rand(), v = rand();
    rand(); rand();
    if (normalZ(t, i, j, u, v) < fd.maxSlope) continue;
    count++;
  }
  return count;
}

// instances per foliage type over the whole terrain
export function foliageCounts(t) {
  return t.foliage.map((fd, fi) => {
    let n = 0;
    for (let j = 0; j < t.sy - 1; j++) for (let i = 0; i < t.sx - 1; i++) n += cellFoliageCount(t, fi, i, j);
    return n;
  });
}
