// lightoracle.mjs: a JS oracle of the physical light description
// (docs/lights.md), written from the documented formulas, independent of the
// engine's C (tr_ulight_phys.c) and GLSL (interaction_fp.glsl ULIGHT_PHYSICAL).
//
//   x     = d / radius                     (0 at x >= 1)
//   v     = softcap(intensity * falloff(x))
//   light = min(v * angular * color, ceiling)   per channel
//
// profile(keys) translates an entity's keys the way the engine documents;
// light(desc, d, cosine) evaluates one light at a surface point.

// falloff 'smooth': 1 - smoothstep(d / R), no ceiling (fitted in linear space);
// 'line' with lineZero and capK is the profile fitted in the reference renderer's display space
// before (kept for controls)
export const UE1 = { falloff: 'smooth', gain: 0.0128, floor: 0.010, lineZero: 0.89, capK: 115 };
const Q3 = { pointScale: 7500, linearScale: 1 / 8000, minDist: 16 };

export function ue1Level(brightness) {
  let v = brightness * 1.4 / 255;
  if (v <= 0) return 0;
  v *= 0.7 / (0.01 + Math.sqrt(v));
  return Math.min(1, Math.max(0, v));
}

export function ue1Color(h, saturation) {
  let base;
  if (h < 86) base = [(85 - h) / 85, h / 85, 0];
  else if (h < 171) base = [0, (170 - h) / 85, (h - 85) / 85];
  else base = [(h - 170) / 85, 0, (255 - h) / 84];
  const s = saturation / 255;
  return base.map((c) => c + s * (1 - c));
}

const line = (zero) => ({ mode: 'table', points: [[0, 1], [zero, 0]] });
const quadratic = () => ({ mode: 'table', points: Array.from({ length: 16 }, (_, i) => { const x = i / 15; return [x, (1 - x) ** 2]; }) });

const num = (v, d) => (v === undefined || v === '' ? d : Number(v));
const vec = (v) => String(v).trim().split(/\s+/).map(Number);

const LT = ['LT_None', 'LT_Steady', 'LT_Pulse', 'LT_Blink', 'LT_Flicker', 'LT_Strobe', 'LT_BackdropLight', 'LT_SubtlePulse', 'LT_TexturePaletteOnce', 'LT_TexturePaletteLoop'];
const LE = ['LE_None', 'LE_TorchWaver', 'LE_FireWaver', 'LE_WateryShimmer', 'LE_Searchlight', 'LE_SlowWave', 'LE_FastWave', 'LE_CloudCast', 'LE_StaticSpot', 'LE_Shock', 'LE_Disco', 'LE_Warp', 'LE_Spotlight', 'LE_NonIncidence'];
const enumOf = (v, names, d) => (v === undefined ? d : /^-?\d/.test(String(v)) ? Number(v) : Math.max(0, names.findIndex((n) => n.toLowerCase() === String(v).toLowerCase())));

