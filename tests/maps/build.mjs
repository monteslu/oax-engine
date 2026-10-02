#!/usr/bin/env node
// build.mjs: compile the engine's test maps.
//
//   node tests/maps/build.mjs [name ...]
//
// Each tests/maps/src/<name>.mjs exports `build()` returning
// { map: MapFile, light: 'lightmap'|'none', manifest: {...}, files: {path: text} }.
// The map is written to tests/maps/out/baseoa/maps/<name>.map and compiled
// with q3map2 (-bsp -meta, -vis, -light unless light is 'none') and bspc
// (-bsp2aas), then an OAX_MANIFEST BSPX lump is added. `files` (shaders,
// scripts, guis) are written under tests/maps/out/baseoa. The out directory
// is packed into the cart (pack-cart --overlay) and copied into the native
// test home. Shader files written there are listed in its
// scripts/shaderlist.txt, so q3map2 sees them too.
//
// Tools: Q3MAP2 and BSPC env vars, else the sibling oa-mapgen checkout's
// vendor/bin. OpenArena content: OA_BASEOA (as the tests use).

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { addLump } from '../../misc/tools/bspx.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..');
export const outDir = path.join(here, 'out');
const gameOut = path.join(outDir, 'baseoa');

function tool(env, name) {
  const c = [process.env[env], path.join(repo, '..', 'oa-mapgen', 'vendor', 'bin', name)].find((p) => p && fs.existsSync(p));
  if (!c) throw new Error(`${name} not found; set ${env}`);
  return c;
}

function findBaseoa() {
  for (const c of [process.env.OA_BASEOA, '/usr/share/games/openarena/baseoa']) if (c && fs.existsSync(c)) return c;
  throw new Error('OpenArena baseoa not found; set OA_BASEOA');
}

function run(cmd, args, cwd) {
  try {
    return execFileSync(cmd, args, { cwd, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], maxBuffer: 64 << 20 });
  } catch (e) {
    throw new Error(`${path.basename(cmd)} failed:\n${String(e.stdout || '').slice(-3000)}${String(e.stderr || '').slice(-2000)}`);
  }
}

// q3map2 exits 0 on several failures; read the log (oa-mapgen PIPELINE.md).
function checkQ3map2(stage, log, name) {
  if (/\*{6,} ERROR \*{6,}|MAP LEAKED|\*+ leaked \*+/.test(log)) throw new Error(`q3map2 ${stage} failed for ${name}:\n${log.slice(-3000)}`);
  if (stage === 'bsp' && /\b0 total meta surfaces|inverted/i.test(log) && !/[1-9]\d* total meta surfaces/.test(log)) {
    throw new Error(`q3map2 bsp produced no surfaces for ${name}:\n${log.slice(-3000)}`);
  }
}

export async function buildMap(name) {
  const src = path.join(here, 'src', `${name}.mjs`);
  const mod = await import(pathToFileURL(src).href);
  const spec = await mod.build();
  const mapsDir = path.join(gameOut, 'maps');
  fs.mkdirSync(mapsDir, { recursive: true });
  for (const [rel, text] of Object.entries(spec.files || {})) {
    const p = path.join(gameOut, rel);
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, text);
  }
  // q3map2 reads only the shader files named in scripts/shaderlist.txt
  // (every copy on its search path): list the test maps' own shaders
  const scriptsDir = path.join(gameOut, "scripts");
  if (fs.existsSync(scriptsDir)) {
    const shaders = fs.readdirSync(scriptsDir).filter((f) => f.endsWith(".shader")).map((f) => f.slice(0, -7)).sort();
    fs.writeFileSync(path.join(scriptsDir, "shaderlist.txt"), shaders.join("\n") + "\n");
  }
  const mapFile = path.join(mapsDir, `${name}.map`);
  fs.writeFileSync(mapFile, String(spec.map));

  const q3map2 = tool('Q3MAP2', 'q3map2');
  const baseoa = findBaseoa();
  // our own shaders and textures (out/baseoa) are found through the homepath
  const fsArgs = ['-fs_basepath', path.dirname(baseoa), '-fs_homepath', outDir, '-fs_game', 'baseoa', '-game', 'quake3'];
  const log = [];
  let out = run(q3map2, [...fsArgs, '-meta', '-patchmeta', '-leaktest', ...(spec.bspArgs || []), mapFile], mapsDir);
  checkQ3map2('bsp', out, name);
  log.push(out);
  out = run(q3map2, [...fsArgs, '-vis', mapFile], mapsDir);
  checkQ3map2('vis', out, name);
  log.push(out);
  if ((spec.light || 'lightmap') !== 'none') {
    out = run(q3map2, [...fsArgs, '-light', '-fast', '-patchshadows', '-samples', '2', ...(spec.lightArgs || []), mapFile], mapsDir);
    checkQ3map2('light', out, name);
    log.push(out);
  }
  if (spec.aas !== false) {
    // bspc truncates paths at 64 characters: run it from the maps directory
    out = run(tool('BSPC', 'mbspc'), ['-bsp2aas', `${name}.bsp`, '-forcesidesvisible', '-optimize', '-output', '.'], mapsDir);
    if (!fs.existsSync(path.join(mapsDir, `${name}.aas`))) throw new Error(`bspc wrote no ${name}.aas:\n${out.slice(-2000)}`);
    log.push(out);
  }
  const manifest = { oax: 1, map: name, ...(spec.manifest || {}) };
  addLump(path.join(mapsDir, `${name}.bsp`), 'OAX_MANIFEST', Buffer.from(JSON.stringify(manifest)));
  fs.writeFileSync(path.join(mapsDir, `${name}.buildlog`), log.join('\n'));
  for (const junk of ['.prt', '.srf', '.lin']) fs.rmSync(path.join(mapsDir, name + junk), { force: true });
  return path.join(mapsDir, `${name}.bsp`);
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const names = process.argv.slice(2);
  const all = fs.readdirSync(path.join(here, 'src')).filter((f) => f.endsWith('.mjs')).map((f) => f.slice(0, -4));
  for (const n of names.length ? names : all) {
    const t = Date.now();
    const bsp = await buildMap(n);
    console.log(`${n}: ${path.relative(repo, bsp)} (${((Date.now() - t) / 1000).toFixed(1)}s)`);
  }
}
