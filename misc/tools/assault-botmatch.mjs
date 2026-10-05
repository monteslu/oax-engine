#!/usr/bin/env node
// assault-botmatch: play a bot-only Assault match (g_gametype 14) on the
// native client, headless, and report how it went: per round, when each
// objective fell, the outcome, the winner; how often bots got stuck; how
// many defending bots held their posts.
//
//   node misc/tools/assault-botmatch.mjs --map <name> --home <fs_homepath>
//     [--basepath <dir with baseoa>] [--binary <ioquake3>] [--bots 4]
//     [--skill 3] [--vehicles 1] [--time <round seconds>] [--rounds 1|2]
//     [--sample 300] [--max-minutes 20] [--expect complete] [--json out.json]
//
// --home is an oax home whose baseoa holds the map and the oax game pk3
// (zzz-oax-game.pk3). --rounds 1 stops after round 1 (the attackers' run);
// 2 (default) plays the match out. --expect complete exits 1 unless the
// attackers of round 1 completed every objective within the limit.
// Bot names come from OpenArena's scripts/bots.txt.
//
// It reads the game's debug values (debugvalues; g_as_*, g_navbot_*): see
// the game code's docs/assault.md.

import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..');

function opt(name, def) {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}

const map = opt('map');
const home = opt('home');
if (!map || !home) {
  console.error('usage: assault-botmatch --map <name> --home <fs_homepath> [options]');
  process.exit(2);
}
const basepath = opt('basepath', process.env.OA_BASEOA ? path.dirname(process.env.OA_BASEOA) : '/usr/share/games/openarena');
const binary = opt('binary', path.join(repo, 'build-native', 'Release', 'ioquake3'));
const bots = Number(opt('bots', 4));
const skill = Number(opt('skill', 3));
const rounds = Number(opt('rounds', 2));
const sample = Number(opt('sample', 300));
const maxMinutes = Number(opt('max-minutes', 20));
const expect = opt('expect', '');
const jsonOut = opt('json', '');
const game = path.join(home, 'baseoa');

