import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { ZipReader, writeZip, crc32 } from '../lib/zip.mjs';
import { parseEntities, serializeEntities, entity } from '../lib/entities.mjs';
import { parseShaderFile, loadShaders } from '../lib/shader.mjs';
import { md3Info } from '../lib/md3.mjs';
import { imageInfo } from '../lib/image.mjs';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp } from '../lib/bsp.mjs';

const haveOA = fs.existsSync(DEFAULT_BASEOA);

test('zip: write then read, stable bytes', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oac-'));
  const f1 = path.join(dir, 'a.pk3'), f2 = path.join(dir, 'b.pk3');
  const files = [{ name: 'scripts/x.shader', data: Buffer.from('textures/a { }\n'.repeat(50)) }, { name: 'maps/m.oaxmap', data: Buffer.from('{ "classname" "worldspawn" }') }];
  writeZip(f1, files); writeZip(f2, [...files].reverse());
  assert.deepEqual(fs.readFileSync(f1), fs.readFileSync(f2));
  const z = new ZipReader(f1);
  assert.equal(z.read('MAPS/m.oaxmap').toString(), '{ "classname" "worldspawn" }');
  assert.equal(z.read('scripts/x.shader').length, 15 * 50);
  assert.equal(crc32(Buffer.from('123456789')), 0xcbf43926);
  fs.rmSync(dir, { recursive: true });
});

test('entities: parse, set, serialize round trip', () => {
  const ents = parseEntities('// c\n{\n"classname" "worldspawn"\n"message" "a b"\n}\n{ "classname" "light" "origin" "1 2 3" }');
  assert.equal(ents.length, 2);
  assert.equal(ents[0].get('message'), 'a b');
  assert.deepEqual(ents[1].origin, [1, 2, 3]);
  ents[0].set('oax_lighting', 'hybrid');
  const again = parseEntities(serializeEntities(ents));
  assert.equal(again[0].get('oax_lighting'), 'hybrid');
  assert.equal(entity('rtlight', { origin: [1, 2.5, 3] }).get('origin'), '1 2.5 3');
  assert.throws(() => parseEntities('{ "a" }'));
});

test('shader: derived facts', () => {
  const sh = parseShaderFile(`
textures/x/sky1
{
  surfaceparm sky
  q3map_sun 1 1 1 100 30 60
  skyparms env/x - -
}
textures/x/glow
{
  q3map_surfacelight 500
  {
    map textures/x/glow.tga
    blendfunc add
  }
}
textures/x/water
{
  surfaceparm water
  {
    map textures/x/w.tga
    tcMod scroll 0.1 0
  }
}`, 'a.shader');
  assert.equal(sh.length, 3);
  assert.ok(sh[0].sky && sh[0].sun[3] === 100);
  assert.equal(sh[1].surfaceLight, 500);
  assert.equal(sh[2].liquid, 'water');
  assert.deepEqual(sh[1].textures, ['textures/x/glow']);
});

test('shader: first definition in sorted file order wins', () => {
  const { byName } = loadShaders([{ name: 'b.shader', data: Buffer.from('t/a { cull none }') }, { name: '00_over.shader', data: Buffer.from('t/a { cull back }') }]);
  assert.equal(byName.get('t/a').cull, 'back');
});

test('image: tga and png headers', () => {
  const tga = Buffer.alloc(18 + 4); tga[2] = 2; tga.writeUInt16LE(2, 12); tga.writeUInt16LE(1, 14); tga[16] = 32;
  assert.deepEqual(imageInfo(tga, 'x.tga'), { format: 'tga', width: 2, height: 1, alpha: true, type: 2 });
});

test('md3: a stock model reads sensibly', { skip: !haveOA }, () => {
  const cs = new ContentSet();
  const f = 'models/weapons2/rocketl/rocketl.md3';
  if (!cs.has(f)) return;
  const i = md3Info(cs.read(f), f);
  assert.ok(i.vertices > 50 && i.triangles > 50 && i.surfaces >= 1);
});

