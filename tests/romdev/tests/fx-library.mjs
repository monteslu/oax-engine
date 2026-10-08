// The effect library (particles/oax_*.prt, misc/oacontent fxpreview): every
// shipped particle decl renders something visible on the oax_fx map at
// fixed frames after its spawn, measured against an empty-room frame.
// Control: a decl that draws nothing (an invisible additive colour) must be
// reported as the empty-room frame, or the check could not fail.

import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { repoRoot } from '../lib/romdev.mjs';
import { findQvms } from '../lib/native.mjs';
import { readPng } from '../lib/png.mjs';
import { meanRGB } from '../lib/imgstat.mjs';

export const name = 'fx-library';

const TOOL = path.join(repoRoot, 'misc', 'oacontent', 'oacontent.mjs');
const env = () => ({ ...process.env, OA_QVM_DIR: findQvms() });

export async function run() {
  const failures = [], rows = [];
  const out = path.join(repoRoot, 'build-native', 'fxlibrary');
  fs.mkdirSync(out, { recursive: true });
  const all = spawnSync('node', [TOOL, 'fxpreview', '--out', path.join(out, 'all')], { env: env(), encoding: 'utf8', timeout: 900000 });
  rows.push(String(all.stdout).trim().split('\n')[0]);
  if (all.status !== 0) failures.push(`fxpreview over the shipped decls exited ${all.status}: ${String(all.stdout).split('\n').slice(1, 6).join(' | ')}${all.stderr ? ' ' + String(all.stderr).slice(0, 200) : ''}`);
  const j = JSON.parse(fs.readFileSync(path.join(out, 'all', 'fxpreview.json'), 'utf8'));
  const names = new Set(j.rows.map((r) => r.decl));
  for (const want of ['oax/metal_sparks', 'oax/dust_puff', 'oax/fireball', 'oax/debris', 'oax/shockwave', 'oax/flame', 'oax/flame_small', 'oax/rain', 'oax/snow', 'oax/heat_haze']) {
    if (!names.has(want)) failures.push(`library decl ${want} is not shipped`);
  }
  // control
  const prt = path.join(out, 'invisible.prt');
  fs.writeFileSync(prt, 'particle oaxtest/invisible {\n\t{\n\t\tcount 8\n\t\tmaterial oaxfx/spark\n\t\ttime 1\n\t\tcycles 1\n\t\tdistribution sphere 2 2 2\n\t\tspeed "50" to "10"\n\t\tsize "1" to "1"\n\t\tcolor 0 0 0 0\n\t\tfadeColor 0 0 0 0\n\t}\n}\n');
  const ctl = spawnSync('node', [TOOL, 'fxpreview', '--decls', 'oaxtest/invisible', '--prt', prt, '--out', path.join(out, 'control')], { env: env(), encoding: 'utf8', timeout: 600000 });
  rows.push(`control (invisible decl): exit ${ctl.status}`);
  if (ctl.status !== 1 || !/empty-room frame/.test(String(ctl.stdout))) failures.push('control did not fail: an invisible decl passed fxpreview');
  // lit smoke: the same smoke with `lit 1` takes the light grid's colour at its origin, so it
  // differs from the unlit one (darker in this room), and each run is its own session
  const smoke = (lit) => `particle oaxtest/smoke${lit} {\n\t{\n\t\tcount 12\n\t\tmaterial oaxfx/smoke\n\t\ttime 3\n\t\tcycles 1\n\t\tbunching 1\n\t\tdistribution sphere 10 10 4\n\t\tdirection cone 20\n\t\tspeed "6" to "2"\n\t\tsize "30" to "40"\n\t\tcolor 1 1 1 0.9\n\t\tfadeColor 1 1 1 0.9\n${lit ? '\t\tlit 1\n' : ''}\t\tsoftDistance -1\n\t}\n}\n`;
  const mean = [];
  for (const lit of [0, 1]) {
    const p = path.join(out, `smoke${lit}.prt`);
    fs.writeFileSync(p, smoke(lit));
    const o = path.join(out, `smoke${lit}`);
    spawnSync('node', [TOOL, 'fxpreview', '--decls', `oaxtest/smoke${lit}`, '--prt', p, '--frames', '30', '--out', o], { env: env(), encoding: 'utf8', timeout: 600000 });
    const img = readPng(path.join(o, 'frames', `oaxtest_smoke${lit}_30.png`));
    const m = meanRGB(img, { x0: 0.4, x1: 0.6, y0: 0.55, y1: 0.8 });
    mean.push((m[0] + m[1] + m[2]) / 3);
  }
  rows.push(`smoke brightness in the spawn box: unlit ${mean[0].toFixed(1)}, lit ${mean[1].toFixed(1)}`);
  if (!(mean[1] < mean[0] - 2)) failures.push(`lit smoke is not darker than unlit in this room (${mean[1].toFixed(1)} against ${mean[0].toFixed(1)})`);
  return { ok: failures.length === 0, failures, rows };
}
