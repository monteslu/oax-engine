// T8 packaging: builds the enhanced overlay pack from the other tools' output
// and validates it.
//
//   oacontent pack [--out dir] [--name zzz-oax-enhanced.pk3] [--maps a,b]
//                  [--skip lights,smooth,materials,textures] [--parts weather] [--baseoa dir] [--check file.pk3]
// Inputs (under <out>): lights/<map>.oaxmap, smooth/<map>.oaxmap,
// materials/00_oax_enhanced_materials.shader, textures/pack/** and
// textures/manifest.json, plus hand-written data/sidecars/<map>.oaxmap.
// The sidecar parts of one map are merged into a single maps/<map>.oaxmap
// (one worldspawn block, keys from every part; later parts win a clash; the
// other entities appended in order). The pack name sorts after the stock
// pk3s' maps and shaders do not matter: the overlay shader file name sorts
// first by itself.
// Output: <out>/pack/<name>, <out>/pack/manifest.json (every file, size,
// sha256, origin) and LICENSES-oax-enhanced.txt inside the pk3.
//
// Validation (exit 1 on any failure): every sidecar parses and has at most
// one worldspawn block; its map exists in the install; every shader file has
// balanced braces and its overlaid names exist in the stock shader set;
// every _n/_s file has a diffuse next to it in the stock content; no file
// outside maps/, scripts/, textures/ or the licence file (texture companions
// next to a model's or an effect's diffuse, models/ and gfx/ _n/_s, are allowed); total size.
// --check validates an existing pk3 without building.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { ZipReader, writeZip } from '../lib/zip.mjs';
import { loadShaders } from '../lib/shader.mjs';
import { parseEntities, serializeEntities } from '../lib/entities.mjs';
import { OUT } from '../lib/common.mjs';

const HERE = path.dirname(new URL(import.meta.url).pathname);
const PARTS = ['lights', 'smooth', 'fires', 'sky'];

// merge the parts' entity lists into one sidecar text
export function mergeSidecars(texts) {
  let world = null;
  const rest = [];
  for (const t of texts) {
    for (const e of parseEntities(t)) {
      if ((e.classname || '').toLowerCase() === 'worldspawn') {
        if (!world) world = e;
        else for (const [k, v] of e.keys) world.set(k, v);
      } else rest.push(e);
    }
  }
  return serializeEntities(world ? [world, ...rest] : rest);
}

const sha = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

function walk(dir, base = dir) {
  if (!fs.existsSync(dir)) return [];
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((d) => (d.isDirectory() ? walk(path.join(dir, d.name), base) : [path.relative(base, path.join(dir, d.name)).split(path.sep).join('/')]));
}

