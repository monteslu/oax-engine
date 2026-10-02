// tga.mjs: read the TGA screenshots the native client writes (uncompressed
// type 2 or RLE type 10, 24/32-bit), as {width, height, data RGBA}.

import fs from 'node:fs';

export function readTga(file) {
  const b = fs.readFileSync(file);
  const idLen = b[0], type = b[2], w = b.readUInt16LE(12), h = b.readUInt16LE(14), bpp = b[16] >> 3, desc = b[17];
  let p = 18 + idLen;
  const px = Buffer.alloc(w * h * 4);
  let i = 0;
  const put = (o) => {
    px[i * 4] = b[o + 2]; px[i * 4 + 1] = b[o + 1]; px[i * 4 + 2] = b[o]; px[i * 4 + 3] = 255; i++;
  };
  if (type === 2) {
    for (; i < w * h; p += bpp) put(p);
  } else if (type === 10) {
    while (i < w * h) {
      const c = b[p++];
      const n = (c & 0x7f) + 1;
      if (c & 0x80) { for (let k = 0; k < n; k++) put(p); p += bpp; }
      else { for (let k = 0; k < n; k++, p += bpp) put(p); }
    }
  } else {
    throw new Error(`${file}: TGA type ${type} not supported`);
  }
  // bottom-up unless the descriptor says top-down
  if (!(desc & 0x20)) {
    const row = w * 4, tmp = Buffer.alloc(row);
    for (let y = 0; y < h >> 1; y++) {
      const a = y * row, z = (h - 1 - y) * row;
      px.copy(tmp, 0, a, a + row); px.copy(px, a, z, z + row); tmp.copy(px, z);
    }
  }
  return { width: w, height: h, data: px };
}

// A 32-bit uncompressed TGA, stored bottom-up (Quake III ignores the
// top-down bit), from {width, height, data RGBA} in top-down rows. The
// engine's loader returns exactly `data`.
export function encodeTga({ width, height, data }) {
  const out = Buffer.alloc(18 + width * height * 4);
  out[2] = 2;
  out.writeUInt16LE(width, 12);
  out.writeUInt16LE(height, 14);
  out[16] = 32;
  out[17] = 0x08;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = (y * width + x) * 4, o = 18 + ((height - 1 - y) * width + x) * 4;
      out[o] = data[i + 2];
      out[o + 1] = data[i + 1];
      out[o + 2] = data[i];
      out[o + 3] = data[i + 3];
    }
  }
  return out;
}
