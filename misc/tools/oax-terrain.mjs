#!/usr/bin/env node
// oax-terrain.mjs: bake `misc_oax_terrain` entities into the OAX_TERRAIN
// BSPX lump (engine side: code/qcommon/oax_terrain.h, which documents the
// layout).
//
//   node misc/tools/oax-terrain.mjs <map.bsp> <game dir>
//
// tests/maps/build.mjs calls bakeTerrain() after the last q3map2 stage. The
// entity names image files relative to the game directory:
//
//   "classname" "misc_oax_terrain"
//   "origin" "x y z"            world position of sample (0,0) at height 0
//   "cellsize" "64"             distance between samples
//   "heightmap" "maps/x_h.pgm"  binary PGM (P5), 8 or 16 bit; size = samples
//   "heightscale" "0.25"        world units per height step
//   "bottom" "-512"             z of the solid underside (default: lowest - 64)
//   "splatmap" "maps/x_s.pam"   PAM (P7) 4 x 8 bit: layer weights (default layer 0)
//   "densitymap" "maps/x_d.pam" PAM (P7) 4 x 8 bit: foliage density channels
//   "layer0".."layer3"          shader/texture names; "layerscale0".."3" world units per repeat
//   "foliageseed" "1234"
//   "foliage0".."foliage3"      "<grass|tree> <shader> <channel> <density/cell> <sizeMin> <sizeMax>
//                                <fadeStart> <fadeEnd> [collideRadius collideHeight [maxSlope]]"
//   "contents" "1"  "surfaceflags" "0"

import fs from 'node:fs';
import path from 'node:path';
import { addLump } from './bspx.mjs';

const NAME = 64;
const MAX_LAYERS = 4;
const MAX_FOLIAGE = 4;
const FOLIAGE_SIZE = 4 + NAME + 4 + 11 * 4;   // 116
const DISK_SIZE = 820;                        // sizeof(oaxTerrainDisk_t)

