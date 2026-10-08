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
  for (const want of ['oax/metal_sparks', 'oax/dust_puff', 'oax/fireball', 'oax/debris', 'oax/shockwave', 'oax/flame', 'oax/flame_small']) {
    if (!names.has(want)) failures.push(`library decl ${want} is not shipped`);
  }
  // control
  const prt = path.join(out, 'invisible.prt');
  fs.writeFileSync(prt, 'particle oaxtest/invisible {\n\t{\n\t\tcount 8\n\t\tmaterial oaxfx/spark\n\t\ttime 1\n\t\tcycles 1\n\t\tdistribution sphere 2 2 2\n\t\tspeed "50" to "10"\n\t\tsize "1" to "1"\n\t\tcolor 0 0 0 0\n\t\tfadeColor 0 0 0 0\n\t}\n}\n');
  const ctl = spawnSync('node', [TOOL, 'fxpreview', '--decls', 'oaxtest/invisible', '--prt', prt, '--out', path.join(out, 'control')], { env: env(), encoding: 'utf8', timeout: 600000 });
  rows.push(`control (invisible decl): exit ${ctl.status}`);
  if (ctl.status !== 1 || !/empty-room frame/.test(String(ctl.stdout))) failures.push('control did not fail: an invisible decl passed fxpreview');
  return { ok: failures.length === 0, failures, rows };
}
