// content.mjs: which materials exist, for the strict map build. A material
// (a BSP shader name or an OAX_SURFACES material) exists when a shader
// script defines it or an image of that name exists (.tga, .jpg, .png),
// exactly as the engine resolves it at load. Sources: the OpenArena pk3s
// (indexed once, cached by their names, sizes and mtimes) and the build's
// own output tree (the maps' generated shaders and textures).

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const IMAGE = /\.(tga|jpg|jpeg|png)$/i;

// shader names a .shader text defines (top-level names before a '{')
export function shaderNames(text) {
  const names = [];
  const clean = text.replace(/\/\/[^\n]*/g, '').replace(/\/\*[\s\S]*?\*\//g, '');
  let depth = 0, last = null;
  for (const tok of clean.match(/\{|\}|[^\s{}]+/g) || []) {
    if (tok === '{') { if (depth === 0 && last) names.push(last.toLowerCase()); depth++; last = null; }
    else if (tok === '}') depth = Math.max(0, depth - 1);
    else if (depth === 0) last = tok;
  }
  return names;
}

function indexPk3s(baseoa, cacheFile) {
  const pk3s = fs.readdirSync(baseoa).filter((f) => f.toLowerCase().endsWith('.pk3')).sort();
  const key = pk3s.map((f) => { const st = fs.statSync(path.join(baseoa, f)); return `${f}:${st.size}:${st.mtimeMs}`; }).join('|');
  if (cacheFile && fs.existsSync(cacheFile)) {
    try {
      const c = JSON.parse(fs.readFileSync(cacheFile, 'utf8'));
      if (c.key === key) return { shaders: new Set(c.shaders), images: new Set(c.images) };
    } catch { /* rebuild */ }
  }
  const shaders = new Set(), images = new Set();
  for (const f of pk3s) {
    const p = path.join(baseoa, f);
    const list = execFileSync('unzip', ['-Z1', p], { encoding: 'utf8', maxBuffer: 64 << 20 }).split('\n').filter(Boolean);
    for (const e of list) if (IMAGE.test(e)) images.add(e.replace(IMAGE, '').toLowerCase());
    if (list.some((e) => /^scripts\/[^/]+\.shader$/i.test(e))) {
      const text = execFileSync('unzip', ['-p', p, 'scripts/*.shader'], { encoding: 'latin1', maxBuffer: 256 << 20 });
      for (const n of shaderNames(text)) shaders.add(n);
    }
  }
  if (cacheFile) {
    fs.mkdirSync(path.dirname(cacheFile), { recursive: true });
    fs.writeFileSync(cacheFile, JSON.stringify({ key, shaders: [...shaders], images: [...images] }));
  }
  return { shaders, images };
}

function indexTree(dir, shaders, images) {
  if (!fs.existsSync(dir)) return;
  const walk = (d, rel) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const r = rel ? `${rel}/${e.name}` : e.name;
      if (e.isDirectory()) walk(path.join(d, e.name), r);
      else if (IMAGE.test(e.name)) images.add(r.replace(IMAGE, '').toLowerCase());
      else if (/^scripts\/[^/]+\.shader$/i.test(r)) for (const n of shaderNames(fs.readFileSync(path.join(d, e.name), 'latin1'))) shaders.add(n);
    }
  };
  walk(dir, '');
}

// { has(name) -> bool }
export function contentIndex(baseoa, gameOut, cacheFile) {
  const { shaders, images } = indexPk3s(baseoa, cacheFile);
  const s = new Set(shaders), i = new Set(images);
  indexTree(gameOut, s, i);
  return {
    has(name) {
      const n = name.toLowerCase().replace(IMAGE, '');
      return s.has(n) || i.has(n);
    },
  };
}
