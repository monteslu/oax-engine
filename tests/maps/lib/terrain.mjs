// terrain.mjs: build a misc_oax_terrain for a test map: the heightmap,
// splat and density images (written through the map spec's `files`), the
// entity keys, and the same height lookup the engine uses, so a map source
// can put spawns, items and bases on the ground.
//
// Heights are integers (uint16); world z = origin z + h * heightScale, and a
// cell is split along its (i,j)-(i+1,j+1) diagonal (code/qcommon/oax_terrain.h).

import { writePGM16, writePAM } from '../../../misc/tools/oax-terrain.mjs';

export class Terrain {
  // origin: world position of sample (0,0) at height 0
  constructor({ name, origin, cellSize, samplesX, samplesY, heightScale }) {
    Object.assign(this, { name, origin, cellSize, samplesX, samplesY, heightScale });
    this.h = new Uint16Array(samplesX * samplesY);
    this.splat = new Uint8Array(samplesX * samplesY * 4);
    this.density = new Uint8Array(samplesX * samplesY * 4);
    for (let k = 0; k < samplesX * samplesY; k++) this.splat[k * 4] = 255;
  }

  get size() { return [(this.samplesX - 1) * this.cellSize, (this.samplesY - 1) * this.cellSize]; }

  // world xy of sample (i, j)
  xy(i, j) { return [this.origin[0] + i * this.cellSize, this.origin[1] + j * this.cellSize]; }

  // set heights from fn(x, y) -> world z (rounded to the height step)
  shape(fn) {
    for (let j = 0; j < this.samplesY; j++) {
      for (let i = 0; i < this.samplesX; i++) {
        const [x, y] = this.xy(i, j);
        const h = Math.round((fn(x, y) - this.origin[2]) / this.heightScale);
        this.h[j * this.samplesX + i] = Math.max(0, Math.min(65535, h));
      }
    }
    return this;
  }

  sampleZ(i, j) {
    i = Math.max(0, Math.min(this.samplesX - 1, i));
    j = Math.max(0, Math.min(this.samplesY - 1, j));
    return Math.fround(this.origin[2] + Math.fround(this.h[j * this.samplesX + i] * this.heightScale));
  }

  // surface z at world (x, y) on the collision triangles
  heightAt(x, y) {
    const fx = (x - this.origin[0]) / this.cellSize, fy = (y - this.origin[1]) / this.cellSize;
    const i = Math.min(this.samplesX - 2, Math.max(0, Math.floor(fx))), j = Math.min(this.samplesY - 2, Math.max(0, Math.floor(fy)));
    const u = fx - i, v = fy - j;
    const za = this.sampleZ(i, j), zb = this.sampleZ(i + 1, j), zc = this.sampleZ(i, j + 1), zd = this.sampleZ(i + 1, j + 1);
    return u >= v ? za + u * (zb - za) + v * (zd - zb) : za + u * (zd - zc) + v * (zc - za);
  }

  // highest surface point within radius r of (x, y): where a player stands
  groundZ(x, y, r = 24) {
    let z = -Infinity;
    for (const [dx, dy] of [[0, 0], [r, 0], [-r, 0], [0, r], [0, -r], [r, r], [-r, -r], [r, -r], [-r, r]]) z = Math.max(z, this.heightAt(x + dx, y + dy));
    return z;
  }

  // per-sample paint: fn(x, y, z, slope) -> {splat: [4], density: [4]}
  paint(fn) {
    for (let j = 0; j < this.samplesY; j++) {
      for (let i = 0; i < this.samplesX; i++) {
        const [x, y] = this.xy(i, j);
        const z = this.sampleZ(i, j);
        const dzx = (this.sampleZ(i + 1, j) - this.sampleZ(i - 1, j)) / (2 * this.cellSize);
        const dzy = (this.sampleZ(i, j + 1) - this.sampleZ(i, j - 1)) / (2 * this.cellSize);
        const slope = 1 / Math.sqrt(1 + dzx * dzx + dzy * dzy);   // normal z
        const p = fn(x, y, z, slope);
        const k = (j * this.samplesX + i) * 4;
        if (p.splat) {
          const s = p.splat.reduce((a, b) => a + b, 0) || 1;
          let acc = 0;
          for (let c = 0; c < 4; c++) { const v = c < 3 ? Math.round(255 * p.splat[c] / s) : 255 - acc; this.splat[k + c] = Math.max(0, Math.min(255, v)); acc += this.splat[k + c]; }
        }
        if (p.density) for (let c = 0; c < 4; c++) this.density[k + c] = Math.max(0, Math.min(255, Math.round(p.density[c] * 255)));
      }
    }
    return this;
  }

  bounds() {
    let lo = Infinity, hi = -Infinity;
    for (const v of this.h) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
    return { minZ: this.origin[2] + lo * this.heightScale, maxZ: this.origin[2] + hi * this.heightScale };
  }

  // files for the map spec and the entity keys
  files() {
    return {
      [`maps/${this.name}_h.pgm`]: writePGM16(this.samplesX, this.samplesY, this.h),
      [`maps/${this.name}_s.pam`]: writePAM(this.samplesX, this.samplesY, this.splat),
      [`maps/${this.name}_d.pam`]: writePAM(this.samplesX, this.samplesY, this.density),
    };
  }

  entityKeys({ layers = [], foliage = [], seed = 1, bottom } = {}) {
    const k = {
      origin: this.origin, cellsize: this.cellSize, heightscale: String(this.heightScale),   // full precision (numbers are written with 3 decimals)
      heightmap: `maps/${this.name}_h.pgm`, splatmap: `maps/${this.name}_s.pam`, densitymap: `maps/${this.name}_d.pam`,
      foliageseed: seed,
    };
    if (bottom !== undefined) k.bottom = bottom;
    layers.forEach((l, i) => { k[`layer${i}`] = l.shader; k[`layerscale${i}`] = l.scale; });
    foliage.forEach((f, i) => { k[`foliage${i}`] = f; });
    return k;
  }
}
