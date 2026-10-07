// T4 material classifier: rules over the shader scripts (names, parameters,
// stages) that decide which stock shaders get an oax material keyword, written
// as an overlay shader file that wins over the stock definitions.
//
//   oacontent materials [--baseoa dir] [--out dir] [--aggressive]
// Output: <out>/materials/00_oax_enhanced_materials.shader (the overlay: the
// stock shader text with the keywords inserted, under the same names; the
// engine reads shader files in name order and the first definition of a name
// wins, so "00_" sorts first) and report.{json,txt}: every shader of the
// install accounted for as classified, skipped (and why) or needing review.
//
// The rules (each reversible by leaving the file out):
//   sunsky   a sky shader with q3map_sun and no q3gl2_sun gets q3gl2_sun with
//            the same numbers (sun shadows), when every map that uses it is
//            outdoors or mixed (T10 exposure); a sky shared with indoor maps is
//            reported, not changed (the sun costs shadow passes indoors too);
//   water    surfaceparm water becomes oaxWater with a tint from the water
//            texture's colour (reflection, refraction, depth tint, shore foam);
//   metal    name rules for obvious metals (chrome, steel plate, grates) get
//            oaxMetal with a dark reflectance; low-confidence name matches only
//            with --aggressive;
//   noshadow nonsolid blended or alpha-tested effects (webs, sprites, fx
//            quads) are kept out of the shadow casters;
// and data/materials.json overrides any of it per shader.
import fs from 'node:fs';
import path from 'node:path';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp } from '../lib/bsp.mjs';
import { loadShaders } from '../lib/shader.mjs';
import { decodeRgba, meanColor } from '../lib/image.mjs';
import { OUT, REPO } from '../lib/common.mjs';
import { exposure } from './skyexposure.mjs';

const METAL_RULES = [
  { re: /(^|[\/_])(chrome|mirror)([\/_.]|$)/, args: [0.92, 0.92, 0.96, 0.12], confidence: 'high', why: 'chrome / mirror in the name' },
  { re: /(^|[\/_])(steel|iron)(plate|wall|floor|trim|grate|[\/_.0-9]|$)/, args: [0.6, 0.6, 0.64, 0.45], confidence: 'medium', why: 'steel / iron in the name' },
  { re: /(grate|grating|mesh)([\/_.0-9]|$)/, args: [0.5, 0.5, 0.53, 0.55], confidence: 'medium', why: 'a grate' },
  { re: /(metalplate|metalfloor|diamond|rivet|pipe)/, args: [0.45, 0.45, 0.48, 0.6], confidence: 'low', why: 'metal-ish name (plate, floor, diamond, rivets, pipe)' },
  { re: /(^|[\/_])(metal|metl|mtl)([\/_.0-9]|$)/, args: [0.45, 0.45, 0.48, 0.6], confidence: 'low', why: 'metal in the name' },
];

function overrides() {
  const f = path.join(REPO, 'misc', 'oacontent', 'data', 'materials.json');
  return fs.existsSync(f) ? JSON.parse(fs.readFileSync(f, 'utf8')).overrides || {} : {};
}

// the first '{' of the shader block (its own, not a stage's) and the text with
// `lines` inserted after it
export function insertKeywords(text, lines) {
  const at = text.indexOf('{');
  return text.slice(0, at + 1) + '\n' + lines.map((l) => `\t${l}`).join('\n') + text.slice(at + 1);
}

function textureColor(cs, def) {
  for (const t of def.textures) {
    const f = ['tga', 'jpg', 'png'].map((e) => `${t}.${e}`).find((p) => cs.has(p));
    if (!f) continue;
    try { return meanColor(decodeRgba(cs.read(f), f)); } catch { /* next */ }
  }
  return null;
}

// a stage that modulates what is behind it (the lightmap, a detail overlay) is
// not an effect: blendfunc filter, or GL_DST_COLOR GL_ZERO / GL_ZERO GL_SRC_COLOR
const isFilter = (b) => b[0] === 'filter' || (b[0] === 'gl_dst_color' && b[1] === 'gl_zero') || (b[0] === 'gl_zero' && b[1] === 'gl_src_color');
const effectStage = (s) => !s.lightmap && ((s.blend && !isFilter(s.blend)) || s.alphaFunc);

