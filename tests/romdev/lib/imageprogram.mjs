// imageprogram.mjs: a JS port of the engine's image programs
// (code/renderergl2/tr_image_program.c, itself adapted from DOOM-3's
// Image_program.cpp), as the oracle the engine's output is compared with
// byte for byte. Every float operation is rounded to float32 the way the C
// code rounds it (Math.fround after each op; a double result rounded once
// to float32 equals the float32 op for + - * / and sqrt).

const f = Math.fround;
const f32buf = new Float32Array(1);
const i32buf = new Int32Array(f32buf.buffer);

// idMath::RSqrt
function rsqrt(x) {
  const y = f(x * 0.5);
  f32buf[0] = x;
  i32buf[0] = 0x5f3759df - (i32buf[0] >> 1);
  const r = f32buf[0];
  return f(r * f(1.5 - f(f(r * r) * y)));
}

function sqrLen(v) {
  return f(f(f(v[0] * v[0]) + f(v[1] * v[1])) + f(v[2] * v[2]));
}

function normalizeFast(v) {
  const inv = rsqrt(sqrLen(v));
  v[0] = f(v[0] * inv); v[1] = f(v[1] * inv); v[2] = f(v[2] * inv);
}

function normalize(v) {
  const s = sqrLen(v);
  if (s <= 0) return;
  const inv = f(1 / f(Math.sqrt(s)));
  v[0] = f(v[0] * inv); v[1] = f(v[1] * inv); v[2] = f(v[2] * inv);
}

const toByte = (x) => Math.trunc(x) & 255;

function heightmap(img, scaleArg) {
  const { width: w, height: h, data } = img;
  const scale = f(f(scaleArg) / 256);
  const depth = new Uint8Array(w * h);
  for (let i = 0; i < w * h; i++) depth[i] = Math.trunc((data[i * 4] + data[i * 4 + 1] + data[i * 4 + 2]) / 3);
  for (let i = 0; i < h; i++) {
    for (let j = 0; j < w; j++) {
      let d1 = depth[i * w + j];
      let d2 = depth[i * w + ((j + 1) & (w - 1))];
      let d3 = depth[((i + 1) & (h - 1)) * w + j];
      let a1 = d1, a3 = d3, a4 = depth[((i + 1) & (h - 1)) * w + ((j + 1) & (w - 1))];
      d2 -= d1;
      d3 -= d1;
      const dir = [f(-d2 * scale), f(-d3 * scale), 1];
      normalizeFast(dir);
      a1 -= a3;
      a4 -= a3;
      const dir2 = [f(-a4 * scale), f(a1 * scale), 1];
      normalizeFast(dir2);
      dir[0] = f(dir[0] + dir2[0]); dir[1] = f(dir[1] + dir2[1]); dir[2] = f(dir[2] + dir2[2]);
      normalizeFast(dir);
      const o = (i * w + j) * 4;
      data[o] = toByte(f(f(dir[0] * 127) + 128));
      data[o + 1] = toByte(f(f(dir[1] * 127) + 128));
      data[o + 2] = toByte(f(f(dir[2] * 127) + 128));
      data[o + 3] = 255;
    }
  }
  return img;
}

function dropsample(img, ow, oh) {
  const { width: iw, height: ih, data } = img;
  const out = Buffer.alloc(ow * oh * 4);
  for (let i = 0; i < oh; i++) {
    const row = 4 * iw * Math.trunc(((i + 0.25) * ih) / oh);
    for (let j = 0; j < ow; j++) {
      const k = Math.trunc((j * iw) / ow);
      data.copy(out, (i * ow + j) * 4, row + k * 4, row + k * 4 + 4);
    }
  }
  return { width: ow, height: oh, data: out };
}

function addNormals(a, b) {
  if (b.width !== a.width || b.height !== a.height) b = dropsample(b, a.width, a.height);
  for (let p = 0; p < a.width * a.height; p++) {
    const d1 = p * 4, d2 = p * 4;
    const n = [f((a.data[d1] - 128) / 127), f((a.data[d1 + 1] - 128) / 127), f((a.data[d1 + 2] - 128) / 127)];
    const s = sqrLen(n);
    const len = f(s * rsqrt(s));
    if (len < 1) {
      const t = f(f(1 - f(n[0] * n[0])) - f(n[1] * n[1]));
      n[2] = t > 0 ? f(Math.sqrt(t)) : 0;
    }
    n[0] = f(n[0] + (b.data[d2] - 128) / 127);
    n[1] = f(n[1] + (b.data[d2 + 1] - 128) / 127);
    normalize(n);
    a.data[d1] = toByte(f(f(n[0] * 127) + 128));
    a.data[d1 + 1] = toByte(f(f(n[1] * 127) + 128));
    a.data[d1 + 2] = toByte(f(f(n[2] * 127) + 128));
    a.data[d1 + 3] = 255;
  }
  return a;
}

