// zip.mjs: read and write the pk3 (zip) files OpenArena ships, with no
// dependencies (node's zlib). Reading handles stored and deflated entries;
// writing deflates, with a stable entry order and fixed timestamps so that
// the same inputs build the same bytes.
import fs from 'node:fs';
import zlib from 'node:zlib';

const EOCD = 0x06054b50, CEN = 0x02014b50, LOC = 0x04034b50;

export class ZipReader {
  constructor(file) {
    this.file = file;
    this.buf = fs.readFileSync(file);
    this.entries = new Map();
    this.#readDirectory();
  }

  #readDirectory() {
    const b = this.buf;
    let e = -1;
    for (let i = b.length - 22; i >= Math.max(0, b.length - 65557); i--) {
      if (b.readUInt32LE(i) === EOCD) { e = i; break; }
    }
    if (e < 0) throw new Error(`${this.file}: not a zip (no end of central directory)`);
    const count = b.readUInt16LE(e + 10);
    let p = b.readUInt32LE(e + 16);
    for (let i = 0; i < count; i++) {
      if (b.readUInt32LE(p) !== CEN) throw new Error(`${this.file}: bad central directory entry ${i}`);
      const method = b.readUInt16LE(p + 10);
      const crc = b.readUInt32LE(p + 16);
      const csize = b.readUInt32LE(p + 20);
      const size = b.readUInt32LE(p + 24);
      const nlen = b.readUInt16LE(p + 28), xlen = b.readUInt16LE(p + 30), clen = b.readUInt16LE(p + 32);
      const lofs = b.readUInt32LE(p + 42);
      const name = b.toString('latin1', p + 46, p + 46 + nlen);
      if (!name.endsWith('/')) this.entries.set(name.toLowerCase(), { name, method, crc, csize, size, lofs });
      p += 46 + nlen + xlen + clen;
    }
  }

  names() { return [...this.entries.values()].map((e) => e.name); }
  has(name) { return this.entries.has(name.toLowerCase()); }
  stat(name) { return this.entries.get(name.toLowerCase()) || null; }

  read(name) {
    const e = this.entries.get(name.toLowerCase());
    if (!e) throw new Error(`${this.file}: no entry ${name}`);
    const b = this.buf;
    if (b.readUInt32LE(e.lofs) !== LOC) throw new Error(`${this.file}: bad local header for ${name}`);
    const start = e.lofs + 30 + b.readUInt16LE(e.lofs + 26) + b.readUInt16LE(e.lofs + 28);
    const raw = b.subarray(start, start + e.csize);
    if (e.method === 0) return Buffer.from(raw);
    if (e.method === 8) return zlib.inflateRawSync(raw);
    throw new Error(`${this.file}: ${name}: compression method ${e.method} unsupported`);
  }
}

let crcTable = null;
export function crc32(buf) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crcTable[n] = c >>> 0; }
  }
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// files: [{ name, data: Buffer }]; written sorted by name, DOS time 1980-01-01
export function writeZip(file, files, { level = 9 } = {}) {
  const sorted = [...files].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  const parts = [], central = [];
  let ofs = 0;
  for (const f of sorted) {
    const name = Buffer.from(f.name, 'latin1');
    const crc = crc32(f.data);
    const deflated = zlib.deflateRawSync(f.data, { level });
    const useDeflate = deflated.length < f.data.length;
    const body = useDeflate ? deflated : f.data;
    const method = useDeflate ? 8 : 0;
    const loc = Buffer.alloc(30);
    loc.writeUInt32LE(LOC, 0); loc.writeUInt16LE(20, 4); loc.writeUInt16LE(0, 6); loc.writeUInt16LE(method, 8);
    loc.writeUInt16LE(0, 10); loc.writeUInt16LE(0x21, 12);          // time 0, date 1980-01-01
    loc.writeUInt32LE(crc, 14); loc.writeUInt32LE(body.length, 18); loc.writeUInt32LE(f.data.length, 22);
    loc.writeUInt16LE(name.length, 26); loc.writeUInt16LE(0, 28);
    parts.push(loc, name, body);
    const cen = Buffer.alloc(46);
    cen.writeUInt32LE(CEN, 0); cen.writeUInt16LE(20, 4); cen.writeUInt16LE(20, 6); cen.writeUInt16LE(0, 8); cen.writeUInt16LE(method, 10);
    cen.writeUInt16LE(0, 12); cen.writeUInt16LE(0x21, 14);
    cen.writeUInt32LE(crc, 16); cen.writeUInt32LE(body.length, 20); cen.writeUInt32LE(f.data.length, 24);
    cen.writeUInt16LE(name.length, 28); cen.writeUInt32LE(ofs, 42);
    central.push(cen, name);
    ofs += 30 + name.length + body.length;
  }
  const cdSize = central.reduce((a, c) => a + c.length, 0);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(EOCD, 0); end.writeUInt16LE(sorted.length, 8); end.writeUInt16LE(sorted.length, 10);
  end.writeUInt32LE(cdSize, 12); end.writeUInt32LE(ofs, 16);
  fs.writeFileSync(file, Buffer.concat([...parts, ...central, end]));
}