export function classify(def, ctx) {
  const ov = ctx.overrides[def.name] || ctx.overrides[def.name.toLowerCase()];
  if (ov) return { class: ov.class, args: ov.args, confidence: 'override', why: ov.why || 'data/materials.json' };
  if (def.oax.length) return { class: 'none', why: 'already has oax keywords' };
  if (def.sky) {
    if (!def.sun) return { class: 'none', why: 'sky without q3map_sun' };
    if (def.hasGl2Sun) return { class: 'none', why: 'sky already has q3gl2_sun' };
    const users = ctx.skyUsers.get(def.name.toLowerCase()) || [];
    if (!users.length) return { class: 'none', why: 'sky not used by a stock map' };
    const indoor = users.filter((u) => u.exposed < 0.15), outdoor = users.filter((u) => u.exposed >= 0.15);
    if (!outdoor.length) return { class: 'none', why: `sky used only by indoor maps (${users.map((u) => u.map).join(', ')})` };
    if (indoor.length) return { class: 'review', why: `sky shared with indoor maps (${indoor.map((u) => u.map).join(', ')}): a sun would cost shadow passes there`, confidence: 'medium' };
    return { class: 'sunsky', args: def.sun, confidence: 'high', why: `q3map_sun ${def.sun.join(' ')}; used by ${users.map((u) => u.map).join(', ')}` };
  }
  if (def.liquid === 'water' && !def.fog) {
    // surfaceparm water is also what mappers put on lava, acid and teleporter fx
    const odd = /(lava|acid|slime|tele|portal|plasma|hell|toxic|goo|blood|fire|#)/.exec(def.name.toLowerCase());
    if (odd) return { class: 'review', why: `surfaceparm water but "${odd[1]}" in the name: not clear water`, confidence: 'low' };
    const c = ctx.waterColor(def);
    if (c && c[0] > c[1] * 1.35 && c[0] > c[2] * 1.35) return { class: 'review', why: `surfaceparm water, but a red texture (${c.join(' ')}): lava or blood?`, confidence: 'low' };
    return { class: 'water', confidence: 'high', why: 'surfaceparm water' };
  }
  if (def.liquid) return { class: 'review', why: `${def.liquid} shader (no oax material for it yet)`, confidence: 'low' };
  if (def.parms.has('nonsolid') && !def.nodraw && def.stages.length && def.stageInfo.some(effectStage)) return { class: 'noshadow', confidence: 'medium', why: 'nonsolid blended or alpha-tested effect' };
  const name = def.name.toLowerCase();
  if (!def.parms.has('nonsolid') && !def.blended && def.stages.length) {
    for (const r of METAL_RULES) if (r.re.test(name)) return { class: 'metal', args: r.args, confidence: r.confidence, why: r.why };
  }
  if (def.nodraw) return { class: 'none', why: 'nodraw' };
  if (def.fog) return { class: 'none', why: 'fog volume' };
  if (!def.stages.length) return { class: 'none', why: 'no stages (a q3map-only shader)' };
  return { class: 'none', why: 'no rule' };
}

function keywordsFor(cls, def, ctx) {
  switch (cls.class) {
    case 'sunsky': return [`q3gl2_sun ${cls.args.join(' ')}`];
    case 'water': {
      const c = ctx.waterColor(def) || [0.1, 0.25, 0.3];
      const tint = c.map((v) => +(v * 0.45).toFixed(3));
      return ['oaxWater', `oaxWaterParm tint ${tint.join(' ')}`, 'oaxWaterParm density 0.012', 'oaxWaterParm reflectivity 0.8', 'oaxWaterParm fresnel 0.06', 'oaxWaterParm waves 0.3', 'oaxWaterParm foam 20 0.6'];
    }
    case 'metal': return [`oaxMetal ${cls.args.join(' ')}`];
    case 'noshadow': return ['oaxNoShadow'];
    default: return null;
  }
}

export function buildContext(cs, shaders) {
  // which maps use each shader, and how outdoors each is
  const skyUsers = new Map();
  const mapExposure = new Map();
  const skyNames = new Set([...shaders.byName.values()].filter((d) => d.sky).map((d) => d.name.toLowerCase()));
  const usage = new Map();
  for (const m of cs.maps()) {
    try {
      const bsp = new Bsp(cs.read(m.path), m.path);
      bsp.skyNames = skyNames;
      const names = new Set(bsp.shaders.map((s) => s.name.toLowerCase()));
      for (const n of names) { (usage.get(n) || usage.set(n, []).get(n)).push(m.name); }
      const ex = exposure(bsp, shaders, { spacing: 160, max: 1500 }).exposed;
      mapExposure.set(m.name, ex);
      for (const n of names) if (skyNames.has(n)) { (skyUsers.get(n) || skyUsers.set(n, []).get(n)).push({ map: m.name, exposed: ex }); }
    } catch { /* an unreadable map is not a user */ }
  }
  const colorCache = new Map();
  return { overrides: overrides(), skyUsers, usage, mapExposure, waterColor: (def) => { if (!colorCache.has(def.name)) colorCache.set(def.name, textureColor(cs, def)); return colorCache.get(def.name); } };
}

export async function run(args) {
  const baseoa = args.baseoa || DEFAULT_BASEOA;
  const out = path.join(args.out || OUT, 'materials');
  fs.mkdirSync(out, { recursive: true });
  const cs = new ContentSet([baseoa]);
  const shaders = loadShaders(cs.shaderFiles());
  const ctx = buildContext(cs, shaders);
  const rows = [];
  const blocks = [];
  for (const def of shaders.byName.values()) {
    const cls = classify(def, ctx);
    const aggressive = !!args.aggressive;
    const apply = ['sunsky', 'water', 'metal', 'noshadow'].includes(cls.class) && (cls.class !== 'metal' || cls.confidence !== 'low' || aggressive);
    const kws = apply ? keywordsFor(cls, def, ctx) : null;
    rows.push({ name: def.name, file: def.file, class: cls.class, applied: !!kws, confidence: cls.confidence || null, why: cls.why, maps: (ctx.usage.get(def.name.toLowerCase()) || []).length });
    if (kws) blocks.push(insertKeywords(def.text, kws));
  }
  const header = `// oacontent materials: oax keywords for stock shaders (generated, ${blocks.length} shaders)\n// The engine reads shader files in name order and the first definition of a\n// name wins: this file sorts first. Remove it for stock materials.\n\n`;
  const shaderFile = path.join(out, '00_oax_enhanced_materials.shader');
  fs.writeFileSync(shaderFile, header + blocks.join('\n\n') + '\n');
  fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(rows, null, 1) + '\n');
  const count = (f) => rows.filter(f).length;
  const lines = [
    `materials: ${rows.length} shaders in ${new Set(rows.map((r) => r.file)).size} files; ${blocks.length} get keywords`,
    `  classified and applied: ${count((r) => r.applied)} (sunsky ${count((r) => r.applied && r.class === 'sunsky')}, water ${count((r) => r.applied && r.class === 'water')}, metal ${count((r) => r.applied && r.class === 'metal')}, noshadow ${count((r) => r.applied && r.class === 'noshadow')})`,
    `  classified, not applied (low confidence metal; use --aggressive): ${count((r) => r.class === 'metal' && !r.applied)}`,
    `  needs review: ${count((r) => r.class === 'review')}`,
    `  skipped with a reason: ${count((r) => r.class === 'none')}`,
    '', 'skip reasons:',
    ...Object.entries(rows.filter((r) => r.class === 'none').reduce((a, r) => { const k = r.why.replace(/\(.*\)/, '(...)'); a[k] = (a[k] || 0) + 1; return a; }, {})).sort((a, b) => b[1] - a[1]).map(([k, v]) => `  ${String(v).padStart(5)}  ${k}`),
    '', 'needs review:',
    ...rows.filter((r) => r.class === 'review').map((r) => `  ${r.name}: ${r.why}`),
  ];
  fs.writeFileSync(path.join(out, 'report.txt'), lines.join('\n') + '\n');
  console.log(lines.join('\n'));
  console.log(`written to ${out}`);
  return 0;
}
