#!/usr/bin/env node
// pack-cart.mjs: assemble a wasmcart dev cart directory from a built cart
// wasm and OpenArena content.
//
//   node misc/wasmcart/pack-cart.mjs --wasm build-cart/Release/ioquake3.wasm \
//     --baseoa /usr/share/games/openarena/baseoa --qvm <dir containing vm/> \
//     --out build-cart/cart [--maps oa_dm1,oa_dm4] [--pk3 extra.pk3 ...]
//
// The cart reads loose files (the engine treats a game directory and its
// pk3s identically), so every pk3 is extracted in the engine's own load
// order: sorted by name, later ones overriding earlier ones. Paths are merged
// case-insensitively, as pk3 lookups are, keeping the last writer's case.
// QVMs from --qvm replace any in the pk3s (distro packages ship stubs).
// Extra --pk3 files are extracted last, in the order given. Each --overlay
// directory is copied over the game directory after that (test data).

import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

function parseArgs(argv) {
  const out = { pk3: [], overlay: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    const next = () => argv[++i];
    if (a === '--wasm') out.wasm = next();
    else if (a === '--baseoa') out.baseoa = next();
    else if (a === '--qvm') out.qvm = next();
    else if (a === '--out') out.out = next();
    else if (a === '--pk3') out.pk3.push(next());
    else if (a === '--overlay') out.overlay.push(next());
    else if (a === '--name') out.name = next();
    else throw new Error(`unknown argument ${a}`);
  }
  return out;
}

function findBaseoa() {
  const candidates = [
    process.env.OA_BASEOA,
    '/usr/share/games/openarena/baseoa',
    '/usr/lib/openarena/baseoa',
    path.join(os.homedir(), '.openarena', 'baseoa'),
  ].filter(Boolean);
  for (const c of candidates) {
    if (fs.existsSync(c) && fs.readdirSync(c).some((f) => f.toLowerCase().endsWith('.pk3'))) return c;
  }
  throw new Error('OpenArena baseoa not found; pass --baseoa or set OA_BASEOA');
}

// Recursively list files under dir as forward-slash relative paths.
function listFiles(dir, base = dir, out = []) {
  for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, ent.name);
    if (ent.isDirectory()) listFiles(p, base, out);
    else if (ent.isFile()) out.push(path.relative(base, p).split(path.sep).join('/'));
  }
  return out;
}

// Move every file from src into dst/baseoa, replacing any existing path that
// matches case-insensitively.
function overlay(srcDir, gameDir, index) {
  let n = 0;
  for (const rel of listFiles(srcDir)) {
    const key = rel.toLowerCase();
    const prior = index.get(key);
    if (prior && prior !== rel) fs.rmSync(path.join(gameDir, prior));
    const dest = path.join(gameDir, rel);
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.renameSync(path.join(srcDir, rel), dest);
    index.set(key, rel);
    n++;
  }
  return n;
}

function extractPk3(pk3, gameDir, index, stage) {
  fs.rmSync(stage, { recursive: true, force: true });
  fs.mkdirSync(stage, { recursive: true });
  execFileSync('unzip', ['-q', '-o', pk3, '-d', stage], { stdio: ['ignore', 'ignore', 'inherit'] });
  return overlay(stage, gameDir, index);
}

function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.wasm) throw new Error('--wasm is required');
  if (!args.qvm) throw new Error('--qvm is required (a directory containing vm/*.qvm)');
  const baseoa = args.baseoa || findBaseoa();
  const out = path.resolve(args.out || 'build-cart/cart');
  const gameDir = path.join(out, 'assets', 'baseoa');
  const stage = path.join(out, '.stage');

  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(gameDir, { recursive: true });

  const index = new Map();
  const pk3s = fs.readdirSync(baseoa).filter((f) => f.toLowerCase().endsWith('.pk3')).sort();
  for (const f of pk3s) {
    const n = extractPk3(path.join(baseoa, f), gameDir, index, stage);
    console.log(`${f}: ${n} files`);
  }
  for (const f of args.pk3) {
    const n = extractPk3(path.resolve(f), gameDir, index, stage);
    console.log(`${path.basename(f)}: ${n} files (extra)`);
  }

  const vmDir = path.join(args.qvm, 'vm');
  const qvms = fs.readdirSync(vmDir).filter((f) => f.endsWith('.qvm'));
  if (!qvms.length) throw new Error(`no .qvm files in ${vmDir}`);
  fs.rmSync(stage, { recursive: true, force: true });
  fs.mkdirSync(path.join(stage, 'vm'), { recursive: true });
  for (const q of qvms) fs.copyFileSync(path.join(vmDir, q), path.join(stage, 'vm', q));
  overlay(stage, gameDir, index);
  fs.rmSync(stage, { recursive: true, force: true });
  console.log(`qvm: ${qvms.join(' ')}`);

  for (const dir of args.overlay) {
    fs.rmSync(stage, { recursive: true, force: true });
    fs.cpSync(path.resolve(dir), stage, { recursive: true });
    const n = overlay(stage, gameDir, index);
    fs.rmSync(stage, { recursive: true, force: true });
    console.log(`${dir}: ${n} files (overlay)`);
  }

  // The cart's asset index: every bundled path, relative to assets/.
  const assetsDir = path.join(out, 'assets');
  const all = listFiles(assetsDir).filter((p) => p !== 'files.idx').sort();
  fs.writeFileSync(path.join(assetsDir, 'files.idx'), all.join('\n') + '\n');

  fs.copyFileSync(args.wasm, path.join(out, 'cart.wasm'));
  const manifest = {
    name: args.name || 'OpenArena',
    version: '0.1.0',
    abi: 3,
    entry: 'cart.wasm',
    players: 1,
    width: 1280,
    height: 720,
    controls: ['dpad', 'a', 'b', 'x', 'y', 'l', 'r', 'start', 'select',
      'left_stick', 'right_stick', 'left_trigger', 'right_trigger', 'l3', 'r3'],
  };
  fs.writeFileSync(path.join(out, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
  console.log(`cart: ${out} (${index.size} files)`);
}

main();
