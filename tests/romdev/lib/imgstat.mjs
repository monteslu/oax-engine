// imgstat.mjs: measurements on decoded frames ({width, height, data RGBA})
// for render tests that check a property rather than a golden.

// box: {x0, y0, x1, y1} as fractions of the frame (default: all of it)
function pixelBox(img, box = {}) {
  const { x0 = 0, y0 = 0, x1 = 1, y1 = 1 } = box;
  return [Math.floor(x0 * img.width), Math.floor(y0 * img.height), Math.ceil(x1 * img.width), Math.ceil(y1 * img.height)];
}

export function meanRGB(img, box) {
  const [x0, y0, x1, y1] = pixelBox(img, box);
  const s = [0, 0, 0];
  let n = 0;
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const i = (y * img.width + x) * 4;
      s[0] += img.data[i]; s[1] += img.data[i + 1]; s[2] += img.data[i + 2]; n++;
    }
  }
  return s.map((v) => v / Math.max(n, 1));
}

export function meanLuma(img, box) {
  const [r, g, b] = meanRGB(img, box);
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// fraction of pixels in box where pred(r, g, b) holds
export function fraction(img, pred, box) {
  const [x0, y0, x1, y1] = pixelBox(img, box);
  let hit = 0, n = 0;
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const i = (y * img.width + x) * 4;
      if (pred(img.data[i], img.data[i + 1], img.data[i + 2])) hit++;
      n++;
    }
  }
  return hit / Math.max(n, 1);
}

// centroid (as fractions of the frame) and count of pixels where pred holds
export function centroid(img, pred) {
  let sx = 0, sy = 0, n = 0;
  for (let y = 0; y < img.height; y++) {
    for (let x = 0; x < img.width; x++) {
      const i = (y * img.width + x) * 4;
      if (pred(img.data[i], img.data[i + 1], img.data[i + 2])) { sx += x; sy += y; n++; }
    }
  }
  return n ? { x: (sx / n + 0.5) / img.width, y: (sy / n + 0.5) / img.height, n } : { x: NaN, y: NaN, n: 0 };
}

// fraction of pixels in box whose max channel difference exceeds tol
export function diffFraction(a, b, tol, box) {
  if (a.width !== b.width || a.height !== b.height) return 1;
  const [x0, y0, x1, y1] = pixelBox(a, box);
  let bad = 0, n = 0;
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const i = (y * a.width + x) * 4;
      const d = Math.max(Math.abs(a.data[i] - b.data[i]), Math.abs(a.data[i + 1] - b.data[i + 1]), Math.abs(a.data[i + 2] - b.data[i + 2]));
      if (d > tol) bad++;
      n++;
    }
  }
  return bad / Math.max(n, 1);
}

// mean max-channel difference in box
export function meanDiff(a, b, box) {
  if (a.width !== b.width || a.height !== b.height) return 255;
  const [x0, y0, x1, y1] = pixelBox(a, box);
  let s = 0, n = 0;
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const i = (y * a.width + x) * 4;
      s += Math.max(Math.abs(a.data[i] - b.data[i]), Math.abs(a.data[i + 1] - b.data[i + 1]), Math.abs(a.data[i + 2] - b.data[i + 2]));
      n++;
    }
  }
  return s / Math.max(n, 1);
}

// downscale by an integer factor (box filter), for small goldens
export function shrink(img, k) {
  const w = Math.floor(img.width / k), h = Math.floor(img.height / k);
  const out = Buffer.alloc(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      for (let c = 0; c < 4; c++) {
        let s = 0;
        for (let dy = 0; dy < k; dy++) for (let dx = 0; dx < k; dx++) s += img.data[((y * k + dy) * img.width + x * k + dx) * 4 + c];
        out[(y * w + x) * 4 + c] = Math.round(s / (k * k));
      }
    }
  }
  return { width: w, height: h, data: out };
}
