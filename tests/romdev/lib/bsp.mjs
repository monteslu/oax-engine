// bsp.mjs: read the entity lump of a Q3 BSP, so tests can place cameras and
// players at a map's own spawn points instead of hand-picked coordinates.

import fs from 'node:fs';

export function readEntities(bspPath) {
  const buf = fs.readFileSync(bspPath);
  if (buf.toString('latin1', 0, 4) !== 'IBSP') throw new Error(`${bspPath}: not an IBSP file`);
  const ofs = buf.readInt32LE(8);
  const len = buf.readInt32LE(12);
  const text = buf.toString('latin1', ofs, ofs + len);
  const ents = [];
  const re = /\{([^}]*)\}/g;
  let m;
  while ((m = re.exec(text))) {
    const e = {};
    const kv = /"([^"]*)"\s+"([^"]*)"/g;
    let p;
    while ((p = kv.exec(m[1]))) e[p[1]] = p[2];
    ents.push(e);
  }
  return ents;
}

export function spawnPoints(bspPath) {
  return readEntities(bspPath)
    .filter((e) => e.classname === 'info_player_deathmatch' || e.classname === 'info_player_start')
    .map((e) => {
      const [x, y, z] = (e.origin || '0 0 0').split(/\s+/).map(Number);
      return { x, y, z, yaw: Number(e.angle || 0) };
    });
}

// Read a BSP from a path, or from a member of a .pk3 (zip) when given
// { pk3, member }.
import { execFileSync } from 'node:child_process';

export function bspBuffer(src) {
  if (typeof src === 'string') return fs.readFileSync(src);
  return execFileSync('unzip', ['-p', src.pk3, src.member], { maxBuffer: 512 << 20 });
}

export function entitiesFromBuffer(buf) {
  if (buf.toString('latin1', 0, 4) !== 'IBSP') throw new Error('not an IBSP file');
  const ofs = buf.readInt32LE(8), len = buf.readInt32LE(12);
  const text = buf.toString('latin1', ofs, ofs + len);
  const ents = [];
  const re = /\{([^}]*)\}/g;
  let m;
  while ((m = re.exec(text))) {
    const e = {};
    const kv = /"([^"]*)"\s+"([^"]*)"/g;
    let p;
    while ((p = kv.exec(m[1]))) e[p[1]] = p[2];
    ents.push(e);
  }
  return ents;
}

// World model (model 0) bounds: LUMP_MODELS is lump 7, each dmodel_t is
// mins[3], maxs[3], then 4 ints (40 bytes).
export function worldBounds(buf) {
  const ofs = buf.readInt32LE(8 + 7 * 8);
  const f = (i) => buf.readFloatLE(ofs + i * 4);
  return { mins: [f(0), f(1), f(2)], maxs: [f(3), f(4), f(5)] };
}

// Lowest point of any model: the world plus brush entities such as a
// trigger_hurt pit under a floating map.
export function lowestModelZ(buf) {
  const ofs = buf.readInt32LE(8 + 7 * 8), len = buf.readInt32LE(12 + 7 * 8);
  let z = Infinity;
  for (let at = ofs; at + 40 <= ofs + len; at += 40) z = Math.min(z, buf.readFloatLE(at + 8));
  return z;
}
