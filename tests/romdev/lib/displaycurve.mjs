// displaycurve.mjs: the UE1 reference OpenGL renderer's display curve
// as a JS oracle for r_displayCurve (tr_oax_display.c). Shader brightness
// B = clamp(2 x brightness, 0.05, 2.99), gamma g = 1 / (1 + gammaOffset);
// on the max channel v: B > 1: x (v + (1 - (2v - 1)^2) / 4 (B - 1)) / v,
// clamped; B < 1: x B; then ^g.
export function displayCurve(rgb, { brightness = 1, gammaOffset = 0.1 } = {}) {
  const B = Math.min(Math.max(brightness * 2, 0.05), 2.99), g = 1 / (1 + gammaOffset);
  let c = rgb.map((x) => Math.min(1, Math.max(0, x)));
  const v = Math.max(...c);
  if (B > 1) { const w = Math.max(v, 0.001); const k = (w + (1 - (2 * w - 1) ** 2) * 0.25 * (B - 1)) / w; c = c.map((x) => Math.min(1, x * k)); }
  else if (B < 1) c = c.map((x) => x * B);
  return c.map((x) => x ** g);
}