function botNames() {
  let text = '';
  for (const f of fs.readdirSync(path.join(basepath, 'baseoa')).filter((n) => n.endsWith('.pk3')).sort()) {
    try { text += execFileSync('unzip', ['-p', path.join(basepath, 'baseoa', f), 'scripts/bots.txt'], { encoding: 'latin1', stdio: ['ignore', 'pipe', 'ignore'] }); } catch { /* not in this pk3 */ }
  }
  const names = [...new Set([...text.matchAll(/^\s*name\s+"?([^"\s]+)"?/gim)].map((m) => m[1]))];
  if (names.length < bots * 2) throw new Error(`only ${names.length} bot names in scripts/bots.txt`);
  return names;
}

function parse(file) {
  const v = {};
  for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    const i = line.indexOf(' ');
    if (i > 0) v[line.slice(0, i)] = line.slice(i + 1).trim();
  }
  return v;
}

const tag = `abm_${process.pid}`;
const names = botNames();
// a large map can still be loading for a while: bots added before the
// server runs are lost, so wait well past the load first
const lines = ['wait 800', 'team spectator', 'wait 200'];
names.slice(0, bots).forEach((n) => lines.push(`addbot ${n} ${skill} red`));
names.slice(bots, bots * 2).forEach((n) => lines.push(`addbot ${n} ${skill} blue`));
for (let i = 0; i < 2000; i++) lines.push(`wait ${sample}`, `debugvalues ${tag}_${i}.txt`);
lines.push('quit');
fs.writeFileSync(path.join(game, `${tag}.cfg`), lines.join('\n') + '\n');
fs.rmSync(path.join(game, 'ioq3.pid'), { force: true });

const args = [
  '+set', 'fs_basepath', basepath, '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
  '+set', 'sv_pure', '0', '+set', 'com_introplayed', '1', '+set', 'g_gametype', '14', '+set', 'bot_enable', '1',
  '+set', 'g_oaxVehicles', opt('vehicles', '1'), '+set', 'r_mode', '-1', '+set', 'r_customwidth', '640',
  '+set', 'r_customheight', '360', '+set', 'r_fullscreen', '0', '+set', 'com_maxfps', '125', '+set', 's_volume', '0',
  ...(opt('time') ? ['+set', 'g_oaxAssaultTime', opt('time')] : []),
  '+devmap', map, '+exec', `${tag}.cfg`,
];
const env = { ...process.env, SDL_VIDEODRIVER: 'offscreen' };
delete env.DISPLAY;
delete env.WAYLAND_DISPLAY;
// the console to a file, as a shell redirect would (not a pipe this process
// has to keep draining)
const logFile = path.join(game, `${tag}.log`);
const logFd = fs.openSync(logFile, 'w');
const child = spawn(binary, args, { env, stdio: ['ignore', logFd, logFd] });
const readLog = () => { try { return fs.readFileSync(logFile, 'latin1'); } catch { return ''; } };

// the match as the dumps show it
const roundsSeen = {};
let last = null, idx = 0, done = false;
const started = Date.now();

function track(v) {
  const r = Number(v.g_as_round);
  if (!r) return;
  const R = (roundsSeen[r] ??= { round: r, attackers: Number(v.g_as_attackers), limit: Number(v.g_as_limit), fell: {}, outcome: 0 });
  for (let i = 0; i < 8; i++) {
    const o = v[`g_as_obj${i}`];
    if (!o) continue;
    const [id, , state] = o.split(' ');
    if (Number(state) === 2 && !(id in R.fell)) R.fell[id] = Math.round((R.limit - Number(v.g_as_left)) / 1000);
  }
  if (Number(v.g_as_phase) === 2) R.outcome = Number(v.g_as_outcome);
  R.winner = Number(v.g_as_winner);
  R.navbots = Math.max(R.navbots ?? 0, Number(v.g_navbots ?? 0));
  R.stuck = Number(v.g_navbot_stuck);
  const [attackN, guardN, guardNear, guardMean] = String(v.g_navbot_assault ?? '0 0 0 0').split(' ').map(Number);
  R.posted = (R.posted ?? []).concat([{ attackN, guardN, guardNear, guardMean }]);
  if (Number(v.g_as_phase) === 2 && (r >= rounds)) done = true;
}

await new Promise((resolve) => {
  const timer = setInterval(() => {
    for (;;) {
      const f = path.join(game, `${tag}_${idx}.txt`);
      if (!fs.existsSync(f)) break;
      try { last = parse(f); track(last); } catch { break; }
      fs.rmSync(f, { force: true });
      idx++;
    }
    if (done || Date.now() - started > maxMinutes * 60000) {
      clearInterval(timer);
      // SIGKILL: the client's SIGTERM handler can deadlock in the renderer shutdown
      child.kill('SIGKILL');
      resolve();
    }
  }, 2000);
  child.on('exit', () => { clearInterval(timer); resolve(); });
});
fs.rmSync(path.join(game, `${tag}.cfg`), { force: true });
for (const f of fs.readdirSync(game).filter((n) => n.startsWith(`${tag}_`))) fs.rmSync(path.join(game, f), { force: true });

// the report
const report = { map, bots, skill, rounds: Object.values(roundsSeen), samples: idx, finished: done };
for (const R of report.rounds) {
  const p = R.posted.filter((x) => x.guardN > 0);
  R.defendersPosted = p.length ? +(p.reduce((a, x) => a + x.guardNear / x.guardN, 0) / p.length).toFixed(2) : null;
  R.defenderMeanDist = p.length ? Math.round(p.reduce((a, x) => a + x.guardMean, 0) / p.length) : null;
  delete R.posted;
  const team = R.attackers === 1 ? 'red' : 'blue';
  console.log(`round ${R.round}: ${team} attacks, limit ${R.limit / 1000} s; fell: ${Object.entries(R.fell).map(([id, t]) => `${id} at ${t} s`).join(', ') || 'nothing'}; ` +
    `outcome ${R.outcome === 1 ? 'made it' : R.outcome === 2 ? 'ran out of time' : 'unfinished'}; defenders at their posts ${R.defendersPosted ?? '-'} (mean ${R.defenderMeanDist ?? '-'} units); bot stuck events ${R.stuck}; navmesh bots ${R.navbots} of ${bots * 2}`);
  if (R.navbots < bots * 2) console.log(`  warning: only ${R.navbots} of ${bots * 2} bots ran as navmesh bots (missing bots joined late, failed to join, or are stock AAS bots)`);
}
const fin = report.rounds.at(-1);
if (fin?.winner) console.log(`winner: ${fin.winner === 1 ? 'red' : 'blue'}`);
if (!done) console.log(`stopped after ${idx} samples (${maxMinutes} min cap or the client exited)`);
if (last) console.log(`last sample: round ${last.g_as_round} phase ${last.g_as_phase} left ${last.g_as_left} ms, navmesh bots ${last.g_navbots ?? 0}, level time ${last.g_level_rel ?? '?'}`);
if (last?.g_navbot_stuck_at) console.log(`last stuck spots (x,y,z,goal kind): ${last.g_navbot_stuck_at}`);
if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify(report, null, 2));
let ok = true;
if (expect === 'complete') {
  const r1 = roundsSeen[1];
  ok = !!r1 && r1.outcome === 1;
  console.log(ok ? 'PASS: round 1 attackers completed every objective' : 'FAIL: round 1 attackers did not complete the objectives');
}
if (!idx) console.log(readLog().split('\n').slice(-20).join('\n'));
fs.closeSync(logFd);
// keep the console when something looks wrong, for the post-mortem
const crashed = /Server crashed|ERROR: /.test(readLog());
if (crashed) {
  ok = false;
  console.log(`FAIL: the server crashed: ${(readLog().match(/(ERROR: .*|Server crashed.*)/) || [''])[0]}`);
}
const short = crashed || Object.values(roundsSeen).some((R) => (R.navbots ?? 0) < bots * 2);
if (short || !idx) {
  const keep = path.join(game, `assault-botmatch-${process.pid}.log`);
  fs.renameSync(logFile, keep);
  console.log(`console log kept: ${keep}`);
} else {
  fs.rmSync(logFile, { force: true });
}
process.exit(ok ? 0 : 1);
