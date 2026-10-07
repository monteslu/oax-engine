// render.mjs: runs the native client display-free (offscreen video driver,
// software GL unless asked otherwise) over a list of eyes in one map and
// collects the frames and the per-frame debug values.
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { DEFAULT_BASEOA } from './packs.mjs';
import { REPO } from './common.mjs';

export const NATIVE = path.join(REPO, 'build-native', 'Release', 'ioquake3');

export function findQvms() {
  if (process.env.OA_QVM_DIR) return process.env.OA_QVM_DIR;
  for (const repo of ['oax-gamecode', 'oa-gamecode']) {
    const build = path.join(REPO, '..', repo, 'build');
    const dir = fs.existsSync(build) && fs.readdirSync(build).map((d) => path.join(build, d, 'oax')).find((d) => fs.existsSync(path.join(d, 'vm')));
    if (dir) return dir;
  }
  throw new Error('game QVMs not found: build oax-gamecode next to the engine or set OA_QVM_DIR');
}

// a fresh home with the game modules, the extra pk3s and loose files (path ->
// Buffer/string under baseoa) the run needs
export function makeHome(dir, { files = {}, packs = [] } = {}) {
  fs.rmSync(dir, { recursive: true, force: true });
  const game = path.join(dir, 'baseoa');
  fs.mkdirSync(game, { recursive: true });
  const q = findQvms();
  const parts = ['vm', 'script', 'models', 'particles', 'scripts'].filter((d) => fs.existsSync(path.join(q, d)));
  execFileSync('zip', ['-q', '-r', path.join(game, 'zzz-oa-vm.pk3'), ...parts], { cwd: q });
  for (const p of packs) fs.copyFileSync(p, path.join(game, path.basename(p)));
  for (const [rel, data] of Object.entries(files)) {
    const f = path.join(game, rel);
    fs.mkdirSync(path.dirname(f), { recursive: true });
    fs.writeFileSync(f, data);
  }
  return dir;
}

const CLEAN = ['r_oaxProfile 1', 'bot_enable 0', 'cg_drawGun 0', 'cg_draw2D 0', 'cg_drawFPS 0', 'g_doWarmup 0', 'con_notifytime 0', 'r_fixedShaderTime 100', 'cg_drawCrosshair 0'];

// eyes: [{ eye: [x, y, z], angles: [pitch, yaw, roll] }]; returns when the client has quit
export function renderEyes({ map, eyes, home, baseoa = DEFAULT_BASEOA, size = [640, 360], cvars = {}, settle = 20, timeoutMs = 900000, gpu = false, binary = NATIVE, log = 'native.log', clean = CLEAN }) {
  const game = path.join(home, 'baseoa');
  fs.rmSync(path.join(game, 'screenshots'), { recursive: true, force: true });
  fs.mkdirSync(path.join(game, 'screenshots'), { recursive: true });
  fs.rmSync(path.join(game, 'ioq3.pid'), { force: true });
  // the cheat cvars of the clean view only take once the map has loaded
  const lines = ['fixedtime 16', 'wait 150', ...clean, 'wait 10'];
  eyes.forEach((e, i) => {
    const n = String(i).padStart(3, '0');
    if (e.cmd) lines.push(e.cmd);
    lines.push(`cl_overrideView "${e.eye.map((v) => +v.toFixed(2)).join(' ')} ${e.angles.map((v) => +v.toFixed(2)).join(' ')}"`, `wait ${e.settle ?? settle}`, `screenshot tour_${n}`, 'wait 2', `debugvalues tourv_${n}.txt`, 'wait 1');
  });
  lines.push('quit');
  fs.writeFileSync(path.join(game, 'tour.cfg'), lines.join('\n') + '\n');
  const args = ['+set', 'com_errorQuit', '1', '+set', 'fs_basepath', path.dirname(baseoa), '+set', 'com_basegame', path.basename(baseoa), '+set', 'fs_homepath', home,
    '+set', 'r_mode', '-1', '+set', 'r_customwidth', String(size[0]), '+set', 'r_customheight', String(size[1]), '+set', 'r_fullscreen', '0',
    '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0', '+set', 'com_introplayed', '1',
    '+set', 'fixedtime', '16', '+set', 'com_maxfps', '0', '+set', 'sv_cheats', '1', '+set', 'r_picmip', '0',
    ...Object.entries(cvars).flatMap(([k, v]) => ['+set', k, String(v)]),
    '+devmap', map, '+exec', 'tour.cfg'];
  const env = { ...process.env, SDL_VIDEODRIVER: 'offscreen', SDL_AUDIODRIVER: 'dummy' };
  delete env.DISPLAY; delete env.WAYLAND_DISPLAY;
  if (!gpu) env.LIBGL_ALWAYS_SOFTWARE = '1';
  return new Promise((resolve) => {
    const out = fs.openSync(path.join(home, log), 'w');
    const child = spawn(binary, args, { env, stdio: ['ignore', out, out] });
    const timer = setTimeout(() => child.kill('SIGKILL'), timeoutMs);
    child.on('exit', (code, signal) => { clearTimeout(timer); fs.closeSync(out); resolve({ code, signal, frames: fs.readdirSync(path.join(game, 'screenshots')).filter((f) => /^tour_\d+\.tga$/.test(f)).length }); });
  });
}