// keys: the light entity's keys (strings or numbers); world: worldspawn keys
export function profile(keys, world = {}) {
  const k = Object.fromEntries(Object.entries(keys).map(([a, b]) => [a.toLowerCase(), b]));
  let prof = k.oax_profile;
  if (!prof) prof = Object.keys(k).some((n) => n.startsWith('ue1_')) ? 'ue1' : 'physical';
  const color0 = k._color !== undefined ? vec(k._color) : [1, 1, 1];
  const lightRadius = k.light_radius !== undefined ? Math.max(...vec(k.light_radius)) : num(k.light, 300);
  const d = { profile: prof, intensity: 1, color: [1, 1, 1], cap: 0, knee: 0, lambert: true, mask: 1, effect: null, on: true, falloff: line(1), radius: lightRadius };

  if (prof === 'ue1') {
    const B = num(k.ue1_lightbrightness, 64), r = num(k.ue1_lightradius, 64);
    const type = enumOf(k.ue1_lighttype, LT, 1), eff = enumOf(k.ue1_lighteffect, LE, 0);
    const period = num(k.ue1_lightperiod, 32), phase = num(k.ue1_lightphase, 0);
    d.radius = 25 * (Math.trunc(r) + 1);
    d.intensity = UE1.gain * B;
    d.floor = UE1.floor ?? 0;   // taken off this lamp's own light (r_ulightUE1Floor)
    if (UE1.falloff === 'line') {
      d.falloff = line(UE1.lineZero);
      d.cap = UE1.capK * UE1.gain * ue1Level(B);
    } else {
      d.falloff = { mode: 'smooth' };
      d.cap = 0;
    }
    const lb = Number(world.ue1_LevelBrightness ?? 1);
    if (lb !== 1) { d.intensity *= lb; d.cap *= lb; }
    d.color = ue1Color(num(k.ue1_lighthue, 0), num(k.ue1_lightsaturation, 255));
    d.mask = /^(true|1)$/i.test(String(k.ue1_bspeciallit ?? '')) ? 2 : 1;
    d.on = !(B <= 0 || type === 0);
    if (type === 2 || type === 7) d.effect = { table: 'oax_sin', rate: 35 / Math.max(period, 1), phase: phase / 256, base: type === 2 ? 0.6 : 0.9, amp: type === 2 ? 0.39 : 0.09 };
    if (type === 3) d.effect = { table: 'oax_blink', rate: 35 / (period + 1), phase: phase / 256, base: 0, amp: 1 };
    if (eff === 13) d.lambert = false;
  } else if (prof === 'q3') {
    const I = num(k._light, 0) || num(k.light, 0) || 300;
    const photons = I * (num(k._scale, 1) || 1) * Q3.pointScale;
    const sf = num(k.spawnflags, 0);
    d.lambert = !(sf & 2);
    if (sf & 1) {
      const fade = num(k.fade, 1) || 1;
      d.radius = Math.max(photons * Q3.linearScale / fade, 1);
      d.intensity = photons * Q3.linearScale / 255;
      const m = Math.min(Q3.minDist / d.radius, 1);
      d.falloff = { mode: 'table', points: [[0, 1 - m], [m, 1 - m], [1, 0]] };
      d.lambert = false;
    } else {
      d.radius = Math.max(Math.sqrt(photons), Q3.minDist * 2);
      d.falloff = { mode: 'invsq', min: Q3.minDist / d.radius };
      d.intensity = photons / (Q3.minDist * Q3.minDist) / 255;
    }
    const mx = Math.max(...color0);
    d.color = [1, 1, 1].map((c) => (mx > 0 ? c / mx : c));
  } else if (prof === 'doom3') {
    d.falloff = quadratic();
  }

  // overrides
  if (k.oax_radius !== undefined) d.radius = Number(k.oax_radius);
  if (k.oax_falloff !== undefined) {
    const t = String(k.oax_falloff).trim().split(/\s+/);
    if (t[0] === 'linear') d.falloff = line(1);
    else if (t[0] === 'quadratic') d.falloff = quadratic();
    else if (t[0] === 'smooth') d.falloff = { mode: 'smooth' };
    else if (t[0] === 'invsq') d.falloff = { mode: 'invsq', min: Number(t[1]) / d.radius };
    else if (t[0] === 'table') {
      const pts = [];
      for (let i = 1; i + 1 < t.length && pts.length < 16; i += 2) pts.push([Number(t[i]), Number(t[i + 1])]);
      d.falloff = { mode: 'table', points: pts };
    } else if (t[0] === 'image') d.falloff = { mode: 'image', path: t[1] };
  }
  if (k.oax_intensity !== undefined) d.intensity = Number(k.oax_intensity);
  if (k.oax_color !== undefined) d.color = vec(k.oax_color);
  if (k.oax_cap !== undefined) d.cap = Number(k.oax_cap);
  if (k.oax_capknee !== undefined) d.knee = Number(k.oax_capknee);
  if (k.oax_angular !== undefined) d.lambert = String(k.oax_angular).toLowerCase() !== 'none';
  if (k.oax_mask !== undefined) d.mask = Number(k.oax_mask) & 0xffff;
  if (k.oax_effect !== undefined) {
    const t = String(k.oax_effect).trim().split(/\s+/);
    d.effect = t[0] === 'none' ? null : { table: t[0], rate: num(t[1], 1), phase: num(t[2], 0), base: num(t[3], 0), amp: num(t[4], 1) };
  }
  // _color (shaderParm 0-2) multiplies; q3 already holds 1 / max(_color)
  d.color = d.color.map((c, i) => c * color0[i]);
  return d;
}

// the falloff curve at x = d / radius (image curves need `sample(x)`)
export function falloff(f, x, sample) {
  if (f.mode === 'invsq') { const r = Math.max(x, f.min); return (f.min * f.min) / (r * r); }
  if (f.mode === 'image') return sample(x);
  if (f.mode === 'smooth') { const t = Math.min(1, Math.max(0, x)); return 1 - t * t * (3 - 2 * t); }
  if (f.mode === 'cos') { const t = Math.min(1, Math.max(0, x)); return 0.5 + 0.5 * Math.cos(Math.PI * t); }
  const p = f.points;
  if (x <= p[0][0]) return p[0][1];
  for (let i = 1; i < p.length; i++) {
    if (x <= p[i][0]) return p[i - 1][1] + (p[i][1] - p[i - 1][1]) * (x - p[i - 1][0]) / Math.max(p[i][0] - p[i - 1][0], 1e-6);
  }
  return p[p.length - 1][1];
}

export function softcap(v, c, k) {
  if (c <= 0) return v;
  if (k <= 0) return Math.min(v, c);
  if (v <= c - k) return v;
  if (v >= c + k) return c;
  const t = v - c + k;
  return v - t * t / (4 * k);
}

// the built-in effect tables (tr_matexpr.c R_BuiltinTables)
export function tableLookup(name, index) {
  if (name === 'oax_sin') {
    const vals = Array.from({ length: 64 }, (_, i) => Math.fround(Math.sin(i * 2 * Math.PI / 64)));
    vals.push(vals[0]);
    let x = index * 64;
    x -= 64 * Math.floor(x / 64);
    const i = Math.floor(x), f = x - i;
    return vals[i] * (1 - f) + vals[i + 1] * f;
  }
  if (name === 'oax_blink') {
    let x = index * 2;
    x -= 2 * Math.floor(x / 2);
    return Math.floor(x) === 0 ? 1 : 0;
  }
  throw new Error(`no oracle for table ${name}`);
}

export function effect(desc, time) {
  if (!desc.effect) return 1;
  const e = desc.effect;
  return e.base + e.amp * tableLookup(e.table, time * e.rate + e.phase);
}

// one channel's light multiplier at distance d with N.L = cosine
export function light(desc, d, cosine, { ceiling = 1, time = 0, sample } = {}) {
  const x = d / desc.radius;
  if (!desc.on || x >= 1) return [0, 0, 0];
  const v = softcap(desc.intensity * falloff(desc.falloff, x, sample), desc.cap, desc.knee);
  const ang = desc.lambert ? Math.max(0, cosine) : (cosine >= 0 ? 1 : 0);
  const e = effect(desc, time);
  return desc.color.map((c) => Math.max(Math.min(v * ang * c * e, ceiling) - (desc.floor || 0), 0));
}
