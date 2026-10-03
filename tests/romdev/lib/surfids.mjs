// surfids.mjs: read the engine's surface id dumps (r_oaxSurfaceIdDump,
// docs/test-hooks.md) and the r_surfid<i> debug values.

// the optional tail: [model] [variant=<surface-world shader variant>]
function tail(p) {
  const v = p.find((x) => x.startsWith('variant='));
  return { model: p.find((x) => !x.startsWith('variant=')), variant: v && v.slice(8) };
}

// "x y drawId kind index entity shaderIndex material [model] [variant=...]"
export function parseSurfIdValue(v) {
  const p = String(v || '').split(' ');
  return { x: +p[0], y: +p[1], id: +p[2], kind: p[3], index: +p[4], entity: +p[5], shaderIndex: +p[6], shader: p[7], ...tail(p.slice(8)) };
}

// { width, height, frame, surfs: Map(id -> {kind, index, entity, shaderIndex, shader, model}), ids: Uint32Array, truncated }
export function parseSurfIdDump(text) {
  const lines = String(text || '').split('\n');
  if (lines[0] !== 'oaxsurfids 1') throw new Error(`not a surface id dump: ${lines[0]}`);
  const out = { surfs: new Map(), truncated: false };
  for (const l of lines) {
    const p = l.split(' ');
    if (p[0] === 'size') { out.width = +p[1]; out.height = +p[2]; out.ids = new Uint32Array(out.width * out.height); }
    else if (p[0] === 'frame') out.frame = +p[1];
    else if (p[0] === 'surf') out.surfs.set(+p[1], { kind: p[2], index: +p[3], entity: +p[4], shaderIndex: +p[5], shader: p[6], ...tail(p.slice(7)) });
    else if (p[0] === 'row') {
      const y = +p[1];
      let x = 0;
      for (const run of p.slice(2)) {
        const [id, n] = run.split('*').map(Number);
        out.ids.fill(id, y * out.width + x, y * out.width + x + n);
        x += n;
      }
    } else if (p[0] === 'truncated') out.truncated = true;
  }
  return out;
}

// pixel mask (Uint8Array, 1 = in) of every pixel whose material name ends with `shader`
export function materialMask(dump, shader) {
  const ids = new Set([...dump.surfs].filter(([, s]) => s.shader === shader || s.shader.endsWith(`/${shader}`)).map(([id]) => id));
  const m = new Uint8Array(dump.ids.length);
  for (let i = 0; i < m.length; i++) m[i] = ids.has(dump.ids[i]) ? 1 : 0;
  return m;
}

// mean r g b, luminance and its standard deviation of img over mask (same size)
export function maskStats(img, mask) {
  let n = 0, r = 0, g = 0, b = 0, l2 = 0;
  for (let i = 0; i < mask.length; i++) {
    if (!mask[i]) continue;
    const o = i * 4;
    const lum = 0.299 * img.data[o] + 0.587 * img.data[o + 1] + 0.114 * img.data[o + 2];
    r += img.data[o]; g += img.data[o + 1]; b += img.data[o + 2]; l2 += lum * lum; n++;
  }
  if (!n) return { n: 0, r: 0, g: 0, b: 0, lum: 0, std: 0 };
  r /= n; g /= n; b /= n;
  const lum = 0.299 * r + 0.587 * g + 0.114 * b;
  return { n, r, g, b, lum, std: Math.sqrt(Math.max(0, l2 / n - lum * lum)) };
}
