// terrain.mjs: the terrains of a compiled map, read from its OAX_TERRAIN
// BSPX lump (layout: code/qcommon/oax_terrain.h), with the engine's height
// lookup (tests/maps/lib/terrain.mjs) for checks against player positions.

import { readBspx } from '../../../misc/tools/bspx.mjs';
import { Terrain } from '../../maps/lib/terrain.mjs';

export function terrainFromBsp(buf) {
  const lump = readBspx(buf).lumps.find((l) => l.name === 'OAX_TERRAIN');
  if (!lump) return [];
  const b = Buffer.from(lump.data);
  if (b.toString('latin1', 0, 4) !== 'OTRN') throw new Error('bad OAX_TERRAIN');
  const n = b.readInt32LE(8);
  const out = [];
  let at = 16;
  for (let k = 0; k < n; k++) {
    const size = b.readInt32LE(at);
    const t = new Terrain({
      name: `terrain${k}`,
      origin: [b.readFloatLE(at + 4), b.readFloatLE(at + 8), b.readFloatLE(at + 12)],
      cellSize: b.readFloatLE(at + 16),
      samplesX: b.readInt32LE(at + 20),
      samplesY: b.readInt32LE(at + 24),
      heightScale: b.readFloatLE(at + 28),
    });
    t.bottom = b.readFloatLE(at + 32);
    const ho = b.readInt32LE(at + 792);
    for (let i = 0; i < t.samplesX * t.samplesY; i++) t.h[i] = b.readUInt16LE(at + ho + i * 2);
    const inside = (x, y) => {
      const fx = (x - t.origin[0]) / t.cellSize, fy = (y - t.origin[1]) / t.cellSize;
      return fx >= 0 && fy >= 0 && fx <= t.samplesX - 1 && fy <= t.samplesY - 1;
    };
    const base = t.heightAt.bind(t);
    t.heightAt = (x, y) => (inside(x, y) ? base(x, y) : null);
    t.groundZ = (x, y, r = 24) => {
      let z = -Infinity;
      for (const [dx, dy] of [[0, 0], [r, 0], [-r, 0], [0, r], [0, -r], [r, r], [-r, -r], [r, -r], [-r, r]]) {
        const h = t.heightAt(x + dx, y + dy);
        if (h !== null) z = Math.max(z, h);
      }
      return z;
    };
    const dOfs = b.readInt32LE(at + 800);
    t.density = new Uint8Array(b.subarray(at + dOfs, at + dOfs + t.samplesX * t.samplesY * 4));
    t.seed = b.readInt32LE(at + 320);
    t.foliage = [];
    for (let f = 0; f < b.readInt32LE(at + 324); f++) {
      const p = at + 328 + f * 116;
      const fl = (o) => b.readFloatLE(p + o);
      t.foliage.push({ kind: b.readInt32LE(p), shader: b.toString('latin1', p + 4, p + 68).replace(/\0.*$/s, ''), channel: b.readInt32LE(p + 68) & 3,
        density: fl(72), sizeMin: fl(76), sizeMax: fl(80), fadeStart: fl(84), fadeEnd: fl(88), collideRadius: fl(92), collideHeight: fl(96), maxSlope: fl(100) });
    }
    t.cellFoliage = (f, i, j) => cellFoliage(t, f, i, j);
    out.push(t);
    at += (size + 3) & ~3;
  }
  return out;
}

// JS port of OAXTerrain_CellFoliage (code/qcommon/oax_terrain.c), float32
// rounding at every step like the C
const F = Math.fround;
function hash32(x) {
  x >>>= 0;
  x ^= x >>> 16; x = Math.imul(x, 0x7feb352d) >>> 0;
  x ^= x >>> 15; x = Math.imul(x, 0x846ca68b) >>> 0;
  x ^= x >>> 16;
  return x >>> 0;
}
export function cellFoliage(t, f, i, j) {
  const fd = t.foliage[f];
  const sx = t.samplesX, c = fd.channel, d = t.density;
  const sum = d[(j * sx + i) * 4 + c] + d[(j * sx + i + 1) * 4 + c] + d[((j + 1) * sx + i) * 4 + c] + d[((j + 1) * sx + i + 1) * 4 + c];
  if (!sum || !(fd.density > 0)) return [];
  let state = hash32((t.seed >>> 0) ^ hash32((Math.imul(f, 73856093) ^ Math.imul(i, 19349663) ^ Math.imul(j, 83492791)) >>> 0));
  const rand = () => { state = hash32((state + 0x9e3779b9) >>> 0); return F((state >>> 8) * (1 / 16777216)); };
  const expected = F(F(F(sum) * F(1 / 1020)) * F(fd.density));
  let n = Math.trunc(expected);
  if (rand() < F(expected - n)) n++;
  n = Math.min(n, 16);
  const out = [];
  for (let k = 0; k < n; k++) {
    const u = rand(), v = rand(), yaw = rand(), sc = rand();
    const x = F(t.origin[0] + F(F(i + u) * t.cellSize)), y = F(t.origin[1] + F(F(j + v) * t.cellSize));
    out.push({ x, y, z: t.heightAt(x, y), yaw: F(yaw * 360), scale: F(fd.sizeMin + F(sc * F(fd.sizeMax - fd.sizeMin))) });
  }
  return out;   // (instances on slopes steeper than maxSlope are dropped by the engine; not filtered here)
}
export function allFoliage(t, f) {
  const out = [];
  for (let j = 0; j < t.samplesY - 1; j++) for (let i = 0; i < t.samplesX - 1; i++) out.push(...cellFoliage(t, f, i, j));
  return out;
}
