// T5 texture pipeline: normal and specular companions for the stock textures
// that need them, the upscale hook with an approval list, a size budget and a
// manifest of every derived file with its licence note.
//
//   oacontent textures [--baseoa dir] [--out dir] [--limit 400] [--budget-mb 60]
//                      [--only substr] [--strength 0.35] [--upscaler "cmd {in} {out}"]
// Output: <out>/textures/pack/textures/<name>_n.png and _s.png (the engine
// loads <diffuse>_n and <diffuse>_s next to the diffuse image of a lightmapped
// shader without being told; docs/materials.md), <out>/textures/manifest.json
// and report.txt.
//
// The generated maps are a floor, not art: the normal map comes from the
// diffuse's own luminance (a height guess, blurred a little, scaled to a
// target mean slope), the specular from luminance and the material class
// (T4's classification: metals shine, stone barely). Anything better is
// painted or authored and goes in by hand. Upscales are never automatic: only
// textures named in data/texture-approvals.json are upscaled, by the command
// given with --upscaler, written under the stock name and format so the
// overlay pack replaces them.
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { ContentSet, DEFAULT_BASEOA } from '../lib/packs.mjs';
import { Bsp } from '../lib/bsp.mjs';
import { loadShaders } from '../lib/shader.mjs';
import { decodeRgba, imageInfo, pngEncode } from '../lib/image.mjs';
import { OUT, REPO } from '../lib/common.mjs';

const COMPANION = /_(n|nh|s)$/i;
const EXTS = ['tga', 'jpg', 'png', 'jpeg'];

// the diffuse textures of the install that lightmapped surfaces of the maps use, with usage
export function candidates(cs, shaders) {
  const use = new Map();
  const add = (tex, map, cls) => { const e = use.get(tex) || use.set(tex, { maps: new Set(), cls }).get(tex); e.maps.add(map); };
  for (const m of cs.maps()) {
    let bsp;
    try { bsp = new Bsp(cs.read(m.path), m.path); } catch { continue; }
    for (const sh of bsp.shaders) {
      const name = sh.name.toLowerCase();
      const def = shaders.byName.get(name);
      if (def) {
        if (def.sky || def.nodraw || def.fog || def.liquid || def.deform.length) continue;
        // the diffuse: the first stage that is not the lightmap and not a blended effect
        const st = def.stageInfo.find((s) => !s.lightmap && !s.whiteimage && s.map.length && !(s.blend && s.blend[0] !== 'filter' && s.blend[0] !== 'gl_dst_color'));
        if (!st || st.map.length !== 1 || st.map[0].startsWith('$')) continue;
        if (!def.stageInfo.some((s) => s.lightmap) && !def.noLightmap) continue;
        add(st.map[0].replace(/\.(tga|jpg|jpeg|png)$/i, '').toLowerCase(), m.name);
      } else if (name.startsWith('textures/')) add(name, m.name);
    }
  }
  const out = [];
  for (const [tex, e] of use) {
    if (COMPANION.test(tex)) continue;
    const file = EXTS.map((x) => `${tex}.${x}`).find((p) => cs.has(p));
    if (!file) continue;
    out.push({ texture: tex, file, pack: cs.packOf(file), maps: e.maps.size });
  }
  return out.sort((a, b) => b.maps - a.maps || (a.texture < b.texture ? -1 : 1));
}

function luminance(img) {
  const n = img.width * img.height, l = new Float32Array(n);
  for (let i = 0; i < n; i++) l[i] = (0.2126 * img.data[i * 4] + 0.7152 * img.data[i * 4 + 1] + 0.0722 * img.data[i * 4 + 2]) / 255;
  return l;
}

function blur3(src, w, h) {
  const out = new Float32Array(src.length);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    let s = 0;
    for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) s += src[((y + dy + h) % h) * w + ((x + dx + w) % w)];
    out[y * w + x] = s / 9;
  }
  return out;
}