export function validate(files, content) {
  const bad = [];
  const names = new Set(files.map((f) => f.name));
  const stockMaps = new Set(content.list(/^maps\/[^/]+\.bsp$/i).map((n) => n.toLowerCase()));
  const stock = loadShaders(content.list(/^scripts\/[^/]+\.shader$/i).map((n) => ({ name: n, data: content.read(n) })));
  for (const f of files) {
    const n = f.name;
    if (n === 'LICENSES-oax-enhanced.txt') continue;
    if (!/^(maps|scripts|textures)\//.test(n) && !/^(models|gfx)\/.+_(n|nh|s)\.(png|tga|jpg)$/.test(n)) { bad.push(`${n}: outside maps/, scripts/, textures/`); continue; }
    if (/^maps\/.+\.oaxmap$/.test(n)) {
      const map = n.slice(5, -7);
      if (!stockMaps.has(`maps/${map}.bsp`.toLowerCase())) bad.push(`${n}: no stock map ${map}`);
      try {
        const ents = parseEntities(f.data.toString('latin1'));
        if (ents.filter((e) => (e.classname || '').toLowerCase() === 'worldspawn').length > 1) bad.push(`${n}: more than one worldspawn block`);
      } catch (e) { bad.push(`${n}: ${e.message}`); }
    } else if (/\.shader$/.test(n)) {
      const t = f.data.toString('latin1').replace(/\/\/[^\n]*/g, '');
      const open = (t.match(/\{/g) || []).length, close = (t.match(/\}/g) || []).length;
      if (open !== close) bad.push(`${n}: unbalanced braces (${open} open, ${close} close)`);
      for (const sh of loadShaders([{ name: n, data: f.data }]).all) {
        if (sh.name && !stock.byName.has(sh.name.toLowerCase())) bad.push(`${n}: overlays ${sh.name}, which no stock shader defines`);
      }
    } else if (/_(n|nh|s)\.(png|tga|jpg)$/.test(n)) {
      const stem = n.replace(/_(n|nh|s)\.(png|tga|jpg)$/, '');
      if (!['jpg', 'tga', 'png'].some((x) => content.has(`${stem}.${x}`) || names.has(`${stem}.${x}`))) bad.push(`${n}: no diffuse ${stem}.* in the stock content`);
    }
  }
  return bad;
}

export async function run(args = {}) {
  const out = path.resolve(args.out || OUT);
  const baseoa = args.baseoa || DEFAULT_BASEOA;
  const content = new ContentSet([baseoa]);
  if (args.check) {
    const z = new ZipReader(path.resolve(String(args.check)));
    const files = z.names().filter((n) => !n.endsWith('/')).map((n) => ({ name: n, data: z.read(n) }));
    const bad = validate(files, content);
    console.log(`pack check: ${files.length} files, ${bad.length} problems`);
    for (const b of bad) console.log('  ' + b);
    return bad.length ? 1 : 0;
  }
  const skip = new Set(String(args.skip || '').split(',').filter(Boolean));
  const only = args.maps ? new Set(String(args.maps).split(',')) : null;
  const files = [], origin = new Map();
  const add = (name, data, from) => { files.push({ name, data: Buffer.isBuffer(data) ? data : Buffer.from(data) }); origin.set(name, from); };

  // sidecars: per map, the parts merged
  const parts = new Map();
  for (const part of [...PARTS, ...String(args.parts || '').split(',').filter(Boolean)]) {
    if (skip.has(part)) continue;
    const dir = path.join(out, part);
    for (const f of fs.existsSync(dir) ? fs.readdirSync(dir) : []) {
      if (!f.endsWith('.oaxmap')) continue;
      const map = f.slice(0, -7);
      if (only && !only.has(map)) continue;
      if (!parts.has(map)) parts.set(map, []);
      parts.get(map).push({ part, text: fs.readFileSync(path.join(dir, f), 'latin1') });
    }
  }
  const hand = path.join(HERE, '..', 'data', 'sidecars');
  for (const f of fs.existsSync(hand) ? fs.readdirSync(hand) : []) {
    if (!f.endsWith('.oaxmap')) continue;
    const map = f.slice(0, -7);
    if (only && !only.has(map)) continue;
    if (!parts.has(map)) parts.set(map, []);
    parts.get(map).push({ part: 'hand', text: fs.readFileSync(path.join(hand, f), 'latin1') });
  }
  for (const [map, ps] of [...parts].sort()) add(`maps/${map}.oaxmap`, mergeSidecars(ps.map((p) => p.text)), ps.map((p) => p.part).join('+'));

  if (!skip.has('materials')) {
    const f = path.join(out, 'materials', '00_oax_enhanced_materials.shader');
    if (fs.existsSync(f)) add('scripts/00_oax_enhanced_materials.shader', fs.readFileSync(f), 'materials');
  }
  const man = path.join(out, 'textures', 'manifest.json');
  const texManifest = !skip.has('textures') && fs.existsSync(man) ? JSON.parse(fs.readFileSync(man, 'utf8')) : [];
  if (!skip.has('textures')) {
    const root = path.join(out, 'textures', 'pack');
    for (const rel of walk(root)) add(rel, fs.readFileSync(path.join(root, rel)), 'textures');
  }
  const licence = [
    'oax enhanced content overlay',
    '',
    'Generated by misc/oacontent from the OpenArena content (GPLv2 or later, with',
    'the per-asset licences of the OpenArena credits). Nothing here is original art:',
    'map sidecars and shader overlays are derived text; texture companions are',
    'derived from the stock texture of the same name and inherit its licence.',
    '',
    `files: ${files.length}`,
    ...texManifest.slice(0, 0),
    ...[...new Set(texManifest.map((t) => `${t.sourcePack}: ${t.licence}`))].map((l) => `  ${l}`),
    '',
    'The per-file list with sources is in manifest.json next to this pack.',
    '',
  ].join('\n');
  add('LICENSES-oax-enhanced.txt', licence, 'licence');

  const bad = validate(files, content);
  const name = String(args.name || 'zzz-oax-enhanced.pk3');
  const dir = path.join(out, 'pack');
  fs.mkdirSync(dir, { recursive: true });
  const pk3 = path.join(dir, name);
  writeZip(pk3, files);
  const total = files.reduce((a, f) => a + f.data.length, 0);
  const manifest = files.map((f) => ({ file: f.name, bytes: f.data.length, sha256: sha(f.data), origin: origin.get(f.name) }));
  fs.writeFileSync(path.join(dir, 'manifest.json'), JSON.stringify({ pack: name, files: manifest, bytes: total, problems: bad }, null, 1));
  const byKind = (re) => files.filter((f) => re.test(f.name)).length;
  console.log(`pack: ${files.length} files (${byKind(/\.oaxmap$/)} sidecars, ${byKind(/\.shader$/)} shader files, ${byKind(/^textures\//)} textures), ${(total / 1048576).toFixed(1)} MB raw, ${(fs.statSync(pk3).size / 1048576).toFixed(1)} MB packed -> ${pk3}`);
  for (const b of bad) console.log('  PROBLEM ' + b);
  return bad.length ? 1 : 0;
}
