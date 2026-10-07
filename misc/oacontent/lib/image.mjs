// image.mjs: header-level image facts (width, height, alpha) for TGA, PNG and
// JPEG without decoding, and ImageMagick-backed decode to raw RGBA when pixels
// are needed (mean colour, normal and specular generation, upscales).
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export function imageInfo(buf, name = '') {
  if (buf.length > 18 && /\.tga$/i.test(name)) {
    const type = buf[2], w = buf.readUInt16LE(12), h = buf.readUInt16LE(14), bpp = buf[16];
    return { format: 'tga', width: w, height: h, alpha: bpp === 32, type };
  }
  if (buf.length > 24 && buf.readUInt32BE(0) === 0x89504e47) {
    return { format: 'png', width: buf.readUInt32BE(16), height: buf.readUInt32BE(20), alpha: buf[25] === 6 || buf[25] === 4 };
  }
  if (buf[0] === 0xff && buf[1] === 0xd8) {
    let p = 2;
    while (p + 9 < buf.length) {
      if (buf[p] !== 0xff) { p++; continue; }
      const m = buf[p + 1];
      if (m >= 0xc0 && m <= 0xcf && m !== 0xc4 && m !== 0xc8 && m !== 0xcc) return { format: 'jpg', width: buf.readUInt16BE(p + 7), height: buf.readUInt16BE(p + 5), alpha: false };
      p += 2 + buf.readUInt16BE(p + 2);
    }
  }
  return { format: 'unknown', width: 0, height: 0, alpha: false };
}

// decode any image to { width, height, data: Buffer RGBA } via ImageMagick
export function decodeRgba(buf, name = 'img') {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oacontent-'));
  try {
    const ext = (/\.(\w+)$/.exec(name) || [, 'bin'])[1];
    const src = path.join(dir, `in.${ext}`), dst = path.join(dir, 'out.rgba');
    fs.writeFileSync(src, buf);
    const dims = execFileSync('magick', ['identify', '-format', '%w %h', `${src}[0]`], { encoding: 'utf8' }).trim().split(' ').map(Number);
    execFileSync('magick', [`${src}[0]`, '-depth', '8', `rgba:${dst}`]);
    return { width: dims[0], height: dims[1], data: fs.readFileSync(dst) };
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
}

export function encodePng(img) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oacontent-'));
  try {
    const src = path.join(dir, 'in.rgba'), dst = path.join(dir, 'out.png');
    fs.writeFileSync(src, img.data);
    execFileSync('magick', ['-size', `${img.width}x${img.height}`, '-depth', '8', `rgba:${src}`, dst]);
    return fs.readFileSync(dst);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
}

export function meanColor(img) {
  const s = [0, 0, 0]; const n = img.width * img.height;
  for (let i = 0; i < n; i++) { s[0] += img.data[i * 4]; s[1] += img.data[i * 4 + 1]; s[2] += img.data[i * 4 + 2]; }
  return s.map((v) => +(v / n / 255).toFixed(4));
}

// A PNG encoder with no dependencies: 8 bit RGB or RGBA, scanlines filtered
// with Sub, deflated. data is Buffer/Uint8Array of width*height*4 RGBA bytes
// (alpha dropped when alpha is false).
import zlib from 'node:zlib';
import { crc32 } from './zip.mjs';

export function pngEncode(width, height, rgba, { alpha = true } = {}) {
  const bpp = alpha ? 4 : 3;
  const raw = Buffer.alloc((width * bpp + 1) * height);
  for (let y = 0; y < height; y++) {
    const ro = y * (width * bpp + 1);
    raw[ro] = 1;                                         // filter: Sub
    for (let x = 0; x < width; x++) {
      for (let c = 0; c < bpp; c++) {
        const v = rgba[(y * width + x) * 4 + c];
        const left = x > 0 ? rgba[(y * width + x - 1) * 4 + c] : 0;
        raw[ro + 1 + x * bpp + c] = (v - left) & 255;
      }
    }
  }
  const chunk = (type, body) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(body.length);
    const td = Buffer.concat([Buffer.from(type, 'latin1'), body]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0); ihdr.writeUInt32BE(height, 4); ihdr[8] = 8; ihdr[9] = alpha ? 6 : 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
