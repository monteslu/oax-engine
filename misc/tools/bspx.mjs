#!/usr/bin/env node
// bspx.mjs: add, list or strip BSPX extension lumps in a Q3 BSP (v46).
//
//   node misc/tools/bspx.mjs list  <map.bsp>
//   node misc/tools/bspx.mjs add   <map.bsp> <NAME> <file>   (replaces NAME)
//   node misc/tools/bspx.mjs strip <map.bsp>
//
// Layout (engine side: code/qcommon/bspx.c): after the last standard lump,
// aligned to 4 bytes, "BSPX", int count, then count x { char name[24];
// int ofs; int len; }, then the lump data. q3map2 -light rewrites the file
// and drops the block, so add lumps after the last compile stage.

import fs from 'node:fs';

const HEADER_LUMPS = 17;

function standardEnd(buf) {
  if (buf.toString('latin1', 0, 4) !== 'IBSP') throw new Error('not a Q3 BSP');
  let end = 8 + HEADER_LUMPS * 8;
  for (let i = 0; i < HEADER_LUMPS; i++) {
    const ofs = buf.readInt32LE(8 + i * 8), len = buf.readInt32LE(12 + i * 8);
    end = Math.max(end, ofs + len);
  }
  return (end + 3) & ~3;
}

export function readBspx(buf) {
  const at = standardEnd(buf);
  if (at + 8 > buf.length || buf.toString('latin1', at, at + 4) !== 'BSPX') return { at, lumps: [] };
  const count = buf.readInt32LE(at + 4);
  const lumps = [];
  for (let i = 0; i < count; i++) {
    const e = at + 8 + i * 32;
    const name = buf.toString('latin1', e, e + 24).replace(/\0.*$/s, '');
    const ofs = buf.readInt32LE(e + 24), len = buf.readInt32LE(e + 28);
    lumps.push({ name, data: buf.subarray(ofs, ofs + len) });
  }
  return { at, lumps };
}

export function writeBspx(buf, lumps) {
  const at = standardEnd(buf);
  const base = Buffer.alloc(at);
  buf.copy(base, 0, 0, Math.min(at, buf.length));
  if (!lumps.length) return base;
  const dirLen = 8 + lumps.length * 32;
  const dir = Buffer.alloc(dirLen);
  dir.write('BSPX', 0, 'latin1');
  dir.writeInt32LE(lumps.length, 4);
  const datas = [];
  let ofs = at + dirLen;
  lumps.forEach((l, i) => {
    if (l.name.length > 23) throw new Error(`lump name too long: ${l.name}`);
    const e = 8 + i * 32;
    dir.write(l.name, e, 'latin1');
    dir.writeInt32LE(ofs, e + 24);
    dir.writeInt32LE(l.data.length, e + 28);
    const padded = Buffer.alloc((l.data.length + 3) & ~3);
    l.data.copy(padded);
    datas.push(padded);
    ofs += padded.length;
  });
  return Buffer.concat([base, dir, ...datas]);
}

export function addLump(file, name, data) {
  const buf = fs.readFileSync(file);
  const { lumps } = readBspx(buf);
  const rest = lumps.filter((l) => l.name !== name).map((l) => ({ name: l.name, data: Buffer.from(l.data) }));
  fs.writeFileSync(file, writeBspx(buf, [...rest, { name, data: Buffer.from(data) }]));
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const [cmd, file, name, src] = process.argv.slice(2);
  if (!cmd || !file) {
    console.error('usage: bspx.mjs list|add|strip <map.bsp> [NAME file]');
    process.exit(2);
  }
  const buf = fs.readFileSync(file);
  if (cmd === 'list') {
    for (const l of readBspx(buf).lumps) console.log(`${l.name} ${l.data.length}`);
  } else if (cmd === 'add') {
    addLump(file, name, fs.readFileSync(src));
  } else if (cmd === 'strip') {
    fs.writeFileSync(file, writeBspx(buf, []));
  } else {
    console.error(`unknown command ${cmd}`);
    process.exit(2);
  }
}
