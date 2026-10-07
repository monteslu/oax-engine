// T9 safety checks: what an enhancement pack must not change, measured
// against the same maps without the pack.
//
//   oacontent safety [--pack file.pk3] [--maps a,b] [--out dir] [--bots 4]
//                    [--frames 600] [--budget 0] [--max-draws 2500] [--gpu]
// Per map, two runs with bots playing (A: stock, B: stock + the pack):
//   identity  the gameplay-visible debug values agree: navmesh polygons and
//             hash (the collision geometry the bots path over), item count
//             (the items bots pick up change from run to run, so only the count); any difference fails;
//   smoke     the client exits cleanly, the log has no error line, the bots
//             were added and the snapshot carries entities;
//   budget    B's draw calls at the spawn view are under --max-draws; with --budget N
//             B's mean frame time (r_oaxProfile) must also be within N times A's
//             Frame time is REPORTED but not gated by default: the software
//             rasteriser the display-free runs use is fill-bound and says little
//             about a GPU (measure on hardware with --gpu --budget 2).
// Self-test: the identity comparison is also made between two DIFFERENT maps
// and must report a difference (a check that cannot fail is not a check);
// if it does not, the whole run fails as inconclusive.
// Output: <out>/safety/report.{json,txt}; exit 1 on any failure.
import fs from 'node:fs';
import path from 'node:path';
import { OUT } from '../lib/common.mjs';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp } from '../lib/bsp.mjs';
import { parseEntities } from '../lib/entities.mjs';
import { makeHome, renderEyes } from '../lib/render.mjs';
import { readTga } from '../../../tests/romdev/lib/tga.mjs';
import { parseDebugValues } from '../../../tests/romdev/lib/values.mjs';

const CLEAN = ['r_oaxProfile 1', 'cg_drawGun 0', 'cg_draw2D 0', 'cg_drawFPS 0', 'g_doWarmup 0', 'con_notifytime 0', 'r_fixedShaderTime 100', 'cg_drawCrosshair 0', 'bot_enable 1'];
export const IDENTITY_KEYS = ['sv_nav_polys', 'sv_nav_hash', 'sv_nav_links', 'sv_nav_areas', 'sv_nav_models', 'g_items_total'];
const DEFAULT_MAPS = ['oa_dm1', 'oa_dm6', 'oa_bases3'];

export function compareIdentity(a, b) {
  return IDENTITY_KEYS.filter((k) => String(a[k]) !== String(b[k])).map((k) => `${k}: ${String(a[k]).slice(0, 60)} vs ${String(b[k]).slice(0, 60)}`);
}

function spawnEye(map) {
  const cs = new ContentSet([DEFAULT_BASEOA]);
  const bsp = new Bsp(cs.read(`maps/${map}.bsp`));
  const sp = parseEntities(bsp.entityText).find((e) => /^info_player_(deathmatch|start)$/.test(e.classname || ''));
  const p = sp ? sp.origin : [0, 0, 64];
  return { eye: [p[0], p[1], p[2] + 26], angles: [0, Number(sp?.get('angle') || 0), 0] };
}

async function one(label, map, outDir, o) {
  const home = makeHome(path.join(outDir, 'home', `${map}-${label}`), { packs: label === 'B' ? [o.pack] : [] });
  const cmds = [];
  for (let i = 0; i < o.bots; i++) cmds.push(`addbot ${['sarge', 'grunt', 'major', 'visor'][i % 4]} 3`);
  const eyes = [{ ...spawnEye(map), cmd: cmds.join(';'), settle: o.frames }];
  const r = await renderEyes({ map, eyes, home, size: [320, 180], settle: o.frames, gpu: o.gpu, clean: CLEAN, cvars: { bot_enable: 1, g_gametype: 0 }, timeoutMs: 600000 });
  const log = fs.readFileSync(path.join(home, 'native.log'), 'utf8');
  const vf = path.join(home, 'baseoa', 'tourv_000.txt');
  const v = fs.existsSync(vf) ? parseDebugValues(fs.readFileSync(vf, 'utf8')) : {};
  const errors = log.split('\n').filter((l) => /^(ERROR|\*\*\*|Sys_Error|Com_Error)|segmentation|Assertion/i.test(l)).slice(0, 5);
  const bots = (log.match(/entered the game/g) || []).length;
  return { code: r.code, signal: r.signal, values: v, errors, bots, frameMs: Number(v.r_prof_frame_ms ?? NaN), draws: Number(v.r_prof_draws ?? v.r_draw_calls ?? NaN), entities: Number(v.cl_snap_entities ?? NaN) };
}

