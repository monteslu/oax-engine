// png.mjs: just enough PNG to compare screenshots. Decodes 8-bit RGB/RGBA,
// non-interlaced (what romdev writes); encodes RGBA for diff images.

import fs from 'node:fs';
import zlib from 'node:zlib';

const SIG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

export function decodePng(buf) {
  if (!buf.subarray(0, 8).equals(SIG)) throw new Error('not a PNG');
  let pos = 8, width = 0, height = 0, colorType = 0, bitDepth = 0, interlace = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('latin1', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === 'IDAT') {
      idat.push(data);
    } else if (type === 'IEND') {
      break;
    }
    pos += 12 + len;
  }
  if (bitDepth !== 8 || interlace !== 0 || (colorType !== 2 && colorType !== 6)) {
    throw new Error(`unsupported PNG (depth ${bitDepth}, color ${colorType}, interlace ${interlace})`);
  }
  const bpp = colorType === 6 ? 4 : 3;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * bpp;
  const out = Buffer.alloc(width * height * 4);
  let prev = Buffer.alloc(stride);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = Buffer.from(raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)));
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? line[i - bpp] : 0;
      const b = prev[i];
      const c = i >= bpp ? prev[i - bpp] : 0;
      let v = line[i];
      if (filter === 1) v += a;
      else if (filter === 2) v += b;
      else if (filter === 3) v += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      line[i] = v & 0xff;
    }
    for (let x = 0; x < width; x++) {
      out[(y * width + x) * 4] = line[x * bpp];
      out[(y * width + x) * 4 + 1] = line[x * bpp + 1];
      out[(y * width + x) * 4 + 2] = line[x * bpp + 2];
      out[(y * width + x) * 4 + 3] = bpp === 4 ? line[x * bpp + 3] : 255;
    }
    prev = line;
  }
  return { width, height, data: out };
}

export function readPng(file) {
  return decodePng(fs.readFileSync(file));
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'latin1'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([len, body, crc]);
}

export function encodePng({ width, height, data }) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  const raw = Buffer.alloc((width * 4 + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (width * 4 + 1)] = 0;
    data.copy(raw, y * (width * 4 + 1) + 1, y * width * 4, (y + 1) * width * 4);
  }
  return Buffer.concat([SIG, chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

// Perceptual-ish comparison: per pixel, the largest channel difference.
// `tolerance` forgives small shading noise; the result reports how many
// pixels exceed it and the mean difference, and can write a diff image.
export function comparePng(a, b, { tolerance = 16, diffPath } = {}) {
  if (a.width !== b.width || a.height !== b.height) {
    return { sameSize: false, sizeA: [a.width, a.height], sizeB: [b.width, b.height], badFraction: 1 };
  }
  const n = a.width * a.height;
  let bad = 0, sum = 0, max = 0;
  const diff = diffPath ? Buffer.alloc(n * 4) : null;
  for (let i = 0; i < n; i++) {
    const d = Math.max(
      Math.abs(a.data[i * 4] - b.data[i * 4]),
      Math.abs(a.data[i * 4 + 1] - b.data[i * 4 + 1]),
      Math.abs(a.data[i * 4 + 2] - b.data[i * 4 + 2]));
    sum += d;
    if (d > max) max = d;
    if (d > tolerance) bad++;
    if (diff) {
      const g = a.data[i * 4 + 1] >> 2;
      diff[i * 4] = d > tolerance ? 255 : g;
      diff[i * 4 + 1] = d > tolerance ? 0 : g;
      diff[i * 4 + 2] = d > tolerance ? 0 : g;
      diff[i * 4 + 3] = 255;
    }
  }
  if (diff) fs.writeFileSync(diffPath, encodePng({ width: a.width, height: a.height, data: diff }));
  return { sameSize: true, badPixels: bad, badFraction: bad / n, meanDiff: sum / n, maxDiff: max };
}

// Distinct colours in a frame: a black, white or single-colour frame is not
// a picture of a map, whatever any comparison says.
export function distinctColors(img, cap = 4096) {
  const seen = new Set();
  for (let i = 0; i < img.width * img.height && seen.size < cap; i += 7) {
    seen.add((img.data[i * 4] << 16) | (img.data[i * 4 + 1] << 8) | img.data[i * 4 + 2]);
  }
  return seen.size;
}

// 2x2 box-filter downsample: goldens are stored at half size (a quarter of
// the bytes in git), and the comparison forgives single-pixel shimmer.
export function halfSize(img) {
  const w = img.width >> 1, h = img.height >> 1;
  const data = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      for (let c = 0; c < 4; c++) {
        const at = (yy, xx) => img.data[((y * 2 + yy) * img.width + (x * 2 + xx)) * 4 + c];
        data[(y * w + x) * 4 + c] = (at(0, 0) + at(0, 1) + at(1, 0) + at(1, 1) + 2) >> 2;
      }
    }
  }
  return { width: w, height: h, data };
}

export function writePng(file, img) {
  fs.writeFileSync(file, encodePng(img));
}
