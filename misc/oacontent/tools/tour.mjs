// T2 tour: camera tours over the stock maps, rendered display-free, with
// metrics, contact sheets and before/after comparisons. The first thing it
// makes is the "before" set: every map on the unmodified engine.
//
//   oacontent tour [--maps a,b] [--tag baseline] [--eyes 48] [--size 640x360]
//                  [--jobs 2] [--sidecars dir] [--pack file.pk3] [--gpu] [--cvars a=1,b=2]
//       renders the tour of each map into <out>/tour/<tag>/<map>/ (eyes.json,
//       metrics.json, frames/*.png, sheet.png) and <out>/tour/<tag>/summary.*
//   oacontent tour compare --a baseline --b enhanced [--maps a,b]
//       per-eye differences between two tags: diff.json and a sheet of
//       [A | B | difference x4] per map.
//   oacontent tour cameras --maps a,b       prints the eyes without rendering
//
// --pack: adds an overlay pk3 (oacontent pack) to every run.
// --sidecars <dir>: copies <dir>/<map>.oaxmap into the run (lights tool output).
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp, MST, SURF } from '../lib/bsp.mjs';
import { loadShaders } from '../lib/shader.mjs';
import { parseEntities } from '../lib/entities.mjs';
import { OUT, REPO } from '../lib/common.mjs';
import { makeHome, renderEyes } from '../lib/render.mjs';
import { floorSamples } from './skyexposure.mjs';
import { readTga } from '../../../tests/romdev/lib/tga.mjs';
import { readPng, writePng } from '../../../tests/romdev/lib/png.mjs';
import { parseDebugValues } from '../../../tests/romdev/lib/values.mjs';

const SPAWN = /^(info_player_(deathmatch|start)|team_CTF_(red|blue)(spawn|player))$/;
const ITEM = /^(item_|weapon_|ammo_|holdable_|team_CTF_(red|blue)flag)/;
const EYE = 26;

// 12 horizontal rays: the yaw with the most open room, and how far it goes
function openestYaw(bsp, p) {
  let best = { yaw: 0, d: -1 };
  for (let k = 0; k < 12; k++) {
    const yaw = k * 30, r = (yaw * Math.PI) / 180;
    const h = bsp.ray(p, [Math.cos(r), Math.sin(r), 0], 1500, 1);
    const d = h ? h.t : 1500;
    if (d > best.d) best = { yaw, d };
  }
  return best;
}