export async function run(args = {}) {
  const o = { pack: path.resolve(args.pack || path.join(OUT, 'pack', 'zzz-oax-enhanced.pk3')), bots: Number(args.bots || 4), frames: Number(args.frames || 600), budget: Number(args.budget || 0), maxDraws: Number(args['max-draws'] || 2500), gpu: !!args.gpu };
  if (!fs.existsSync(o.pack)) { console.error(`no pack at ${o.pack} (oacontent pack first)`); return 2; }
  const outDir = path.resolve(args.out || path.join(OUT, 'safety'));
  const maps = String(args.maps || DEFAULT_MAPS.join(',')).split(',');
  const failures = [], rows = [], A = {}, report = { maps: {} };
  for (const map of maps) {
    const a = await one('A', map, outDir, o);
    const b = await one('B', map, outDir, o);
    A[map] = a;
    const diff = compareIdentity(a.values, b.values);
    const f = [];
    if (!a.values.g_items_total) f.push('stock run produced no debug values (run failed)');
    if (diff.length) f.push(`identity: ${diff.join('; ')}`);
    for (const [n, r] of [['A', a], ['B', b]]) {
      if (r.code !== 0) f.push(`${n}: client exit ${r.code} ${r.signal || ''}`);
      if (r.errors.length) f.push(`${n}: errors in log: ${r.errors.join(' | ')}`);
      if (r.bots < 1) f.push(`${n}: no bot entered the game`);
      if (!(r.entities > 0)) f.push(`${n}: snapshot carried no entities`);
    }
    const ratioMs = b.frameMs / a.frameMs, ratioDraws = b.draws / a.draws;
    if (o.budget && ratioMs > o.budget) f.push(`budget: frame time x${ratioMs.toFixed(2)} (A ${a.frameMs} ms, B ${b.frameMs} ms)`);
    if (b.draws > o.maxDraws) f.push(`budget: ${b.draws} draw calls at the spawn view (limit ${o.maxDraws}; A ${a.draws})`);
    report.maps[map] = { A: { frameMs: a.frameMs, draws: a.draws, bots: a.bots, entities: a.entities }, B: { frameMs: b.frameMs, draws: b.draws, bots: b.bots, entities: b.entities }, ratioMs, ratioDraws, identityDiff: diff, failures: f };
    rows.push(`${map}: identity ${diff.length ? 'DIFFERS' : 'same'}, bots ${a.bots}/${b.bots}, frame ${a.frameMs} -> ${b.frameMs} ms (x${ratioMs.toFixed(2)}), draws ${a.draws} -> ${b.draws}${f.length ? '  FAIL: ' + f.join(' ; ') : ''}`);
    failures.push(...f.map((x) => `${map}: ${x}`));
  }
  // self-test: two different maps must differ
  if (maps.length >= 2) {
    const d = compareIdentity(A[maps[0]].values, A[maps[1]].values);
    rows.push(`self-test: ${maps[0]} vs ${maps[1]} differ in ${d.length} identity keys`);
    report.selfTest = d.length;
    if (!d.length) failures.push('self-test: the identity comparison reported no difference between two different maps (inconclusive)');
  } else failures.push('self-test needs at least two maps');
  fs.mkdirSync(outDir, { recursive: true });
  fs.writeFileSync(path.join(outDir, 'report.json'), JSON.stringify({ ...report, failures }, null, 1));
  fs.writeFileSync(path.join(outDir, 'report.txt'), rows.join('\n') + '\n' + failures.map((x) => 'FAIL ' + x).join('\n') + '\n');
  console.log(rows.join('\n'));
  console.log(failures.length ? `safety: ${failures.length} failures` : 'safety: ok');
  return failures.length ? 1 : 0;
}
