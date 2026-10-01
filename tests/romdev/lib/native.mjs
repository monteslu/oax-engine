// native.mjs: run the NATIVE client (build-native) on a map with a script of
// console commands, for comparisons against the cart. Needs an X display
// (DISPLAY, default :9, an Xvfb is fine), OpenArena content (OA_BASEOA) and
// the OA QVMs (OA_QVM_DIR).
//
// Note: in the native client a console `wait N` lasts about N/2 frames.

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { repoRoot } from './romdev.mjs';

export const nativeBinary = path.join(repoRoot, 'build-native', 'Release', 'ioquake3');

export function findBaseoa() {
  for (const c of [process.env.OA_BASEOA, '/usr/share/games/openarena/baseoa', path.join(os.homedir(), '.openarena', 'baseoa')]) {
    if (c && fs.existsSync(c)) return c;
  }
  throw new Error('OpenArena baseoa not found; set OA_BASEOA');
}

export function findQvms() {
  if (process.env.OA_QVM_DIR) return process.env.OA_QVM_DIR;
  const build = path.join(repoRoot, '..', 'oa-gamecode', 'build');
  const dir = fs.existsSync(build) && fs.readdirSync(build).map((d) => path.join(build, d, 'oax')).find((d) => fs.existsSync(path.join(d, 'vm')));
  if (!dir) throw new Error('OA QVMs not found; set OA_QVM_DIR');
  return dir;
}

// A fresh home directory with the QVMs and the test data in it.
export function nativeHome(name) {
  const home = path.join(repoRoot, 'build-native', `home-${name}`);
  const game = path.join(home, 'baseoa');
  fs.rmSync(home, { recursive: true, force: true });
  fs.mkdirSync(game, { recursive: true });
  execFileSync('zip', ['-q', '-r', path.join(game, 'zzz-oa-vm.pk3'), 'vm'], { cwd: findQvms() });
  fs.cpSync(path.join(repoRoot, 'tests', 'romdev', 'data'), game, { recursive: true });
  return home;
}

// Run `map` with cheats and fixedtime 16, then the given console lines, then
// quit. Returns the home directory (outputs land in <home>/baseoa).
export function runNative(name, map, lines, { timeoutMs = 300000, quit = true } = {}) {
  const home = nativeHome(name);
  const cfg = ['fixedtime 16', ...lines, ...(quit ? ['quit'] : [])];
  fs.writeFileSync(path.join(home, 'baseoa', 'native_test.cfg'), cfg.join('\n') + '\n');
  const log = fs.openSync(path.join(home, 'native.log'), 'w');
  execFileSync(nativeBinary, [
    '+set', 'fs_basepath', path.dirname(findBaseoa()), '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
    '+set', 'r_mode', '-1', '+set', 'r_customwidth', '1280', '+set', 'r_customheight', '720', '+set', 'r_fullscreen', '0',
    '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0',
    '+set', 'bot_enable', '0', '+set', 'com_introplayed', '1', '+set', 'com_maxfps', '0',
    '+devmap', map, '+wait', '200', '+exec', 'native_test.cfg',
  ], { stdio: ['ignore', log, log], timeout: timeoutMs, env: { ...process.env, DISPLAY: process.env.DISPLAY || ':9' } });
  return home;
}