// a tileable normal map from a height field, scaled to a target mean slope
export function normalFromHeight(h, w, ht, target = 0.35) {
  const H = (x, y) => h[((y + ht) % ht) * w + ((x + w) % w)];
  let sum = 0;
  for (let y = 0; y < ht; y++) for (let x = 0; x < w; x++) sum += Math.hypot(H(x + 1, y) - H(x - 1, y), H(x, y + 1) - H(x, y - 1)) * 0.5;
  const mean = sum / (w * ht) || 1e-6;
  const k = Math.min(8, Math.max(0.8, target / mean));
  const out = Buffer.alloc(w * ht * 4);
  for (let y = 0; y < ht; y++) for (let x = 0; x < w; x++) {
    const dx = (H(x + 1, y) - H(x - 1, y)) * 0.5 * k, dy = (H(x, y + 1) - H(x, y - 1)) * 0.5 * k;
    const l = Math.hypot(dx, dy, 1), o = (y * w + x) * 4;
    out[o] = Math.round(((-dx / l) * 0.5 + 0.5) * 255); out[o + 1] = Math.round(((-dy / l) * 0.5 + 0.5) * 255); out[o + 2] = Math.round(((1 / l) * 0.5 + 0.5) * 255); out[o + 3] = 255;
  }
  return { data: out, k };
}

// specular colour (grey) and gloss (alpha) from luminance and the material class
export function specularFrom(lum, w, h, cls) {
  const p = { metal: { base: 0.35, gain: 0.5, gloss: 0.7 }, default: { base: 0.06, gain: 0.16, gloss: 0.3 } }[cls] || { base: 0.06, gain: 0.16, gloss: 0.3 };
  const out = Buffer.alloc(w * h * 4);
  for (let i = 0; i < w * h; i++) {
    const v = Math.min(1, p.base + p.gain * lum[i] * lum[i]);
    out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = Math.round(v * 255);
    out[i * 4 + 3] = Math.round(Math.min(1, p.gloss * (0.6 + 0.8 * lum[i])) * 255);
  }
  return out;
}

function approvals() {
  const f = path.join(REPO, 'misc', 'oacontent', 'data', 'texture-approvals.json');
  return fs.existsSync(f) ? JSON.parse(fs.readFileSync(f, 'utf8')) : { upscale: [] };
}

