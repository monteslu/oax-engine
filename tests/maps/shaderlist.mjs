// shaderlist.mjs: q3map2 only reads the shader scripts named in
// scripts/shaderlist.txt (in every search path), so a test map with its
// own shaders must list them. Maps build one at a time into the same
// tests/maps/out/baseoa, so each returns the union of what is already
// listed (and still exists) and its own scripts.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const scripts = path.join(path.dirname(fileURLToPath(import.meta.url)), 'out', 'baseoa', 'scripts');

export function shaderList(...names) {
  const file = path.join(scripts, 'shaderlist.txt');
  const have = fs.existsSync(file) ? fs.readFileSync(file, 'utf8').split(/\s+/).filter(Boolean) : [];
  const keep = have.filter((n) => fs.existsSync(path.join(scripts, `${n}.shader`)));
  for (const n of names) if (!keep.includes(n)) keep.push(n);
  return keep.join('\n') + '\n';
}