test('bsp: a stock map reads, with sane geometry and an upward ray', { skip: !haveOA }, () => {
  const cs = new ContentSet();
  const m = cs.maps().find((x) => x.name === 'oa_dm1') || cs.maps()[0];
  const bsp = new Bsp(cs.read(m.path), m.path);
  assert.ok(bsp.surfaces.length > 100 && bsp.planes.length > 100 && bsp.entityText.includes('worldspawn'));
  const first = bsp.surfaces.find((s) => s.type === 1);
  assert.ok(bsp.surfaceTriangles(first).length >= 1);
  // the ray from the middle of the world box either hits something above or nothing
  const mn = bsp.models[0].mins, mx = bsp.models[0].maxs;
  const hit = bsp.upRay([(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, mn[2] + 1]);
  assert.ok(hit === null || typeof hit.z === 'number');
});

test('bsp: the spatial grid gives the same rays as testing every brush', { skip: !haveOA }, () => {
  const cs = new ContentSet();
  const m = cs.maps().find((x) => x.name === 'oa_dm6') || cs.maps()[0];
  const bsp = new Bsp(cs.read(m.path), m.path);
  const w = bsp.models[0];
  let seed = 12345;
  const rnd = () => (seed = (seed * 1664525 + 1013904223) >>> 0) / 4294967296;
  let hits = 0;
  for (let i = 0; i < 400; i++) {
    const p = [0, 1, 2].map((a) => w.mins[a] + rnd() * (w.maxs[a] - w.mins[a]));
    const az = rnd() * Math.PI * 2, el = (rnd() - 0.5) * 2;
    const d = [Math.cos(az) * Math.cos(el), Math.sin(az) * Math.cos(el), Math.sin(el)];
    bsp.useGrid = true; const a = bsp.ray(p, d, 2000);
    bsp.useGrid = false; const b = bsp.ray(p, d, 2000);
    assert.equal(!!a, !!b);
    if (a) { hits++; assert.ok(Math.abs(a.t - b.t) < 1e-6); }   // coincident brushes tie: which one is reported may differ
  }
  assert.ok(hits > 50);
});

import { mergeSidecars, validate } from '../tools/pack.mjs';
import { declNames } from '../tools/fxpreview.mjs';
import { compareIdentity } from '../tools/safety.mjs';

test('pack: sidecar parts merge into one worldspawn, keys later-wins, entities in order', () => {
  const a = '{\n"classname" "worldspawn"\n"oax_lighting" "hybrid"\n"x" "1"\n}\n{\n"classname" "rtlight"\n"origin" "1 2 3"\n}\n';
  const b = '{\n"classname" "worldspawn"\n"oax_smoothnormals" "45"\n"x" "2"\n}\n';
  const ents = parseEntities(mergeSidecars([a, b]));
  assert.equal(ents.length, 2);
  assert.equal(ents[0].classname, 'worldspawn');
  assert.equal(ents[0].get('oax_lighting'), 'hybrid');
  assert.equal(ents[0].get('oax_smoothnormals'), '45');
  assert.equal(ents[0].get('x'), '2');
  assert.equal(ents[1].classname, 'rtlight');
});

test('pack: validate rejects what it must (control) and accepts a clean set', { skip: !haveOA }, () => {
  const cs = new ContentSet([DEFAULT_BASEOA]);
  const f = (name, text) => ({ name, data: Buffer.from(text) });
  const good = [f('maps/oa_dm6.oaxmap', '{\n"classname" "worldspawn"\n}\n')];
  assert.deepEqual(validate(good, cs), []);
  const bad = validate([
    f('maps/nonexistent_map.oaxmap', '{\n"classname" "worldspawn"\n}\n'),
    f('maps/oa_dm6.oaxmap', '{\n"classname" "worldspawn"\n}\n{\n"classname" "worldspawn"\n}\n'),
    f('scripts/x.shader', 'textures/a/b\n{\n'),
    f('textures/nothing/here_n.png', 'x'),
    f('models/evil.md3', 'x'),
    f('maps/broken.oaxmap', '{ "a"'),
  ], cs);
  for (const re of [/no stock map nonexistent_map/, /more than one worldspawn/, /unbalanced braces/, /no diffuse/, /outside maps/, /broken/]) {
    assert.ok(bad.some((b) => re.test(b)), `expected a problem matching ${re}: ${bad.join(' | ')}`);
  }
});

test('fxpreview: decl names come from particle blocks only', () => {
  assert.deepEqual(declNames('// particle fake {\nparticle oax/a {\n}\n  particle oax/b{\n}\n'), ['oax/a', 'oax/b']);
});

test('safety: identity comparison reports a differing key and ignores the others', () => {
  const a = { sv_nav_polys: '10', sv_nav_hash: 'ab', g_items_total: '5', noise: '1' };
  assert.deepEqual(compareIdentity(a, { ...a, noise: '2' }), []);
  const d = compareIdentity(a, { ...a, sv_nav_hash: 'cd' });
  assert.equal(d.length, 1);
  assert.match(d[0], /sv_nav_hash/);
});

import { checkAgainstGolden } from '../tools/tour.mjs';
test('tour: the golden check passes a match and fails drift (control)', () => {
  const g = [{ map: 'a', frames: 16, mean: 50, draws: 100 }];
  const ok = { map: 'a', frames: 16, exit: 0, mean: 52, draws: 120 };
  assert.deepEqual(checkAgainstGolden(g, [ok]), []);
  assert.equal(checkAgainstGolden(g, [{ ...ok, mean: 30 }]).length, 1);
  assert.equal(checkAgainstGolden(g, [{ ...ok, draws: 500 }]).length, 1);
  assert.equal(checkAgainstGolden(g, [{ ...ok, frames: 3 }]).length, 1);
  assert.equal(checkAgainstGolden(g, []).length, 1);
});

import { FLAME, fireEntities } from '../tools/fires.mjs';
test('fires: shader names, clustering output, light cap', () => {
  for (const n of ['textures/sfx/flame2', 'textures/amph-dm01/bigflame_4', 'textures/sfx/r_flame-oasago', 'textures/sfx/xflame2']) assert.ok(FLAME.test(n), n);
  for (const n of ['textures/base/wall', 'textures/sfx/firewall', 'models/mapobjects/torch/torch']) assert.ok(!FLAME.test(n) || /torch/.test(n), n);
  const cl = Array.from({ length: 40 }, (_, i) => ({ lo: [i * 100, 0, 0], hi: [i * 100, 0, 50], c: [i * 100, 0, 25], size: 50, names: new Set(['textures/sfx/flame2']) }));
  const ents = fireEntities(cl, { maxLights: 12, shadows: 3 });
  assert.equal(ents.filter((e) => e.classname === 'func_oax_emitter').length, 40);
  const lights = ents.filter((e) => e.classname === 'rtlight');
  assert.equal(lights.length, 12);
  assert.equal(lights.filter((l) => !l.get('noshadows')).length, 3);
});

import { skyKeys } from '../tools/sky.mjs';
test('sky: keys by exposure class', () => {
  const indoor = skyKeys({ exposed: 0.02, sun: null }, 0);
  assert.deepEqual(Object.keys(indoor), ['oax_bloom']);
  const mixed = skyKeys({ exposed: 0.3, sun: { rgb: [1, 1, 1] } }, 0);
  assert.deepEqual(Object.keys(mixed), ['oax_bloom', 'oax_sundisc']);
  const out = skyKeys({ exposed: 0.9, sun: { rgb: [1, 1, 1] } }, -64);
  for (const k of ['oax_sundisc', 'oax_atmosphere', 'oax_clouds', 'oax_grade']) assert.ok(k in out, k);
  assert.ok(!('oax_clouds' in skyKeys({ exposed: 0.9, sun: null }, 0)));
});

import { halve } from '../tools/textures.mjs';
test('textures: halve averages 2x2 blocks', () => {
  const d = new Uint8Array(4 * 4 * 4);
  for (let i = 0; i < 16; i++) { d[i * 4] = (i % 4 < 2) ? 100 : 200; d[i * 4 + 1] = 0; d[i * 4 + 2] = 0; d[i * 4 + 3] = 255; }
  const h = halve(d, 4, 4);
  assert.equal(h.w, 2); assert.equal(h.h, 2);
  assert.deepEqual([h.data[0], h.data[4], h.data[8], h.data[12]], [100, 200, 100, 200]);
});

test('pack: companions under models/ and gfx/ are allowed, other files there are not', { skip: !haveOA }, () => {
  const cs = new ContentSet([DEFAULT_BASEOA]);
  const f = (name) => ({ name, data: Buffer.from('x') });
  const bad = validate([f('models/mapobjects/torch/torch_n.png'), f('gfx/fx/detail/d_stone_s.png'), f('models/players/x.md3'), f('gfx/evil.png')], cs);
  assert.ok(!bad.some((b) => /torch_n|d_stone_s/.test(b) && /outside/.test(b)), bad.join(' | '));
  assert.ok(bad.some((b) => /x\.md3.*outside/.test(b)));
  assert.ok(bad.some((b) => /evil\.png.*outside/.test(b)));
});
