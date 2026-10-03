// ulight.mjs: shared helpers for the unified-lighting tests: the same
// camera list shot on the cart (romdev) and on the native client, plus a
// small image toolkit (pixel access, region differences).

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { Session } from './romdev.mjs';
import { loadScene, CLEAN_VIEW } from './scenes.mjs';
import { readValues, parseDebugValues } from './values.mjs';
import { readPng, writePng } from './png.mjs';
import { nativeHome, nativeBinary, findBaseoa, execNative } from './native.mjs';
import { nativeCaptureArgs } from './capture.mjs';

// Uncompressed (type 2) or RLE (type 10) 24/32-bit TGA, as the engine writes.
export function readTga(file) {
  const b = fs.readFileSync(file);
  const idLen = b[0], type = b[2], w = b.readUInt16LE(12), h = b.readUInt16LE(14), bpp = b[16] >> 3, desc = b[17];
  let p = 18 + idLen;
  const px = Buffer.alloc(w * h * 4);
  let i = 0;
  const put = (o) => { px[i * 4] = b[o + 2]; px[i * 4 + 1] = b[o + 1]; px[i * 4 + 2] = b[o]; px[i * 4 + 3] = 255; i++; };
  if (type === 2) {
    for (; i < w * h; p += bpp) put(p);
  } else if (type === 10) {
    while (i < w * h) {
      const c = b[p++];
      const n = (c & 0x7f) + 1;
      if (c & 0x80) { for (let k = 0; k < n; k++) put(p); p += bpp; } else { for (let k = 0; k < n; k++, p += bpp) put(p); }
    }
  } else {
    throw new Error(`${file}: TGA type ${type} not supported`);
  }
  if (!(desc & 0x20)) {
    const row = w * 4, tmp = Buffer.alloc(row);
    for (let y = 0; y < h >> 1; y++) {
      const a = y * row, z = (h - 1 - y) * row;
      px.copy(tmp, 0, a, a + row); px.copy(px, a, z, z + row); tmp.copy(px, z);
    }
  }
  return { width: w, height: h, data: px };
}

// Shoot `views` ("x y z pitch yaw roll") on the cart. `player` places the
// player (setviewpos x y z yaw): the areamask follows the player, not the
// camera. Returns { files, values }.
export async function shootCart(map, views, { name, out, pre = '', preload = '', player, settle = 20, seed = 1 } = {}) {
  const s = new Session(name);
  const files = [];
  try {
    // preload: cvars that must be set before the map loads (latched ones)
    await loadScene(s, map, { seed, view: preload ? `${CLEAN_VIEW};${preload}` : CLEAN_VIEW });
    const cmds = [pre, player ? `setviewpos ${player}` : ''].filter(Boolean).join(';');
    if (cmds) {
      await s.command(cmds);
      await s.step(60);
    }
    for (let i = 0; i < views.length; i++) {
      await s.command(`cl_overrideView "${views[i]}"`);
      await s.step(settle);
      const file = path.join(out, `${name}_${i}.png`);
      await s.screenshot(file);
      files.push(file);
    }
    return { files, values: await readValues(s) };
  } finally {
    await s.shutdown();
  }
}

// The same on the native client (fixedtime 16; a console `wait N` lasts
// about N/2 frames there).
export function shootNative(map, views, { name, out, pre = '', preload = '', player } = {}) {
  const home = nativeHome(name);
  const game = path.join(home, 'baseoa');
  const lines = ['fixedtime 16', ...CLEAN_VIEW.split(';'), ...pre.split(';').filter(Boolean)];
  if (player) lines.push(`setviewpos ${player}`);
  lines.push('wait 120');
  views.forEach((v, i) => lines.push(`cl_overrideView "${v}"`, 'wait 40', `screenshot shot_${i}`, 'wait 2'));
  lines.push('debugvalues values.txt', 'quit');
  fs.writeFileSync(path.join(game, 'ulight_shots.cfg'), lines.join('\n') + '\n');
  try {
    execNative([
      '+set', 'fs_basepath', path.dirname(findBaseoa()), '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
      ...nativeCaptureArgs(),
      '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0',
      '+set', 'bot_enable', '0', '+set', 'com_introplayed', '1', '+set', 'fixedtime', '16', '+set', 'com_maxfps', '0',
      ...preload.split(';').filter(Boolean).flatMap((c) => { const [k, ...v] = c.trim().split(/\s+/); return ['+set', k, v.join(' ')]; }),
      '+devmap', map, '+wait', '200', '+exec', 'ulight_shots.cfg',
    ], { home, timeout: 300000 });
  } catch (e) {
    if (e.code === 'ETIMEDOUT' || e.signal) throw new Error(`native client did not finish (${e.signal || e.code})`);
  }
  const files = views.map((_, i) => {
    const tga = path.join(game, 'screenshots', `shot_${i}.tga`);
    if (!fs.existsSync(tga)) throw new Error(`native client wrote no screenshot ${i} (see ${home}/native.log)`);
    const file = path.join(out, `${name}_${i}.png`);
    writePng(file, readTga(tga));
    return file;
  });
  const vf = path.join(game, 'values.txt');
  return { files, values: fs.existsSync(vf) ? parseDebugValues(fs.readFileSync(vf, 'utf8')) : {} };
}

// max-channel difference image of two frames, and stats inside an optional mask
export function frameDiff(a, b, mask = null) {
  let sum = 0, n = 0, over = 0;
  for (let i = 0, p = 0; i < a.data.length; i += 4, p++) {
    if (mask && !mask[p]) continue;
    const d = Math.max(Math.abs(a.data[i] - b.data[i]), Math.abs(a.data[i + 1] - b.data[i + 1]), Math.abs(a.data[i + 2] - b.data[i + 2]));
    sum += d; n++;
    if (d > 16) over++;
  }
  return { mean: n ? sum / n : 0, over: n ? over / n : 0, n };
}

export function uniqueColors(img) {
  const c = new Set();
  for (let i = 0; i < img.data.length; i += 4) c.add(img.data.readUInt32LE(i) & 0xffffff);
  return c.size;
}

export { readPng, writePng };
