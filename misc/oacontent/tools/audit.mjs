// T1 audit: the machine-readable inventory of an OpenArena install.
//   oacontent audit [--baseoa dir] [--out dir] [--colors] [--maps a,b]
// Writes <out>/audit/{maps,shaders,textures,models,summary}.json and
// summary.txt. Re-runnable in about a minute (--colors, which decodes every
// texture for its mean colour, takes a few minutes more).
import fs from 'node:fs';
import path from 'node:path';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp, SURF, MST } from '../lib/bsp.mjs';
import { loadShaders } from '../lib/shader.mjs';
import { parseEntities } from '../lib/entities.mjs';
import { imageInfo, decodeRgba, meanColor } from '../lib/image.mjs';
import { md3Info } from '../lib/md3.mjs';
import { OUT } from '../lib/common.mjs';

const ITEM = /^(item_|weapon_|ammo_|holdable_|team_CTF_(red|blue)flag|team_CTF_neutralflag)/;
const SPAWN = /^(info_player_|team_CTF_(red|blue)(spawn|player)|team_CTF_neutral)/;
const area = (t) => { const a = [t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]], b = [t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]]; return Math.hypot(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]) / 2; };

export function auditMap(cs, shaders, mapPath) {
  const buf = cs.read(mapPath);
  const bsp = new Bsp(buf, mapPath);
  const ents = parseEntities(bsp.entityText);
  const classes = {};
  for (const e of ents) classes[e.classname || '?'] = (classes[e.classname || '?'] || 0) + 1;
  const ws = ents[0] || null;
  const used = {}, kinds = { planar: 0, patch: 0, soup: 0, flare: 0 };
  let skyArea = 0, totalArea = 0, emitterArea = 0, upArea = 0;
  const emitters = {};
  const liquids = new Set(), skies = new Set();
  for (const s of bsp.surfaces) {
    const sh = bsp.shaders[s.shader];
    const name = sh.name.toLowerCase();
    used[name] = (used[name] || 0) + 1;
    if (s.type === MST.PLANAR) kinds.planar++; else if (s.type === MST.PATCH) kinds.patch++; else if (s.type === MST.TRIANGLE_SOUP) kinds.soup++; else if (s.type === MST.FLARE) kinds.flare++;
    if (s.type === MST.FLARE || s.type === MST.BAD) continue;
    const def = shaders.byName.get(name);
    let a = 0, nz = 0;
    for (const t of bsp.surfaceTriangles(s)) { const ar = area(t); a += ar; const ux = t[1][0] - t[0][0], uy = t[1][1] - t[0][1], vx = t[2][0] - t[0][0], vy = t[2][1] - t[0][1]; nz += (ux * vy - uy * vx) / 2; }
    totalArea += a;
    if (nz > 0.7 * a && s.type === MST.PLANAR) upArea += a;
    const isSky = (sh.surfaceFlags & SURF.SKY) || def?.sky;
    if (isSky) { skyArea += a; skies.add(name); }
    if (def?.liquid) liquids.add(`${name}:${def.liquid}`);
    if (def && def.surfaceLight > 0) { emitterArea += a; const e = emitters[name] ||= { surfaceLight: def.surfaceLight, surfaces: 0, area: 0 }; e.surfaces++; e.area += a; }
  }
  const mn = bsp.models[0].mins, mx = bsp.models[0].maxs;
  const unknown = Object.keys(used).filter((n) => !shaders.byName.has(n) && !cs.has(`${n}.tga`) && !cs.has(`${n}.jpg`) && !cs.has(`${n}.png`) && !n.startsWith('noshader') && !n.startsWith('flareshader'));
  return {
    name: path.basename(mapPath, '.bsp'), path: mapPath, pack: cs.packOf(mapPath), bytes: buf.length,
    bounds: { mins: mn.map(Math.round), maxs: mx.map(Math.round), size: [0, 1, 2].map((k) => Math.round(mx[k] - mn[k])) },
    worldspawn: ws ? Object.fromEntries(ws.keys) : {},
    entities: { total: ents.length, classes },
    lights: { entities: classes.light || 0, rtlights: classes.rtlight || 0, withKeepLights: ws?.get('_keepLights') ?? null },
    spawns: Object.entries(classes).filter(([k]) => SPAWN.test(k)).reduce((a, [, v]) => a + v, 0),
    items: Object.entries(classes).filter(([k]) => ITEM.test(k)).reduce((a, [, v]) => a + v, 0),
    surfaces: { total: bsp.surfaces.length, ...kinds, lightmaps: bsp.numLightmaps },
    area: { total: Math.round(totalArea), sky: Math.round(skyArea), upwardFloors: Math.round(upArea), emitters: Math.round(emitterArea) },
    sky: [...skies], liquids: [...liquids], emitters,
    clusters: bsp.lumps[16].len ? bsp.buf.readInt32LE(bsp.lumps[16].ofs) : 0,
    brushes: bsp.brushes.length, models: bsp.models.length,
    shadersUsed: Object.keys(used).length, shaderList: used,
    unresolvedShaders: unknown,
  };
}

