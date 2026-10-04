#!/usr/bin/env node
// build.mjs: compile the engine's test maps.
//
//   node tests/maps/build.mjs [name ...]
//
// Each tests/maps/src/<name>.mjs exports `build()` returning
// { map: MapFile, light: 'lightmap'|'none', manifest: {...}, files: {path: text},
//   surfaces: SurfaceWorld, collision: CollisionMeshes, expectUnbound: n }.
// The map is written to tests/maps/out/baseoa/maps/<name>.map and compiled
// with q3map2 (-bsp -meta, -vis, -light unless light is 'none') and bspc
// (-bsp2aas), then an OAX_MANIFEST BSPX lump is added. `files` (shaders,
// scripts, guis) are written under tests/maps/out/baseoa. The out directory
// is packed into the cart (pack-cart --overlay) and copied into the native
// test home. Shader files written there are listed in its
// scripts/shaderlist.txt, so q3map2 sees them too.
//
// A surface-world map (docs/map-format.md) returns `surfaces` (a
// SurfaceWorld from misc/tools/oax-surfaces.mjs): after the last compile
// stage each surface is bound to the BSP leaves it passes through and the
// OAX_SURFACES lump is added; `collision` (CollisionMeshes) becomes
// OAX_COLLISION.
//
// The build is strict (it throws, so the process exits non-zero) on:
// q3map2 errors and leaks; brushes q3map2 dropped (it drops brushes past
// 65536 without failing); unknown materials (a brush shader or surface
// material that is neither a shader script nor an image in OpenArena's
// pk3s or this build's own files); empty meta output (no drawable surface
// and no surface world); surfaces that bind to no leaf (buried in the
// hull) unless the map expects them (`expectUnbound`).
//
// Tools: q3map2 (NetRadiant-custom's) and bspc (mbspc): the Q3MAP2 and BSPC
// env vars, else the names on PATH, else a sibling oa-mapgen checkout's
// vendor/bin. OpenArena content: OA_BASEOA (as the tests use).

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { addLump } from '../../misc/tools/bspx.mjs';
import { bakeTerrain } from '../../misc/tools/oax-terrain.mjs';
import { contentIndex } from './lib/content.mjs';
import { Patch } from './mapwriter.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..');
export const outDir = path.join(here, 'out');
const gameOut = path.join(outDir, 'baseoa');

function onPath(names) {
  for (const dir of (process.env.PATH || '').split(path.delimiter)) {
    for (const n of names) if (dir && fs.existsSync(path.join(dir, n))) return path.join(dir, n);
  }
  return null;
}

