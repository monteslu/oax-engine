// native.mjs: run the NATIVE client (build-native) on a map with a script of
// console commands, for comparisons against the cart. Needs an X display
// (a private Xvfb started on demand, or OA_NATIVE_DISPLAY), OpenArena content (OA_BASEOA) and
// the OA QVMs (OA_QVM_DIR).
//
// Note: in the native client a console `wait N` lasts about N/2 frames.

import { execFileSync, spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { repoRoot } from './romdev.mjs';
import { nativeCaptureArgs } from './capture.mjs';

export const nativeBinary = path.join(repoRoot, 'build-native', 'Release', 'ioquake3');

// The X display native runs render on: OA_NATIVE_DISPLAY if set; else a
// private Xvfb for this test process when Xvfb is installed (on a shared
// display another session's window can disturb a run mid-test); else the
// headless Xvfb :9 when it exists; DISPLAY only as a last resort (it is
// usually the desktop, where the test window could take or lose focus).
let privateDisplay = null;
function hasXvfb() {
  return (process.env.PATH || '').split(':').some((d) => d && fs.existsSync(`${d}/Xvfb`));
}
export function nativeDisplay() {
  if (process.env.OA_NATIVE_DISPLAY) return process.env.OA_NATIVE_DISPLAY;
  if (privateDisplay) return privateDisplay;
  if (!hasXvfb()) return fs.existsSync('/tmp/.X11-unix/X9') ? ':9' : (process.env.DISPLAY || ':9');
  for (let n = 100 + (process.pid % 300); n < 1000; n++) {
    if (fs.existsSync(`/tmp/.X${n}-lock`) || fs.existsSync(`/tmp/.X11-unix/X${n}`)) continue;
    const x = spawn('Xvfb', [`:${n}`, '-screen', '0', '1280x1024x24', '-nolisten', 'tcp'], { stdio: 'ignore', detached: false });
    x.on('error', () => {});
    const until = Date.now() + 10000;
    while (!fs.existsSync(`/tmp/.X11-unix/X${n}`) && Date.now() < until) execFileSync('sleep', ['0.1']);
    if (!fs.existsSync(`/tmp/.X11-unix/X${n}`)) { try { x.kill(); } catch { /* never started */ } continue; }
    process.on('exit', () => { try { x.kill(); } catch { /* gone */ } });
    privateDisplay = `:${n}`;
    return privateDisplay;
  }
  throw new Error('no free X display for a private Xvfb; set OA_NATIVE_DISPLAY');
}

export function findBaseoa() {
  for (const c of [process.env.OA_BASEOA, '/usr/share/games/openarena/baseoa', path.join(os.homedir(), '.openarena', 'baseoa')]) {
    if (c && fs.existsSync(c)) return c;
  }
  throw new Error('OpenArena baseoa not found; set OA_BASEOA');
}

// The game modules: OA_QVM_DIR (a directory holding vm/*.qvm), else a build
// of the sibling oax-gamecode checkout (build/release-<os>-<arch>/oax).
export function findQvms() {
  if (process.env.OA_QVM_DIR) return process.env.OA_QVM_DIR;
  for (const repo of ['oax-gamecode', 'oa-gamecode']) {
    const build = path.join(repoRoot, '..', repo, 'build');
    const dir = fs.existsSync(build) && fs.readdirSync(build).map((d) => path.join(build, d, 'oax')).find((d) => fs.existsSync(path.join(d, 'vm')));
    if (dir) return dir;
  }
  throw new Error('game QVMs not found: build https://github.com/monteslu/oax-gamecode next to the engine, or set OA_QVM_DIR');
}

// Run the native client with the test's arguments. Errors are never
// swallowed: com_errorQuit 1 makes any engine error (a missing map, a bad
// QVM trap, ERR_DROP) end the process at once, and a non-zero exit, a
// signal or a timeout throws with the error lines and the log tail, so a
// test fails on the spot with the reason instead of waiting.
export function execNative(args, { home, timeout = 300000, logName = 'native.log' }) {
  const logPath = path.join(home, logName);
  const log = fs.openSync(logPath, 'w');
  try {
    execFileSync(nativeBinary, ['+set', 'com_errorQuit', '1', ...args], {
      // SIGKILL at the limit: the client's SIGTERM handler runs the whole
      // renderer shutdown inside the signal handler and can deadlock there
      // (nav-bots under load sat 56 min in a futex after its 900 s limit)
      stdio: ['ignore', log, log], timeout, killSignal: 'SIGKILL', env: { ...process.env, DISPLAY: nativeDisplay() },
    });
  } catch (e) {
    const text = fs.existsSync(logPath) ? fs.readFileSync(logPath, 'utf8') : '';
    const errors = text.split('\n').filter((l) => /Sys_Error|^ERROR|com_errorQuit|Bad (game|cgame|ui) system trap|\*\*\*\*\* ERROR/.test(l)).slice(-5);
    // the client traps SIGTERM and exits 1 itself, so a timeout can arrive as
    // a plain exit status: name it from the error code, not the signal
    const timedOut = e.code === 'ETIMEDOUT' || e.error?.code === 'ETIMEDOUT';
    const why = timedOut ? `timed out after ${timeout} ms` : e.signal ? `killed by ${e.signal}` : `exit ${e.status}`;
    throw new Error(`native client ${why}: ${errors.join(' | ') || '(no error line)'}\n--- last log lines (${logPath}):\n${text.split('\n').slice(-15).join('\n')}`);
  } finally {
    fs.closeSync(log);
  }
  return logPath;
}

// A fresh home directory with the QVMs and the test data in it.
export function nativeHome(name) {
  const home = path.join(repoRoot, 'build-native', `home-${name}`);
  const game = path.join(home, 'baseoa');
  fs.rmSync(home, { recursive: true, force: true });
  fs.mkdirSync(game, { recursive: true });
  // the QVMs, plus the data the oax game modules ship next to them
  const parts = ['vm', 'script', 'models', 'particles', 'scripts'].filter((d) => fs.existsSync(path.join(findQvms(), d)));
  execFileSync('zip', ['-q', '-r', path.join(game, 'zzz-oa-vm.pk3'), ...parts], { cwd: findQvms() });
  fs.cpSync(path.join(repoRoot, 'tests', 'romdev', 'data'), game, { recursive: true });
  // compiled test maps (tests/maps/build.mjs), as the cart packs them
  const maps = path.join(repoRoot, 'tests', 'maps', 'out', 'baseoa');
  if (fs.existsSync(maps)) fs.cpSync(maps, game, { recursive: true });
  return home;
}

// Run `map` with cheats and fixedtime 16, then the given console lines, then
// quit. Returns the home directory (outputs land in <home>/baseoa). `set`:
// cvars set on the command line before the map loads (latched server rules).
export function runNative(name, map, lines, { timeoutMs = 300000, quit = true, set = {}, picmip } = {}) {
  const home = nativeHome(name);
  const cfg = ['fixedtime 16', ...lines, ...(quit ? ['quit'] : [])];
  fs.writeFileSync(path.join(home, 'baseoa', 'native_test.cfg'), cfg.join('\n') + '\n');
  execNative([
    '+set', 'fs_basepath', path.dirname(findBaseoa()), '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
    ...nativeCaptureArgs({ picmip }),
    '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0',
    // fixedtime from the first frame: set only after the map loads, the
    // early frames ran on the wall clock, which left the server's 50 ms frame
    // phase against the client different from run to run (native-parity flake)
    '+set', 'bot_enable', '0', '+set', 'com_introplayed', '1', '+set', 'com_maxfps', '0', '+set', 'fixedtime', '16',
    ...Object.entries(set).flatMap(([k, v]) => ['+set', k, String(v)]),
    '+devmap', map, '+wait', '200', '+exec', 'native_test.cfg',
  ], { home, timeout: timeoutMs });
  return home;
}
