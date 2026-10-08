// T6 fxpreview: renders every particle decl (the game's particles/*.prt plus
// any --prt files) on the oax_fx test map at fixed frames after the spawn,
// with the per-frame particle counts, and makes one contact sheet.
//
//   oacontent fxpreview [--decls a,b] [--prt file.prt,...] [--frames 4,14,40]
//                       [--size 480x270] [--out dir] [--gpu]
// Fails (exit 1) when none of a decl's frames differs from the empty room.
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { OUT, REPO } from '../lib/common.mjs';
import { findQvms, makeHome, renderEyes } from '../lib/render.mjs';
import { readTga } from '../../../tests/romdev/lib/tga.mjs';
import { writePng } from '../../../tests/romdev/lib/png.mjs';
import { diffFraction } from '../../../tests/romdev/lib/imgstat.mjs';
import { parseDebugValues } from '../../../tests/romdev/lib/values.mjs';

export const MAP = 'oax_fx';
export const VIEW = { eye: [320, -300, 90], angles: [4, 90, 0] };
export const SPAWN = [320, -100, 24];
// where the spawn lands on screen; the map's own emitters move between the
// control frame and the effect's frames, so only this box is compared
export const SPAWN_BOX = { x0: 0.3, y0: 0.35, x1: 0.7, y1: 0.95 };

export function declNames(text) {
  return [...text.matchAll(/^\s*particle\s+(\S+)\s*\{/gm)].map((m) => m[1]);
}

export function listDecls(extra = []) {
  const dir = path.join(findQvms(), 'particles');
  const files = fs.existsSync(dir) ? fs.readdirSync(dir).filter((f) => f.endsWith('.prt')).map((f) => path.join(dir, f)) : [];
  const out = [];
  for (const f of [...files, ...extra]) for (const n of declNames(fs.readFileSync(f, 'utf8'))) out.push({ name: n, file: f });
  return out;
}

export async function run(args = {}) {
  const frames = String(args.frames || '4,14,40').split(',').map(Number);
  const size = String(args.size || '480x270').split('x').map(Number);
  const outDir = path.resolve(args.out || path.join(OUT, 'fxpreview'));
  const extra = args.prt ? String(args.prt).split(',').map((f) => path.resolve(f)) : [];
  let decls = listDecls(extra);
  if (args.decls) { const want = new Set(String(args.decls).split(',')); decls = decls.filter((d) => want.has(d.name)); }
  if (!decls.length) { console.error('no particle decls found'); return 2; }
  const mapsDir = path.join(REPO, 'tests', 'maps', 'out', 'baseoa');
  const files = {};
  for (const f of extra) files[`particles/${path.basename(f)}`] = fs.readFileSync(f);
  const home = makeHome(path.join(outDir, 'home'), { files, packs: fs.existsSync(mapsDir) ? fs.readdirSync(mapsDir).filter((f) => f.endsWith('.pk3')).map((f) => path.join(mapsDir, f)) : [] });
  // the fx map ships as loose files in out/baseoa (maps, textures, particles)
  for (const d of ['maps', 'textures', 'scripts', 'particles']) if (fs.existsSync(path.join(mapsDir, d))) fs.cpSync(path.join(mapsDir, d), path.join(home, 'baseoa', d), { recursive: true });
  const eyes = [{ ...VIEW, cmd: '', settle: frames[0], decl: '(none)', frame: frames[0] }];
  for (const d of decls) {
    let prev = 0;
    frames.forEach((fr, i) => {
      eyes.push({ ...VIEW, cmd: i === 0 ? `oaxfx ${d.name} ${SPAWN.join(' ')} 0 0 1` : '', settle: fr - prev, decl: d.name, frame: fr });
      prev = fr;
    });
  }
  // the first eye of each decl waits frames[0]; the next ones the difference
  const r = await renderEyes({ map: MAP, eyes, home, size, settle: 4, gpu: !!args.gpu });
  const shots = path.join(home, 'baseoa', 'screenshots');
  const rows = [], bad = [], imgs = {};
  fs.mkdirSync(path.join(outDir, 'frames'), { recursive: true });
  eyes.forEach((e, i) => {
    const n = String(i).padStart(3, '0');
    const f = path.join(shots, `tour_${n}.tga`);
    if (!fs.existsSync(f)) { bad.push(`${e.decl} @${e.frame}: no frame`); return; }
    const png = path.join(outDir, 'frames', `${e.decl.replace(/\W+/g, '_')}_${e.frame}.png`);
    const img = readTga(f);
    imgs[`${e.decl}@${e.frame}`] = img;
    writePng(png, img);
    const vf = path.join(home, 'baseoa', `tourv_${n}.txt`);
    const v = fs.existsSync(vf) ? parseDebugValues(fs.readFileSync(vf, 'utf8')) : {};
    const drawn = Number(v.r_particles_drawn ?? NaN);
    rows.push({ decl: e.decl, frame: e.frame, drawn, png });
    if (e.decl !== '(none)') {
      const base = imgs[`(none)@${frames[0]}`];
      rows[rows.length - 1].changed = base ? diffFraction(base, img, 8, SPAWN_BOX) : 0;
    }
  });
  // an effect must show in at least one of its frames (a soft fade-in is faint at the first)
  for (const d of decls) {
    const mine = rows.filter((r) => r.decl === d.name);
    const best = Math.max(0, ...mine.map((r) => r.changed ?? 0));
    if (mine.length && best < 0.002) bad.push(`${d.name}: every frame is the empty-room frame (best ${(best * 100).toFixed(3)}% differ), nothing visible spawned`);
  }
  fs.writeFileSync(path.join(outDir, 'fxpreview.json'), JSON.stringify({ frames, rows, bad, exit: r.code }, null, 1));
  if (rows.length) {
    try {
      execFileSync('magick', ['montage', ...rows.map((x) => x.png), '-tile', `${frames.length}x`, '-geometry', '+2+2', '-background', '#222', path.join(outDir, 'sheet.png')]);
    } catch (e) { console.error('contact sheet failed: ' + e.message.split('\n')[0]); }
  }
  console.log(`fxpreview: ${decls.length} decls, ${rows.length} frames, ${bad.length} problems -> ${outDir}`);
  for (const b of bad) console.log('  ' + b);
  return bad.length ? 1 : 0;
}