function tool(env, names) {
  const c = process.env[env] || onPath(names)
    || names.map((n) => path.join(repo, '..', 'oa-mapgen', 'vendor', 'bin', n)).find((p) => fs.existsSync(p));
  if (!c || !fs.existsSync(c)) throw new Error(`${names[0]} not found: put it on PATH or set ${env} (see docs/getting-started.md)`);
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

// q3map2 exits 0 on several failures, so read the log.
function checkQ3map2(stage, log, name) {
  if (/\*{6,} ERROR \*{6,}|MAP LEAKED|\*+ leaked \*+/.test(log)) throw new Error(`q3map2 ${stage} failed for ${name}:\n${log.slice(-3000)}`);
}

// standard lump counts of a compiled BSP
function bspCounts(file) {
  const b = fs.readFileSync(file);
  const lump = (i, size) => b.readInt32LE(12 + i * 8) / size;
  const shaders = [];
  const so = b.readInt32LE(8 + 1 * 8), sn = lump(1, 72);
  for (let i = 0; i < sn; i++) shaders.push(b.toString('latin1', so + i * 72, so + i * 72 + 64).replace(/\0.*$/s, ''));
  return { brushes: lump(8, 12), surfaces: lump(13, 104), shaders };
}

// brushes a .map source holds (patches are not brushes), and those q3map2
// never emits (areaportal, hint, skip and origin only)
const NOT_EMITTED = /^(common\/)?(areaportal|hint|skip|origin)$/i;
function sourceBrushes(map) {
  let all = 0, special = 0;
  for (const e of [map.world, ...map.entities]) {
    for (const br of e.brushes) {
      if (br instanceof Patch) continue;
      all++;
      if (br.faces.every((f) => NOT_EMITTED.test(f.texture))) special++;
    }
  }
  return { all, special };
}

const Q3MAP2_MAX_BRUSHES = 65536;
const builtIgnore = new Set(['noshader']);

export async function buildMap(name) {
  const src = path.join(here, 'src', `${name}.mjs`);
  const mod = await import(pathToFileURL(src).href);
  return buildSpec(name, await mod.build());
}

// compile one map spec (what a source's build() returns) as maps/<name>.bsp
export async function buildSpec(name, spec) {
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
  // compileFiles: written for q3map2 only, removed after the build
  for (const [rel, data] of Object.entries(spec.compileFiles || {})) {
    const p = path.join(gameOut, rel);
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, data);
  }
  const mapFile = path.join(mapsDir, `${name}.map`);
  fs.writeFileSync(mapFile, String(spec.map));

  const q3map2 = tool('Q3MAP2', ['q3map2']);
  const baseoa = findBaseoa();
  // our own shaders and textures (out/baseoa) are found through the homepath
  const fsArgs = ['-fs_basepath', path.dirname(baseoa), '-fs_homepath', outDir, '-fs_game', 'baseoa', '-game', 'quake3'];
  const log = [];
  const srcCount = typeof spec.map === 'object' && spec.map.world ? sourceBrushes(spec.map) : null;
  if (srcCount && srcCount.all > Q3MAP2_MAX_BRUSHES) throw new Error(`${name}: ${srcCount.all} brushes; q3map2 silently drops brushes past ${Q3MAP2_MAX_BRUSHES}`);
  let out = run(q3map2, [...fsArgs, '-meta', '-patchmeta', '-leaktest', ...(spec.bspArgs || []), mapFile], mapsDir);
  checkQ3map2('bsp', out, name);
  log.push(out);
  const bspFile = path.join(mapsDir, `${name}.bsp`);
  {
    const c = bspCounts(bspFile);
    if (srcCount && c.brushes < srcCount.all - srcCount.special) {
      throw new Error(`${name}: q3map2 dropped ${srcCount.all - srcCount.special - c.brushes} brushes (${srcCount.all - srcCount.special} in the source, ${c.brushes} in the BSP)`);
    }
    if (!c.surfaces && !spec.surfaces) throw new Error(`${name}: empty meta output: q3map2 produced no drawable surface and the map has no surface world\n${out.slice(-2000)}`);
  }
  // vis: 'fast' (-vis -fast) for open maps, where full vis buys nothing and its
  // portal flow can crash (FreeStackWinding: already free)
  out = run(q3map2, [...fsArgs, '-vis', ...(spec.vis === 'fast' ? ['-fast'] : []), mapFile], mapsDir);
  checkQ3map2('vis', out, name);
  log.push(out);
  if ((spec.light || 'lightmap') !== 'none') {
    out = run(q3map2, [...fsArgs, '-light', '-fast', '-patchshadows', '-samples', '2', ...(spec.lightArgs || []), mapFile], mapsDir);
    checkQ3map2('light', out, name);
    log.push(out);
  }
  if (spec.aas !== false) {
    // bspc truncates paths at 64 characters: run it from the maps directory
    out = run(tool('BSPC', ['mbspc', 'bspc']), ['-bsp2aas', `${name}.bsp`, '-forcesidesvisible', '-optimize', '-output', '.'], mapsDir);
    if (!fs.existsSync(path.join(mapsDir, `${name}.aas`))) throw new Error(`bspc wrote no ${name}.aas:\n${out.slice(-2000)}`);
    log.push(out);
  }
  const manifest = { oax: 1, map: name, ...(spec.manifest || {}) };
  addLump(path.join(mapsDir, `${name}.bsp`), 'OAX_MANIFEST', Buffer.from(JSON.stringify(manifest)));
  // misc_oax_terrain entities -> OAX_TERRAIN (heightmaps are written by the source through `files`)
  const terrains = bakeTerrain(path.join(mapsDir, `${name}.bsp`), String(spec.map), gameOut);
  if (terrains) log.push(`OAX_TERRAIN: ${terrains} terrain(s)`);
  for (const [lumpName, data] of Object.entries(spec.lumps || {})) addLump(path.join(mapsDir, `${name}.bsp`), lumpName, data);

  // surface world and collision meshes, bound to the compiled tree
  if (spec.surfaces) {
    const st = spec.surfaces.bindLeaves(fs.readFileSync(bspFile));
    const want = spec.expectUnbound || 0;
    if (st.unbound.length !== want) {
      throw new Error(`${name}: ${st.unbound.length} surface-world surfaces bind to no BSP leaf (buried in the hull; expected ${want}): ${st.unbound.slice(0, 20).join(', ')}`);
    }
    addLump(bspFile, 'OAX_SURFACES', spec.surfaces.toBuffer());
    log.push(`OAX_SURFACES: ${spec.surfaces.surfaces.length} surfaces, ${spec.surfaces.materials.length} materials, ${st.leafRefs} leaf refs, ${st.unbound.length} unbound`);
  }
  if (spec.collision) {
    addLump(bspFile, 'OAX_COLLISION', spec.collision.toBuffer());
    log.push(`OAX_COLLISION: ${spec.collision.meshes.length} meshes`);
  }

  // every material must exist (the engine would draw the default shader)
  const content = contentIndex(baseoa, gameOut, path.join(outDir, '.content-index.json'));
  const materials = new Set(bspCounts(bspFile).shaders.filter((n) => !builtIgnore.has(n.toLowerCase())));
  for (const m of spec.surfaces ? spec.surfaces.materials : []) materials.add(m);
  const unknown = [...materials].filter((m) => !content.has(m));
  if (unknown.length) throw new Error(`${name}: unknown materials (no shader script or image): ${unknown.join(', ')}`);
  fs.writeFileSync(path.join(mapsDir, `${name}.buildlog`), log.join('\n'));
  for (const junk of ['.prt', '.srf', '.lin']) fs.rmSync(path.join(mapsDir, name + junk), { force: true });
  for (const rel of Object.keys(spec.compileFiles || {})) fs.rmSync(path.join(gameOut, rel), { force: true });
  // packages: { name: 'x.pk3', files: {path: Buffer|string}, map: true } are
  // zipped into out/baseoa (the cart keeps an overlay's pk3s as pk3s, the
  // native test home copies them). A package with map: true carries the
  // compiled map, which is then not left loose (so the map loads from it).
  for (const pkg of spec.packages || []) {
    const stage = path.join(outDir, '.pkg', pkg.name);
    fs.rmSync(stage, { recursive: true, force: true });
    for (const [rel, data] of Object.entries(pkg.files || {})) {
      const p = path.join(stage, rel);
      fs.mkdirSync(path.dirname(p), { recursive: true });
      fs.writeFileSync(p, data);
    }
    if (pkg.map) {
      fs.mkdirSync(path.join(stage, 'maps'), { recursive: true });
      for (const ext of ['.bsp', '.aas']) {
        const loose = path.join(mapsDir, name + ext);
        if (fs.existsSync(loose)) fs.renameSync(loose, path.join(stage, 'maps', name + ext));
      }
    }
    const zipFile = path.join(gameOut, pkg.name);
    fs.rmSync(zipFile, { force: true });
    run('zip', ['-q', '-X', '-r', zipFile, '.'], stage);
    fs.rmSync(stage, { recursive: true, force: true });
    log.push(`package ${pkg.name}`);
  }
  return (spec.packages || []).some((p) => p.map) ? path.join(gameOut, spec.packages.find((p) => p.map).name) : path.join(mapsDir, `${name}.bsp`);
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