export function cameras(bsp, shaders, { count = 48 } = {}) {
  const ents = parseEntities(bsp.entityText);
  const eyes = [];
  const spawns = ents.filter((e) => SPAWN.test(e.classname || '') && e.origin);
  const items = ents.filter((e) => ITEM.test(e.classname || '') && e.origin);
  const pickEvenly = (arr, n) => (arr.length <= n ? arr : Array.from({ length: n }, (_, i) => arr[Math.floor((i * arr.length) / n)]));
  for (const e of pickEvenly(spawns, Math.min(10, Math.floor(count / 4)))) {
    const o = e.origin;
    eyes.push({ kind: 'spawn', name: `${e.classname}@${o.map(Math.round).join(',')}`, eye: [o[0], o[1], o[2] + EYE], angles: [0, Number(e.get('angle') || 0), 0] });
  }
  for (const e of pickEvenly(items, Math.min(12, Math.floor(count / 3)))) {
    const o = e.origin;
    const p = [o[0], o[1], o[2] + 20];
    const { yaw } = openestYaw(bsp, p);
    const back = ((yaw + 180) * Math.PI) / 180;
    const eye = [o[0] + Math.cos(back) * 110, o[1] + Math.sin(back) * 110, o[2] + EYE + 10];
    const leaf = bsp.leafs[bsp.pointLeaf(eye)];
    const use = leaf && leaf.cluster >= 0 ? eye : [o[0], o[1], o[2] + EYE + 10];
    const d = [o[0] - use[0], o[1] - use[1], o[2] - use[2]];
    eyes.push({ kind: 'item', name: `${e.classname}@${o.map(Math.round).join(',')}`, eye: use, angles: [-Math.atan2(d[2], Math.hypot(d[0], d[1])) * 57.2958, Math.atan2(d[1], d[0]) * 57.2958, 0] });
  }
  // the rest: farthest-point sampling over the floors, looking down the longest open line
  const floors = floorSamples(bsp, shaders, 128, 4000).map((s) => [s.p[0], s.p[1], s.p[2] + EYE - 24]).filter((p) => { const l = bsp.leafs[bsp.pointLeaf(p)]; return l && l.cluster >= 0; });
  const chosen = eyes.map((e) => e.eye);
  const dist2 = (a, b) => (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2;
  const minD = floors.map((p) => (chosen.length ? Math.min(...chosen.map((c) => dist2(p, c))) : Infinity));
  while (eyes.length < count && floors.length) {
    let bi = 0; for (let i = 1; i < floors.length; i++) if (minD[i] > minD[bi]) bi = i;
    if (!(minD[bi] > 0)) break;
    const p = floors[bi];
    const { yaw } = openestYaw(bsp, p);
    eyes.push({ kind: 'grid', name: `grid@${p.map(Math.round).join(',')}`, eye: p, angles: [0, yaw, 0] });
    for (let i = 0; i < floors.length; i++) minD[i] = Math.min(minD[i], dist2(floors[i], p));
  }
  return eyes;
}

// ---- metrics ------------------------------------------------------------------
export function frameMetrics(img) {
  const n = img.width * img.height;
  const hist = new Uint32Array(256);
  const seen = new Set();
  let sum = 0, black = 0, white = 0;
  for (let i = 0; i < n; i++) {
    const r = img.data[i * 4], g = img.data[i * 4 + 1], b = img.data[i * 4 + 2];
    const l = Math.round(0.2126 * r + 0.7152 * g + 0.0722 * b);
    hist[l]++; sum += l;
    if (l < 4) black++; if (l >= 251) white++;
    seen.add(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
  }
  const pct = (q) => { let c = 0; for (let v = 0; v < 256; v++) { c += hist[v]; if (c >= q * n) return v; } return 255; };
  return { mean: +(sum / n).toFixed(2), p5: pct(0.05), p50: pct(0.5), p95: pct(0.95), black: +(black / n).toFixed(4), white: +(white / n).toFixed(4), colours: seen.size };
}

function sheet(files, out, { tile = 8, geometry = '240x135+1+1' } = {}) {
  if (!files.length) return;
  execFileSync('magick', ['montage', ...files, '-tile', `${tile}x`, '-geometry', geometry, '-background', 'black', out]);
}

async function pool(items, jobs, fn) {
  const results = new Array(items.length);
  let next = 0;
  await Promise.all(Array.from({ length: Math.min(jobs, items.length) }, async () => {
    while (next < items.length) { const i = next++; results[i] = await fn(items[i], i); }
  }));
  return results;
}

async function runTour(args) {
  const baseoa = args.baseoa || DEFAULT_BASEOA;
  const tag = args.tag || 'baseline';
  const root = path.join(args.out || OUT, 'tour', tag);
  fs.mkdirSync(root, { recursive: true });
  const cs = new ContentSet([baseoa]);
  const shaders = loadShaders(cs.shaderFiles());
  const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;
  const size = String(args.size || '640x360').split('x').map(Number);
  const count = Number(args.eyes) || 48;
  const cvars = Object.fromEntries(String(args.cvars || '').split(',').filter(Boolean).map((kv) => kv.split('=')));
  const maps = cs.maps().filter((m) => !only || only.has(m.name));
  const t0 = Date.now();
  const rows = await pool(maps, Number(args.jobs) || 2, async (m) => {
    const dir = path.join(root, m.name);
    fs.mkdirSync(path.join(dir, 'frames'), { recursive: true });
    try {
      const bsp = new Bsp(cs.read(m.path), m.path);
      bsp.skyNames = new Set([...shaders.byName.values()].filter((d) => d.sky).map((d) => d.name.toLowerCase()));
      const eyes = cameras(bsp, shaders, { count });
      fs.writeFileSync(path.join(dir, 'eyes.json'), JSON.stringify(eyes, null, 1) + '\n');
      const files = {};
      if (args.sidecars) {
        const sc = path.join(args.sidecars, `${m.name}.oaxmap`);
        if (fs.existsSync(sc)) files[`maps/${m.name}.oaxmap`] = fs.readFileSync(sc);
      }
      const home = makeHome(path.join(dir, 'home'), { files, packs: args.pack ? [path.resolve(String(args.pack))] : [] });
      const t1 = Date.now();
      const r = await renderEyes({ map: m.name, eyes, home, baseoa, size, cvars, gpu: !!args.gpu, timeoutMs: (Number(args.timeout) || 1200) * 1000 });
      const metrics = [];
      const pngs = [];
      for (let i = 0; i < eyes.length; i++) {
        const n = String(i).padStart(3, '0');
        const tga = path.join(home, 'baseoa', 'screenshots', `tour_${n}.tga`);
        if (!fs.existsSync(tga)) { metrics.push({ eye: i, missing: true }); continue; }
        const img = readTga(tga);
        const png = path.join(dir, 'frames', `${n}.png`);
        writePng(png, img);
        pngs.push(png);
        const vf = path.join(home, 'baseoa', `tourv_${n}.txt`);
        const dv = fs.existsSync(vf) ? parseDebugValues(fs.readFileSync(vf, 'utf8')) : {};
        metrics.push({ eye: i, name: eyes[i].name, kind: eyes[i].kind, ...frameMetrics(img), frameMs: Number(dv.r_prof_frame_ms ?? NaN), draws: Number(dv.r_prof_draws ?? 0), lights: Number(dv.r_ulights ?? 0), lightsVisible: Number(dv.r_ulights_visible ?? 0), shadowPasses: Number(dv.r_shadow_passes ?? 0), model: dv.r_ulight_model ?? null });
      }
      sheet(pngs, path.join(dir, 'sheet.png'));
      const ok = metrics.filter((x) => !x.missing);
      const avg = (k) => (ok.length ? +(ok.reduce((a, x) => a + (Number.isFinite(x[k]) ? x[k] : 0), 0) / ok.length).toFixed(3) : null);
      const log = fs.readFileSync(path.join(home, 'native.log'), 'utf8');
      const summary = {
        map: m.name, eyes: eyes.length, frames: ok.length, exit: r.code ?? r.signal, seconds: +((Date.now() - t1) / 1000).toFixed(1),
        mean: avg('mean'), p5: avg('p5'), p95: avg('p95'), black: avg('black'), white: avg('white'), colours: avg('colours'), frameMs: avg('frameMs'), draws: avg('draws'), lightsVisible: avg('lightsVisible'), model: ok[0]?.model ?? null,
        renderer: (/GL_RENDERER: (.*)/.exec(log) || [])[1] || null,
        errors: log.split('\n').filter((l) => /^\^1|ERROR|Sys_Error/.test(l)).slice(0, 3),
        sidecar: !!Object.keys(files).length,
      };
      fs.writeFileSync(path.join(dir, 'metrics.json'), JSON.stringify({ summary, eyes: metrics }, null, 1) + '\n');
      fs.rmSync(path.join(home, 'baseoa', 'screenshots'), { recursive: true, force: true });
      return summary;
    } catch (e) { return { map: m.name, error: e.message }; }
  });
  fs.writeFileSync(path.join(root, 'summary.json'), JSON.stringify(rows, null, 1) + '\n');
  const lines = [`tour ${tag}: ${rows.length} maps, ${((Date.now() - t0) / 1000).toFixed(0)} s`, 'map              eyes  mean   p5  p95  black white colours  ms   lights  secs  exit'];
  for (const r of rows) lines.push(r.error ? `${r.map.padEnd(16)} ERROR ${r.error}` : `${r.map.padEnd(16)} ${String(r.frames).padStart(3)}/${String(r.eyes).padEnd(3)} ${String(r.mean).padStart(5)} ${String(r.p5).padStart(3)} ${String(r.p95).padStart(4)} ${String(r.black).padStart(5)} ${String(r.white).padStart(5)} ${String(Math.round(r.colours)).padStart(7)} ${String(r.frameMs).padStart(5)} ${String(r.lightsVisible).padStart(6)} ${String(r.seconds).padStart(5)}  ${r.exit}`);
  fs.writeFileSync(path.join(root, 'summary.txt'), lines.join('\n') + '\n');
  console.log(lines.join('\n'));
  console.log(`written to ${root}`);
  return rows.some((r) => r.error || r.frames < r.eyes) ? 1 : 0;
}

async function runCompare(args) {
  const rootA = path.join(args.out || OUT, 'tour', args.a || 'baseline'), rootB = path.join(args.out || OUT, 'tour', args.b || 'enhanced');
  const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;
  const maps = fs.readdirSync(rootA).filter((d) => fs.existsSync(path.join(rootA, d, 'metrics.json')) && fs.existsSync(path.join(rootB, d, 'metrics.json')) && (!only || only.has(d)));
  const rows = [];
  for (const map of maps) {
    const A = JSON.parse(fs.readFileSync(path.join(rootA, map, 'metrics.json'), 'utf8')), B = JSON.parse(fs.readFileSync(path.join(rootB, map, 'metrics.json'), 'utf8'));
    const dir = path.join(args.out || OUT, 'tour', `${args.a || 'baseline'}-vs-${args.b || 'enhanced'}`, map);
    fs.mkdirSync(dir, { recursive: true });
    const eyes = [];
    const sheetFiles = [];
    for (let i = 0; i < Math.min(A.eyes.length, B.eyes.length); i++) {
      const n = String(i).padStart(3, '0');
      const fa = path.join(rootA, map, 'frames', `${n}.png`), fb = path.join(rootB, map, 'frames', `${n}.png`);
      if (!fs.existsSync(fa) || !fs.existsSync(fb)) continue;
      const ia = readPng(fa), ib = readPng(fb);
      if (ia.width !== ib.width || ia.height !== ib.height) continue;
      const diff = { width: ia.width, height: ia.height, data: Buffer.alloc(ia.data.length) };
      let sum = 0, over = 0;
      for (let p = 0; p < ia.data.length; p += 4) {
        let m = 0;
        for (let c = 0; c < 3; c++) { const d = Math.abs(ia.data[p + c] - ib.data[p + c]); if (d > m) m = d; diff.data[p + c] = Math.min(255, d * 4); }
        diff.data[p + 3] = 255; sum += m; if (m > 16) over++;
      }
      const dpng = path.join(dir, `diff_${n}.png`);
      writePng(dpng, diff);
      eyes.push({ eye: i, name: A.eyes[i].name, meanDiff: +(sum / (ia.width * ia.height)).toFixed(2), over16: +(over / (ia.width * ia.height)).toFixed(4), meanA: A.eyes[i].mean, meanB: B.eyes[i].mean });
      sheetFiles.push(fa, fb, dpng);
    }
    if (sheetFiles.length) execFileSync('magick', ['montage', ...sheetFiles, '-tile', '6x', '-geometry', '213x120+1+1', '-background', 'black', path.join(dir, 'sheet.png')]);
    const avg = (k) => (eyes.length ? +(eyes.reduce((a, x) => a + x[k], 0) / eyes.length).toFixed(3) : null);
    const row = { map, eyes: eyes.length, meanDiff: avg('meanDiff'), over16: avg('over16'), meanA: avg('meanA'), meanB: avg('meanB'), msA: A.summary.frameMs, msB: B.summary.frameMs, lightsB: B.summary.lightsVisible };
    fs.writeFileSync(path.join(dir, 'diff.json'), JSON.stringify({ summary: row, eyes }, null, 1) + '\n');
    rows.push(row);
  }
  const lines = [`compare ${args.a || 'baseline'} vs ${args.b || 'enhanced'}`, 'map              eyes  meanDiff  >16    meanA  meanB  msA   msB  lights'];
  for (const r of rows) lines.push(`${r.map.padEnd(16)} ${String(r.eyes).padStart(3)}  ${String(r.meanDiff).padStart(7)} ${String(r.over16).padStart(6)} ${String(r.meanA).padStart(6)} ${String(r.meanB).padStart(6)} ${String(r.msA).padStart(5)} ${String(r.msB).padStart(5)} ${String(r.lightsB).padStart(5)}`);
  const rootC = path.join(args.out || OUT, 'tour', `${args.a || 'baseline'}-vs-${args.b || 'enhanced'}`);
  fs.mkdirSync(rootC, { recursive: true });
  fs.writeFileSync(path.join(rootC, 'summary.txt'), lines.join('\n') + '\n');
  fs.writeFileSync(path.join(rootC, 'summary.json'), JSON.stringify(rows, null, 1) + '\n');
  console.log(lines.join('\n'));
  return 0;
}

export async function run(args) {
  const sub = args._[0];
  if (sub === 'compare') return runCompare(args);
  if (sub === 'cameras') {
    const cs = new ContentSet([args.baseoa || DEFAULT_BASEOA]);
    const shaders = loadShaders(cs.shaderFiles());
    const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;
    for (const m of cs.maps().filter((x) => !only || only.has(x.name))) {
      const bsp = new Bsp(cs.read(m.path), m.path);
      bsp.skyNames = new Set([...shaders.byName.values()].filter((d) => d.sky).map((d) => d.name.toLowerCase()));
      const eyes = cameras(bsp, shaders, { count: Number(args.eyes) || 48 });
      console.log(`${m.name}: ${eyes.length} eyes (${['spawn', 'item', 'grid'].map((k) => `${eyes.filter((e) => e.kind === k).length} ${k}`).join(', ')})`);
    }
    return 0;
  }
  return runTour(args);
}