function smoothNormals(img) {
  const { width: w, height: h, data } = img;
  const orig = Buffer.from(data);
  for (let i = 0; i < w; i++) {
    for (let j = 0; j < h; j++) {
      const n = [0, 0, 0];
      for (let k = -1; k < 2; k++) {
        for (let l = -1; l < 2; l++) {
          const o = (((j + l) & (h - 1)) * w + ((i + k) & (w - 1))) * 4;
          if (orig[o] === 0 && orig[o + 1] === 0 && orig[o + 2] === 0) continue;
          if (orig[o] === 128 && orig[o + 1] === 128 && orig[o + 2] === 128) continue;
          for (let c = 0; c < 3; c++) n[c] = f(n[c] + f(1 * (orig[o + c] - 128)));
        }
      }
      normalize(n);
      const o = (j * w + i) * 4;
      for (let c = 0; c < 3; c++) data[o + c] = toByte(f(128 + f(127 * n[c])));
    }
  }
  return img;
}

function addImages(a, b) {
  if (b.width !== a.width || b.height !== a.height) b = dropsample(b, a.width, a.height);
  for (let i = 0; i < a.width * a.height * 4; i++) a.data[i] = Math.min(255, a.data[i] + b.data[i]);
  return a;
}

function scaleImage(img, s) {
  const sc = s.map(f);
  for (let i = 0; i < img.width * img.height * 4; i++) {
    let v = f(img.data[i] * sc[i & 3]);
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    img.data[i] = Math.trunc(v);
  }
  return img;
}

function tokenize(s) {
  return s.match(/[(),]|[^\s(),]+/g) || [];
}

// run(program, images): images maps a path to {width, height, data RGBA}
export function runImageProgram(program, images) {
  const toks = tokenize(program);
  let p = 0;
  const next = () => toks[p++];
  const expect = (t) => { if (next() !== t) throw new Error(`image program: expected ${t}`); };
  const clone = (img) => ({ width: img.width, height: img.height, data: Buffer.from(img.data) });
  const parse = () => {
    const t = next();
    const lc = t.toLowerCase();
    if (lc === 'heightmap') { expect('('); const a = parse(); expect(','); const s = parseFloat(next()); expect(')'); return heightmap(a, s); }
    if (lc === 'addnormals' || lc === 'add') { expect('('); const a = parse(); expect(','); const b = parse(); expect(')'); return lc === 'add' ? addImages(a, b) : addNormals(a, b); }
    if (lc === 'smoothnormals') { expect('('); const a = parse(); expect(')'); return smoothNormals(a); }
    if (lc === 'scale') { expect('('); const a = parse(); const s = []; for (let i = 0; i < 4; i++) { expect(','); s.push(parseFloat(next())); } expect(')'); return scaleImage(a, s); }
    if (['invertalpha', 'invertcolor', 'makeintensity', 'makealpha'].includes(lc)) {
      expect('('); const a = parse(); expect(')');
      const d = a.data;
      for (let i = 0; i < a.width * a.height * 4; i += 4) {
        if (lc === 'invertalpha') d[i + 3] = 255 - d[i + 3];
        else if (lc === 'invertcolor') { d[i] = 255 - d[i]; d[i + 1] = 255 - d[i + 1]; d[i + 2] = 255 - d[i + 2]; }
        else if (lc === 'makeintensity') { d[i + 1] = d[i + 2] = d[i + 3] = d[i]; }
        else { d[i + 3] = Math.trunc((d[i] + d[i + 1] + d[i + 2]) / 3); d[i] = d[i + 1] = d[i + 2] = 255; }
      }
      return a;
    }
    if (!images[t]) throw new Error(`image program: no image ${t}`);
    return clone(images[t]);
  };
  return parse();
}

export function fnv1a(buf) {
  let h = 2166136261;
  for (let i = 0; i < buf.length; i++) {
    h ^= buf[i];
    h = Math.imul(h, 16777619) >>> 0;
  }
  return h >>> 0;
}

// the engine's r_imageprogram_result: "<w>x<h> <fnv1a hex>"
export function programResult(program, images) {
  const img = runImageProgram(program, images);
  return `${img.width}x${img.height} ${fnv1a(img.data).toString(16).padStart(8, '0')}`;
}