export async function run(args) {
  const baseoa = args.baseoa || DEFAULT_BASEOA;
  const out = path.join(args.out || OUT, 'textures');
  const packDir = path.join(out, 'pack');
  fs.rmSync(packDir, { recursive: true, force: true });
  fs.mkdirSync(packDir, { recursive: true });
  const cs = new ContentSet([baseoa]);
  const shaders = loadShaders(cs.shaderFiles());
  const matReport = path.join(args.out || OUT, 'materials', 'report.json');
  const metalShaders = new Set(fs.existsSync(matReport) ? JSON.parse(fs.readFileSync(matReport, 'utf8')).filter((r) => r.class === 'metal').map((r) => r.name.toLowerCase()) : []);
  let cands = candidates(cs, shaders);
  if (typeof args.only === 'string') cands = cands.filter((c) => c.texture.includes(args.only));
  const limit = Number(args.limit) || 400, budget = (Number(args['budget-mb']) || 60) * 1048576;
  const target = Number(args.strength) || 0.35;
  const manifest = [], skipped = { exists: 0, tooSmall: 0, budget: 0, unreadable: 0, limit: 0 };
  let bytes = 0, done = 0;
  for (const c of cands) {
    if (cs.has(`${c.texture}_n.tga`) || cs.has(`${c.texture}_n.png`) || cs.has(`${c.texture}_n.jpg`) || cs.has(`${c.texture}_nh.tga`) || cs.has(`${c.texture}_nh.png`)) { skipped.exists++; continue; }
    if (done >= limit) { skipped.limit++; continue; }
    const data = cs.read(c.file);
    const info = imageInfo(data, c.file);
    if (info.width < 32 || info.height < 32 || info.width > 2048) { skipped.tooSmall++; continue; }
    let img;
    try { img = decodeRgba(data, c.file); } catch { skipped.unreadable++; continue; }
    const lum = luminance(img);
    const height = blur3(lum, img.width, img.height);
    const n = normalFromHeight(height, img.width, img.height, target);
    const cls = [...metalShaders].some((s) => s.includes(c.texture)) ? 'metal' : 'default';
    const nPng = pngEncode(img.width, img.height, n.data, { alpha: false });
    const sPng = pngEncode(img.width, img.height, specularFrom(lum, img.width, img.height, cls), { alpha: true });
    if (bytes + nPng.length + sPng.length > budget) { skipped.budget++; continue; }
    for (const [suffix, buf] of [['_n', nPng], ['_s', sPng]]) {
      const f = path.join(packDir, `${c.texture}${suffix}.png`);
      fs.mkdirSync(path.dirname(f), { recursive: true });
      fs.writeFileSync(f, buf);
    }
    bytes += nPng.length + sPng.length;
    done++;
    const lic = 'derived from the stock texture; inherits its licence (see the source pack\'s own licence file)';
    manifest.push({ file: `${c.texture}_n.png`, kind: 'normal', source: c.file, sourcePack: c.pack, params: { method: 'luminance height, blur3, target slope', target, scale: +n.k.toFixed(2) }, maps: c.maps, licence: lic });
    manifest.push({ file: `${c.texture}_s.png`, kind: 'specular', source: c.file, sourcePack: c.pack, params: { class: cls }, maps: c.maps, licence: lic });
  }
  // upscales: only the approved ones, by the command given
  const ap = approvals();
  let upscaled = 0, pending = [];
  for (const name of ap.upscale || []) {
    const file = EXTS.map((x) => `${name}.${x}`).find((p) => cs.has(p));
    if (!file) { pending.push(`${name} (no such texture)`); continue; }
    if (!args.upscaler) { pending.push(`${name} (no --upscaler given)`); continue; }
    const tmp = fs.mkdtempSync(path.join(out, 'up-'));
    try {
      const src = path.join(tmp, `in.${file.split('.').pop()}`), dst = path.join(tmp, `out.${file.split('.').pop()}`);
      fs.writeFileSync(src, cs.read(file));
      const cmd = String(args.upscaler).replace('{in}', src).replace('{out}', dst);
      execFileSync('sh', ['-c', cmd], { stdio: 'inherit' });
      const dest = path.join(packDir, file);
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      fs.copyFileSync(dst, dest);
      manifest.push({ file, kind: 'upscale', source: file, sourcePack: cs.packOf(file), params: { command: args.upscaler }, licence: 'derived from the stock texture; inherits its licence; approved in data/texture-approvals.json' });
      upscaled++;
    } catch (e) { pending.push(`${name} (${e.message.split('\n')[0]})`); }
    finally { fs.rmSync(tmp, { recursive: true, force: true }); }
  }
  fs.writeFileSync(path.join(out, 'manifest.json'), JSON.stringify(manifest, null, 1) + '\n');
  const lines = [
    `textures: ${cands.length} candidate diffuse textures used by lightmapped surfaces of the stock maps`,
    `  generated ${done} normal + ${done} specular maps (${(bytes / 1048576).toFixed(1)} MB of PNG, budget ${(budget / 1048576).toFixed(0)} MB); ${upscaled} upscaled`,
    `  skipped: ${skipped.exists} already have a normal map, ${skipped.tooSmall} too small or too large, ${skipped.unreadable} unreadable, ${skipped.budget} over the size budget, ${skipped.limit} over --limit`,
    `  upscale approvals pending: ${pending.length}${pending.length ? ' (' + pending.slice(0, 5).join('; ') + ')' : ''}`,
    `  most used: ${cands.slice(0, 6).map((c) => `${c.texture} (${c.maps} maps)`).join(', ')}`,
  ];
  fs.writeFileSync(path.join(out, 'report.txt'), lines.join('\n') + '\n');
  console.log(lines.join('\n'));
  console.log(`written to ${out}`);
  return 0;
}
