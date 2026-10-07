// The Phase 0 pilot: every tool, in order, on a few maps, ending with a pack,
// the safety checks and a before/after tour. The point is to prove the chain
// runs unattended and to put a first contact sheet in front of a human.
//
//   oacontent pilot [--maps oa_dm6,oa_bases3,oa_ctf4ish] [--eyes 12] [--skip-safety]
// Output: <out>/pilot/ (summary.txt, the tour sheets, the compare sheets), the
// tools' own output under <out>/<tool>/.
import fs from 'node:fs';
import path from 'node:path';
import { OUT } from '../lib/common.mjs';

const STEPS = [
  ['lights', (m) => ({ maps: m, calibrate: true })],
  ['smooth', (m) => ({ maps: m })],
  ['materials', () => ({})],
  ['textures', () => ({ limit: 60 })],
  ['pack', (m) => ({ maps: m })],
  ['fxpreview', () => ({})],
];

export async function run(args = {}) {
  const maps = String(args.maps || 'oa_dm6,oa_bases3,oa_ctf4ish');
  const eyes = String(args.eyes || 12);
  const out = path.join(OUT, 'pilot');
  fs.mkdirSync(out, { recursive: true });
  const log = [];
  const step = async (name, mod, a) => {
    const t = Date.now();
    const m = await import(`./${mod}.mjs`);
    a = { _: [], ...a };
    const origLog = console.log;
    const lines = [];
    console.log = (...x) => { lines.push(x.join(' ')); };
    let code;
    try { code = (await m.run(a)) || 0; } finally { console.log = origLog; }
    const secs = ((Date.now() - t) / 1000).toFixed(0);
    log.push(`${code === 0 ? 'ok  ' : 'FAIL'} ${name} (${secs}s): ${lines.filter(Boolean).slice(-1)[0] || ''}`);
    console.log(log[log.length - 1]);
    return code;
  };
  let failed = 0;
  for (const [mod, mk] of STEPS) failed += (await step(mod, mod, { ...mk(maps) })) ? 1 : 0;
  const pack = path.join(OUT, 'pack', 'zzz-oax-enhanced.pk3');
  if (!args['skip-safety']) failed += (await step('safety', 'safety', { maps: maps.split(',').slice(0, 2).join(','), pack })) ? 1 : 0;
  failed += (await step('tour before', 'tour', { maps, tag: 'pilot-before', eyes, jobs: 2 })) ? 1 : 0;
  failed += (await step('tour after', 'tour', { maps, tag: 'pilot-after', eyes, jobs: 2, pack })) ? 1 : 0;
  failed += (await step('tour compare', 'tour', { _: ['compare'], a: 'pilot-before', b: 'pilot-after', maps })) ? 1 : 0;
  fs.writeFileSync(path.join(out, 'summary.txt'), log.join('\n') + '\n');
  console.log(failed ? `pilot: ${failed} steps failed` : `pilot: all steps ok, see ${out} and ${path.join(OUT, 'tour')}`);
  return failed ? 1 : 0;
}