// Entities of a .map or BSP entity string: [{key: value}], brushes skipped.
export function parseEntities(text) {
  const ents = [];
  let depth = 0, cur = null;
  const re = /"([^"]*)"\s+"([^"]*)"|(\{)|(\})|\/\/[^\n]*/g;
  let m;
  while ((m = re.exec(text))) {
    if (m[3]) { depth++; if (depth === 1) cur = {}; }
    else if (m[4]) { if (depth === 1 && cur) ents.push(cur); depth--; }
    else if (m[1] !== undefined && depth === 1 && cur) cur[m[1]] = m[2];
  }
  return ents;
}

function token(buf, at) {
  // skip whitespace and comments
  for (;;) {
    while (at.i < buf.length && /\s/.test(String.fromCharCode(buf[at.i]))) at.i++;
    if (buf[at.i] === 0x23) { while (at.i < buf.length && buf[at.i] !== 0x0a) at.i++; continue; }
    break;
  }
  const s = at.i;
  while (at.i < buf.length && !/\s/.test(String.fromCharCode(buf[at.i]))) at.i++;
  return buf.toString('latin1', s, at.i);
}

// PGM (P5, maxval <= 65535) -> {w, h, data: Uint16Array}
export function readPGM(buf) {
  const at = { i: 0 };
  if (token(buf, at) !== 'P5') throw new Error('not a binary PGM (P5)');
  const w = Number(token(buf, at)), h = Number(token(buf, at)), max = Number(token(buf, at));
  at.i++;
  const data = new Uint16Array(w * h);
  for (let k = 0; k < w * h; k++) data[k] = max > 255 ? buf.readUInt16BE(at.i + k * 2) : buf[at.i + k];
  return { w, h, max, data };
}

// PAM (P7) with DEPTH 4 MAXVAL 255 -> {w, h, data: Uint8Array w*h*4}
export function readPAM(buf) {
  const head = buf.toString('latin1', 0, Math.min(buf.length, 512));
  const end = head.indexOf('ENDHDR\n');
  if (!head.startsWith('P7') || end < 0) throw new Error('not a PAM (P7)');
  const kv = Object.fromEntries(head.slice(3, end).split('\n').filter(Boolean).map((l) => l.trim().split(/\s+/, 2)));
  const w = Number(kv.WIDTH), h = Number(kv.HEIGHT), depth = Number(kv.DEPTH);
  if (depth !== 4 || Number(kv.MAXVAL) !== 255) throw new Error('PAM must be DEPTH 4 MAXVAL 255');
  return { w, h, data: new Uint8Array(buf.subarray(end + 7, end + 7 + w * h * 4)) };
}

export function writePGM16(w, h, data) {
  const head = Buffer.from(`P5\n${w} ${h}\n65535\n`, 'latin1');
  const body = Buffer.alloc(w * h * 2);
  for (let k = 0; k < w * h; k++) body.writeUInt16BE(data[k], k * 2);
  return Buffer.concat([head, body]);
}

export function writePAM(w, h, data) {
  const head = Buffer.from(`P7\nWIDTH ${w}\nHEIGHT ${h}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n`, 'latin1');
  return Buffer.concat([head, Buffer.from(data)]);
}

function writeName(buf, at, s) {
  if (s.length >= NAME) throw new Error(`name too long: ${s}`);
  buf.write(s, at, 'latin1');
}

// one terrain record (oaxTerrainDisk_t + arrays)
export function terrainRecord(ent, gameDir) {
  const num = (k, d) => (ent[k] !== undefined ? Number(ent[k]) : d);
  const origin = (ent.origin || '0 0 0').split(/\s+/).map(Number);
  if (!ent.heightmap) throw new Error('misc_oax_terrain without a heightmap');
  const hm = readPGM(fs.readFileSync(path.join(gameDir, ent.heightmap)));
  const sx = hm.w, sy = hm.h, cells = sx * sy;
  const heightScale = num('heightscale', 1);
  let lo = Infinity;
  for (const v of hm.data) lo = Math.min(lo, v);
  const bottom = num('bottom', origin[2] + lo * heightScale - 64);

  let splat = new Uint8Array(cells * 4);
  if (ent.splatmap) {
    const p = readPAM(fs.readFileSync(path.join(gameDir, ent.splatmap)));
    if (p.w !== sx || p.h !== sy) throw new Error('splatmap size differs from the heightmap');
    splat = p.data;
  } else for (let k = 0; k < cells; k++) splat[k * 4] = 255;
  let density = new Uint8Array(cells * 4);
  if (ent.densitymap) {
    const p = readPAM(fs.readFileSync(path.join(gameDir, ent.densitymap)));
    if (p.w !== sx || p.h !== sy) throw new Error('densitymap size differs from the heightmap');
    density = p.data;
  }

  const align = (n) => (n + 3) & ~3;
  const heightsOfs = DISK_SIZE, splatOfs = align(heightsOfs + cells * 2), densityOfs = splatOfs + cells * 4;
  const size = densityOfs + cells * 4;
  const b = Buffer.alloc(align(size));
  b.writeInt32LE(size, 0);
  b.writeFloatLE(origin[0], 4); b.writeFloatLE(origin[1], 8); b.writeFloatLE(origin[2], 12);
  b.writeFloatLE(num('cellsize', 64), 16);
  b.writeInt32LE(sx, 20); b.writeInt32LE(sy, 24);
  b.writeFloatLE(heightScale, 28);
  b.writeFloatLE(bottom, 32);
  b.writeInt32LE(num('contents', 1), 36);
  b.writeInt32LE(num('surfaceflags', 0), 40);
  let layers = 0;
  for (let l = 0; l < MAX_LAYERS; l++) {
    if (!ent[`layer${l}`]) continue;
    if (l !== layers) throw new Error('terrain layers must be numbered from 0 without gaps');
    writeName(b, 48 + l * NAME, ent[`layer${l}`]);
    b.writeFloatLE(num(`layerscale${l}`, 256), 304 + l * 4);
    layers++;
  }
  b.writeInt32LE(layers, 44);
  b.writeInt32LE(num('foliageseed', 1) | 0, 320);
  let nf = 0;
  for (let f = 0; f < MAX_FOLIAGE; f++) {
    if (!ent[`foliage${f}`]) continue;
    const p = ent[`foliage${f}`].trim().split(/\s+/);
    const at = 328 + nf * FOLIAGE_SIZE;
    const kind = { grass: 0, tree: 1 }[p[0]];
    if (kind === undefined) throw new Error(`foliage kind ${p[0]}`);
    b.writeInt32LE(kind, at);
    writeName(b, at + 4, p[1]);
    const v = p.slice(2).map(Number);
    const vals = [v[1] ?? 1, v[2] ?? 1, v[3] ?? 1, v[4] ?? 1000, v[5] ?? 2000, v[6] ?? 0, v[7] ?? 0, v[8] ?? 0.7];
    b.writeInt32LE(v[0] | 0, at + 68);
    vals.forEach((x, k) => b.writeFloatLE(x, at + 72 + k * 4));
    nf++;
  }
  b.writeInt32LE(nf, 324);
  b.writeInt32LE(heightsOfs, 792); b.writeInt32LE(splatOfs, 796); b.writeInt32LE(densityOfs, 800);
  for (let k = 0; k < cells; k++) b.writeUInt16LE(hm.data[k], heightsOfs + k * 2);
  Buffer.from(splat).copy(b, splatOfs);
  Buffer.from(density).copy(b, densityOfs);
  return b;
}

export function terrainLump(ents, gameDir) {
  const recs = ents.filter((e) => e.classname === 'misc_oax_terrain').map((e) => terrainRecord(e, gameDir));
  if (!recs.length) return null;
  const head = Buffer.alloc(16);
  head.write('OTRN', 0, 'latin1');
  head.writeInt32LE(1, 4);
  head.writeInt32LE(recs.length, 8);
  return Buffer.concat([head, ...recs]);
}

// Adds OAX_TERRAIN to a compiled BSP from its own entity string; returns the
// number of terrains baked.
export function bakeTerrain(bspFile, mapText, gameDir) {
  const lump = terrainLump(parseEntities(mapText), gameDir);
  if (!lump) return 0;
  addLump(bspFile, 'OAX_TERRAIN', lump);
  return lump.readInt32LE(8);
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const [bsp, game] = process.argv.slice(2);
  if (!bsp || !game) {
    console.error('usage: oax-terrain.mjs <map.bsp> <game dir>');
    process.exit(2);
  }
  const buf = fs.readFileSync(bsp);
  const ofs = buf.readInt32LE(8), len = buf.readInt32LE(12);   // LUMP_ENTITIES
  console.log(`${bakeTerrain(bsp, buf.toString('latin1', ofs, ofs + len), game)} terrain(s) baked`);
}