export async function run(args) {
  const baseoa = args.baseoa || DEFAULT_BASEOA;
  const out = path.join(args.out || OUT, 'audit');
  fs.mkdirSync(out, { recursive: true });
  const t0 = Date.now();
  const cs = new ContentSet([baseoa]);
  const shaders = loadShaders(cs.shaderFiles());
  const only = typeof args.maps === 'string' ? new Set(args.maps.split(',')) : null;

  const maps = [];
  for (const m of cs.maps()) {
    if (only && !only.has(m.name)) continue;
    try { maps.push(auditMap(cs, shaders, m.path)); } catch (e) { maps.push({ name: m.name, path: m.path, error: e.message }); }
  }

  const shaderRows = shaders.all.filter((s) => s.name).map((s) => ({
    name: s.name, file: s.file, stages: s.stages.length, sky: s.sky, liquid: s.liquid, fog: s.fog, nodraw: s.nodraw, noLightmap: s.noLightmap,
    surfaceLight: s.surfaceLight, sun: s.sun, gl2Sun: s.hasGl2Sun, cull: s.cull, sort: s.sort, deform: s.deform.length, blended: s.blended,
    textures: s.textures, oaxKeywords: s.oax, tcmod: s.stageInfo.some((x) => x.tcmod.length), animated: s.stageInfo.some((x) => x.map.length > 1),
  }));

  const texFiles = cs.list(/^(textures|models|gfx|sprites|env)\/.*\.(tga|jpg|jpeg|png)$/);
  const textures = [];
  for (const f of texFiles) {
    const data = cs.read(f);
    const info = imageInfo(data, f);
    const row = { name: f, pack: cs.packOf(f), bytes: data.length, ...info };
    if (args.colors) { try { row.mean = meanColor(decodeRgba(data, f)); } catch { /* unreadable */ } }
    textures.push(row);
  }
  const texNames = new Set(texFiles.map((f) => f.replace(/\.[a-z]+$/i, '').toLowerCase()));
  const companions = textures.filter((t) => /(_n|_normal|_s|_spec|_specular|_gloss|_h|_height)\.[a-z]+$/i.test(t.name)).map((t) => t.name);

  const models = [];
  for (const f of cs.list(/^models\/.*\.md3$/)) {
    try { models.push({ name: f, pack: cs.packOf(f), bytes: cs.read(f).length, ...md3Info(cs.read(f), f) }); } catch (e) { models.push({ name: f, error: e.message }); }
  }

  const unusedTextures = [...texNames].filter((t) => t.startsWith('textures/')).length;
  const summary = {
    generated: new Date().toISOString(), baseoa, packs: cs.packs.map((p) => p.name),
    counts: { maps: maps.length, shaders: shaderRows.length, textures: textures.length, models: models.length, shaderFiles: cs.list(/^scripts\/.*\.shader$/).length },
    maps: {
      withoutLightEntities: maps.filter((m) => !m.error && m.lights.entities === 0 && m.lights.rtlights === 0).length,
      withSurfaceLightEmitters: maps.filter((m) => !m.error && Object.keys(m.emitters).length).length,
      withSky: maps.filter((m) => !m.error && m.sky.length).length,
      withLiquids: maps.filter((m) => !m.error && m.liquids.length).length,
      unresolvedShaderCount: maps.reduce((a, m) => a + (m.unresolvedShaders?.length || 0), 0),
    },
    shaders: { sky: shaderRows.filter((s) => s.sky).length, liquid: shaderRows.filter((s) => s.liquid).length, surfaceLight: shaderRows.filter((s) => s.surfaceLight > 0).length, withSun: shaderRows.filter((s) => s.sun).length, gl2Sun: shaderRows.filter((s) => s.gl2Sun).length, oaxKeywords: shaderRows.filter((s) => s.oaxKeywords.length).length },
    textures: { withNormalOrSpecular: companions.length, companions, texturesDirImages: unusedTextures, alpha: textures.filter((t) => t.alpha).length, over1024: textures.filter((t) => Math.max(t.width, t.height) > 1024).length },
    models: { vertices: models.reduce((a, m) => a + (m.vertices || 0), 0), triangles: models.reduce((a, m) => a + (m.triangles || 0), 0) },
    seconds: +((Date.now() - t0) / 1000).toFixed(1),
  };
  const w = (n, v) => fs.writeFileSync(path.join(out, n), JSON.stringify(v, null, 1) + '\n');
  w('maps.json', maps); w('shaders.json', shaderRows); w('textures.json', textures); w('models.json', models); w('summary.json', summary);
  const lines = [
    `audit of ${baseoa} (${summary.packs.length} packs) in ${summary.seconds} s`,
    `maps ${summary.counts.maps}: ${summary.maps.withoutLightEntities} with no light entities, ${summary.maps.withSurfaceLightEmitters} with surfacelight emitters, ${summary.maps.withSky} with sky, ${summary.maps.withLiquids} with liquids, ${summary.maps.unresolvedShaderCount} unresolved shader names`,
    `shaders ${summary.counts.shaders} in ${summary.counts.shaderFiles} files: ${summary.shaders.sky} sky, ${summary.shaders.liquid} liquid, ${summary.shaders.surfaceLight} surfacelight, ${summary.shaders.withSun} q3map_sun, ${summary.shaders.gl2Sun} q3gl2_sun, ${summary.shaders.oaxKeywords} with oax keywords`,
    `textures ${summary.counts.textures}: ${summary.textures.withNormalOrSpecular} normal/specular companions, ${summary.textures.alpha} with alpha, ${summary.textures.over1024} over 1024 px`,
    `models ${summary.counts.models}: ${summary.models.vertices} vertices, ${summary.models.triangles} triangles in all`,
  ];
  fs.writeFileSync(path.join(out, 'summary.txt'), lines.join('\n') + '\n');
  console.log(lines.join('\n'));
  console.log(`written to ${out}`);
  return 0;
}
