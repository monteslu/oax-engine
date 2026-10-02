// nativeshot.mjs: screenshots and debug values from the NATIVE client, for
// render tests that check the cart against the desktop build. Like
// native.mjs's runNative, with a choice of QVM directory (stock-QVM runs)
// and a list of shots, each a console line run before a screenshot.

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { repoRoot } from './romdev.mjs';
import { nativeBinary, findBaseoa, findQvms } from './native.mjs';
import { readTga } from './tga.mjs';
import { parseDebugValues } from './values.mjs';

// shots: [{ cmd, name, values, settle }]: run cmd, wait settle frames, then
// screenshot `name` and/or dump the debug values as `values`. Returns
// { images: {name: img}, values (at the end), valuesAt: {values: {...}}, home }
export function nativeShots(tag, map, shots, { qvmDir = findQvms(), setup = [], settle = 40, timeoutMs = 300000, startArgs = [] } = {}) {
  const home = path.join(repoRoot, 'build-native', `home-${tag}`);
  const game = path.join(home, 'baseoa');
  fs.rmSync(home, { recursive: true, force: true });
  fs.mkdirSync(game, { recursive: true });
  // the QVMs, plus the data the oax game modules ship next to them
  const parts = ['vm', 'script', 'models', 'particles', 'scripts'].filter((d) => fs.existsSync(path.join(qvmDir, d)));
  execFileSync('zip', ['-q', '-r', path.join(game, 'zzz-oa-vm.pk3'), ...parts], { cwd: qvmDir });
  fs.cpSync(path.join(repoRoot, 'tests', 'romdev', 'data'), game, { recursive: true });
  const maps = path.join(repoRoot, 'tests', 'maps', 'out', 'baseoa');
  if (fs.existsSync(maps)) fs.cpSync(maps, game, { recursive: true });

  // the map can still be loading when this runs (cheat cvars would be
  // refused): wait, then set the view twice
  const view = ['cg_drawGun 0', 'cg_draw2D 0', 'cg_drawFPS 0', 'con_notifytime 0', 'bot_enable 0', 'g_doWarmup 0', ...setup];
  const lines = ['fixedtime 16', 'wait 300', ...view, 'wait 60', ...view, 'wait 60'];
  for (const s of shots) {
    if (s.cmd) lines.push(s.cmd);
    lines.push(`wait ${s.settle ?? settle}`);
    if (s.name) lines.push(`screenshot shot_${s.name}`, 'wait 2');
    if (s.values) lines.push(`debugvalues values_${s.values}.txt`, 'wait 2');
  }
  lines.push('debugvalues values.txt', 'wait 2', 'quit');
  fs.writeFileSync(path.join(game, 'shots.cfg'), lines.join('\n') + '\n');
  execFileSync(nativeBinary, [
    '+set', 'fs_basepath', path.dirname(findBaseoa()), '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
    '+set', 'r_mode', '-1', '+set', 'r_customwidth', '1280', '+set', 'r_customheight', '720', '+set', 'r_fullscreen', '0',
    '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0',
    '+set', 'bot_enable', '0', '+set', 'com_introplayed', '1', '+set', 'com_maxfps', '0', '+set', 'fixedtime', '16',
    ...startArgs, '+devmap', map, '+wait', '200', '+exec', 'shots.cfg',
  ], {
    stdio: ['ignore', fs.openSync(path.join(home, 'native.log'), 'w'), fs.openSync(path.join(home, 'native.err'), 'w')],
    timeout: timeoutMs, env: { ...process.env, DISPLAY: process.env.DISPLAY || ':9' },
  });
  const images = {};
  for (const s of shots) {
    if (!s.name) continue;
    const f = path.join(game, 'screenshots', `shot_${s.name}.tga`);
    if (fs.existsSync(f)) images[s.name] = readTga(f);
  }
  const read = (f) => (fs.existsSync(f) ? parseDebugValues(fs.readFileSync(f, 'utf8')) : {});
  const values = read(path.join(game, 'values.txt'));
  const valuesAt = {};
  for (const s of shots) if (s.values) valuesAt[s.values] = read(path.join(game, `values_${s.values}.txt`));
  return { images, values, valuesAt, home };
}
