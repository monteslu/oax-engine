// texgen.mjs: procedural art for the engine's test maps (our own, so no
// licensing questions): TGA writer, tileable height fields, normal maps
// derived from a height field (the heightmap() image program's math), and
// a few patterns (bricks, floor tiles, a projection gobo).
//
// Normal maps are tangent space with +x along increasing s (image right)
// and +y along increasing t (image down), z out of the surface, stored as
// RGB = n * 0.5 + 0.5. That matches renderergl2's tangent frame.

export function tga(width, height, rgba) {
  const head = Buffer.alloc(18);
  head[2] = 2;                       // uncompressed true colour
  head.writeUInt16LE(width, 12);
  head.writeUInt16LE(height, 14);
  head[16] = 32;
  head[17] = 0x28;                   // 8 alpha bits, top-left origin
  const px = Buffer.alloc(width * height * 4);
  for (let i = 0; i < width * height; i++) {
    px[i * 4] = rgba[i * 4 + 2];     // TGA stores BGRA
    px[i * 4 + 1] = rgba[i * 4 + 1];
    px[i * 4 + 2] = rgba[i * 4];
    px[i * 4 + 3] = rgba[i * 4 + 3];
  }
  return Buffer.concat([head, px]);
}

// deterministic hash noise in [0,1)
export function hash2(x, y, seed = 0) {
  let h = (Math.imul(x | 0, 374761393) + Math.imul(y | 0, 668265263) + Math.imul(seed | 0, 2246822519)) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  h ^= h >>> 16;
  return (h >>> 0) / 4294967296;
}

const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const smooth = (e0, e1, x) => { const t = clamp((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); };

// image from a per-pixel function returning [r,g,b,a] in 0..1
export function image(w, h, fn) {
  const out = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const c = fn(x, y);
    for (let k = 0; k < 4; k++) out[(y * w + x) * 4 + k] = Math.round(clamp(c[k] ?? 1, 0, 1) * 255);
  }
  return out;
}

// normal map from a tileable height function height(x, y) in 0..1
export function normalMap(w, h, height, strength) {
  const H = (x, y) => height(((x % w) + w) % w, ((y % h) + h) % h);
  return image(w, h, (x, y) => {
    const dx = (H(x + 1, y) - H(x - 1, y)) * 0.5 * strength;
    const dy = (H(x, y + 1) - H(x, y - 1)) * 0.5 * strength;
    const l = Math.hypot(dx, dy, 1);
    return [(-dx / l) * 0.5 + 0.5, (-dy / l) * 0.5 + 0.5, (1 / l) * 0.5 + 0.5, 1];
  });
}

// running-bond bricks: returns { color(x,y), height(x,y), spec(x,y) }
export function bricks(w, h, { bw = 64, bh = 32, mortar = 4, base = [0.55, 0.27, 0.2], mortarColor = [0.45, 0.43, 0.4] } = {}) {
  const cell = (x, y) => {
    const row = Math.floor(y / bh);
    const ox = (row % 2) * (bw / 2);
    const col = Math.floor(((x + ox) % w) / bw);
    const lx = (x + ox) % bw, ly = y % bh;
    const edge = Math.min(lx, bw - 1 - lx, ly, bh - 1 - ly);
    return { row, col, edge };
  };
  const height = (x, y) => {
    const { row, col, edge } = cell(x, y);
    const bevel = smooth(mortar * 0.5, mortar + 3, edge);
    return bevel * (0.85 + 0.15 * hash2(col, row, 7)) + 0.04 * hash2(x, y, 3);
  };
  const color = (x, y) => {
    const { row, col, edge } = cell(x, y);
    if (edge < mortar * 0.5 + 1) return [...mortarColor.map((c) => c * (0.9 + 0.2 * hash2(x, y, 5))), 1];
    const v = 0.8 + 0.4 * hash2(col, row, 11);
    const n = 0.92 + 0.16 * hash2(x, y, 13);
    return [base[0] * v * n, base[1] * v * n, base[2] * v * n, 1];
  };
  const spec = (x, y) => { const s = cell(x, y).edge < mortar ? 0.05 : 0.25; return [s, s, s, 1]; };
  return { color, height, spec };
}

// square floor tiles with grout
export function tiles(w, h, { size = 64, grout = 3, base = [0.62, 0.62, 0.6] } = {}) {
  const edgeOf = (x, y) => { const lx = x % size, ly = y % size; return Math.min(lx, size - 1 - lx, ly, size - 1 - ly); };
  const id = (x, y) => [Math.floor(x / size), Math.floor(y / size)];
  const height = (x, y) => smooth(grout * 0.5, grout + 4, edgeOf(x, y)) * 0.95 + 0.05 * hash2(x >> 2, y >> 2, 17);
  const color = (x, y) => {
    if (edgeOf(x, y) < grout) return [0.25, 0.25, 0.24, 1];
    const [i, j] = id(x, y);
    const v = 0.85 + 0.25 * hash2(i, j, 19) + 0.05 * hash2(x, y, 23);
    return [base[0] * v, base[1] * v, base[2] * v, 1];
  };
  const spec = (x, y) => { const s = edgeOf(x, y) < grout ? 0.05 : 0.6; return [s, s, s, 1]; };
  return { color, height, spec };
}

// projection image for a spot light: a soft disc crossed by window bars
export function gobo(n = 128) {
  return image(n, n, (x, y) => {
    const u = (x + 0.5) / n - 0.5, v = (y + 0.5) / n - 0.5;
    const r = Math.hypot(u, v) * 2;
    const disc = 1 - smooth(0.8, 0.98, r);
    const bar = (Math.abs(u) < 0.02 || Math.abs(v) < 0.02) ? 0.08 : 1;
    const warm = [1, 0.92, 0.75];
    return [warm[0] * disc * bar, warm[1] * disc * bar, warm[2] * disc * bar, 1];
  });
}
