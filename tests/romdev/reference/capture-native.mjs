#!/usr/bin/env node
// capture-native.mjs: render the golden cameras with the NATIVE client's
// desktop GL renderer, as the reference the cart's GLES renderer is held to.
//
//   DISPLAY=:9 node tests/romdev/reference/capture-native.mjs [map ...]
//
// Needs build-native/ (cmake build), an X display (Xvfb is fine), the
// OpenArena content (OA_BASEOA) and QVMs (OA_QVM_DIR). Writes half-size PNGs
// to tests/romdev/reference/native/<map>_<i>.png from the cameras in
// tests/romdev/goldens/render/<map>.json.

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { halfSize, writePng } from '../lib/png.mjs';
import { viewFor } from '../lib/scenes.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..', '..', '..');
const goldens = path.join(root, 'tests', 'romdev', 'goldens', 'render');
const outDir = path.join(here, 'native');
const binary = path.join(root, 'build-native', 'Release', 'ioquake3');

function findBaseoa() {
  for (const c of [process.env.OA_BASEOA, '/usr/share/games/openarena/baseoa', path.join(os.homedir(), '.openarena', 'baseoa')]) {
    if (c && fs.existsSync(c)) return c;
  }
  throw new Error('OpenArena baseoa not found; set OA_BASEOA');
}

function findQvms() {
  const dir = process.env.OA_QVM_DIR
    || fs.readdirSync(path.join(root, '..', 'oa-gamecode', 'build')).map((d) => path.join(root, '..', 'oa-gamecode', 'build', d, 'oax'))[0];
  if (!dir || !fs.existsSync(path.join(dir, 'vm'))) throw new Error('OA QVMs not found; set OA_QVM_DIR');
  return dir;
}

// Uncompressed (type 2) or RLE (type 10) 24/32-bit TGA, as the engine writes.
function readTga(file) {
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

function capture(map, cameras, home) {
  const shots = path.join(home, 'baseoa', 'screenshots');
  fs.rmSync(shots, { recursive: true, force: true });
  const lines = ['fixedtime 16', 'cg_drawGun 0', 'cg_draw2D 0', 'con_notifytime 0', 'bot_enable 0', 'r_fixedShaderTime 100', 'wait 60'];
  cameras.forEach((c, i) => {
    lines.push(`cl_overrideView "${viewFor(c)}"`, 'wait 40', `screenshot ref_${map}_${i}`, 'wait 2');
  });
  lines.push('quit');
  fs.writeFileSync(path.join(home, 'baseoa', 'refshots.cfg'), lines.join('\n') + '\n');
  execFileSync(binary, [
    '+set', 'fs_basepath', path.dirname(findBaseoa()), '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
    '+set', 'r_mode', '-1', '+set', 'r_customwidth', '1280', '+set', 'r_customheight', '720', '+set', 'r_fullscreen', '0',
    '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0',
    '+set', 'bot_enable', '0', '+set', 'com_introplayed', '1', '+set', 'r_renderer', 'opengl2',
    '+set', 'fixedtime', '16', '+set', 'com_maxfps', '0',
    '+devmap', map, '+wait', '200', '+exec', 'refshots.cfg',
  ], { stdio: ['ignore', fs.openSync(path.join(home, 'native.log'), 'w'), fs.openSync(path.join(home, 'native.err'), 'w')], timeout: 300000 });
  return cameras.map((_, i) => path.join(shots, `ref_${map}_${i}.tga`));
}

const maps = process.argv.slice(2).length
  ? process.argv.slice(2)
  : fs.readdirSync(goldens).filter((f) => f.endsWith('.json')).map((f) => f.replace(/\.json$/, ''));
const home = path.join(root, 'build-native', 'ref-home');
fs.mkdirSync(path.join(home, 'baseoa'), { recursive: true });
const pk3 = path.join(home, 'baseoa', 'zzz-oa-vm.pk3');
fs.rmSync(pk3, { force: true });
execFileSync('zip', ['-q', '-r', pk3, 'vm'], { cwd: findQvms() });
fs.mkdirSync(outDir, { recursive: true });
for (const map of maps) {
  const { cameras } = JSON.parse(fs.readFileSync(path.join(goldens, `${map}.json`), 'utf8'));
  const files = capture(map, cameras, home);
  files.forEach((f, i) => {
    if (!fs.existsSync(f)) throw new Error(`${map}#${i}: native client wrote no screenshot`);
    writePng(path.join(outDir, `${map}_${i}.png`), halfSize(readTga(f)));
  });
  console.log(`${map}: ${files.length} reference views`);
}
