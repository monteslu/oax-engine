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
