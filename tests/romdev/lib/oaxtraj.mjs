// ===========================================================================
// 
// Doom 3 GPL Source Code
// Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company. 
// 
// This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).  
// 
// Doom 3 Source Code is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// Doom 3 Source Code is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.
// 
// In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.
// 
// If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
// 
// ===========================================================================
//
// oaxtraj.mjs: the oax mover trajectories, evaluated in JS for the tests.
//
// Adapted from DOOM-3 neo/idlib/math/Interpolate.h
// (idInterpolateAccelDecelLinear / Sine) and neo/idlib/math/Extrapolate.h
// (the EXTRAPOLATION_* phases), by way of oa-gamecode bg_oax_traj.c: the
// same closed-form phases in JS doubles, plus the packed-duration decoding
// and the func_oax_mover keyframe timeline (not from DOOM-3).

const SQRT_1OVER2 = 0.70710678118654752440;
const HALF_PI = Math.PI / 2;

// BG_OAXPackDuration then BG_OAXUnpackDuration: the accel/decel the
// trajectory actually carries (quantized to 1/255 of the duration).
export function packedTimes(durationMs, accelMs, decelMs) {
  let a = accelMs, d = decelMs;
  const D = durationMs;
  if (a + d > D) { a = Math.trunc(a * D / (a + d)); d = D - a; }
  const aq = Math.trunc((a * 255 + Math.trunc(D / 2)) / D);
  let dq = Math.trunc((d * 255 + Math.trunc(D / 2)) / D);
  if (aq + dq > 255) dq = 255 - aq;
  return { duration: D, accel: (aq * D) / 255, decel: (dq * D) / 255 };
}

// fraction of the move done dt ms after it started
export function accelDecel(dt, duration, accel, decel, sine = false) {
  if (duration <= 0) return 1;
  if (dt < 0) return 0;
  const linear = duration - accel - decel;
  const c = sine ? SQRT_1OVER2 : 0.5;
  const k = 1000 / (linear + (accel + decel) * c);
  if (dt < accel) {
    const f = dt / accel;
    const s = sine ? (1 - Math.cos(f * HALF_PI)) * accel * 0.001 * SQRT_1OVER2 : 0.5 * f * f * accel * 0.001;
    return s * k;
  }
  if (dt < accel + linear) return k * (accel * 0.001 * c) + (dt - accel) * 0.001 * k;
  if (decel <= 0) return 1;
  if (dt > duration) dt = duration;
  const f = (dt - accel - linear) / decel;
  const s = sine ? Math.sin(f * HALF_PI) * decel * 0.001 * SQRT_1OVER2 : (f - 0.5 * f * f) * decel * 0.001;
  return 1 - k * (decel * 0.001 * c) + s * k;
}

const lerp = (a, b, f) => a.map((v, i) => v + (b[i] - v) * f);

// func_oax_mover in mode loop, started at `start`: { origin, angles,
// segment: {index, from, to, t0} or null while waiting, keyTime: true when
// t is exactly a segment boundary }
export function keyedLoop(m, start, t) {
  const n = m.keys.length;
  const seg = Math.round(m.movetime * 1000);
  const glide = Math.round(m.glide * 1000);
  const wait = Math.round(m.stayOpen * 1000);
  const { duration, accel, decel } = packedTimes(seg, glide, glide);
  const half = (n - 1) * seg + wait;
  const cycle = 2 * half;
  const rel = t - start;
  const k = Math.floor(rel / cycle);
  let r = rel - k * cycle;
  const forward = r < half;
  if (!forward) r -= half;
  const seq = forward ? [...Array(n).keys()] : [...Array(n).keys()].reverse();
  const at = (i) => ({ origin: m.keys[i].map((v, j) => v + m.origin[j]), angles: m.angles[i] });
  const keyTime = r % seg === 0 && r <= (n - 1) * seg;
  if (r >= (n - 1) * seg) return { ...at(seq[n - 1]), segment: null, keyTime };
  const s = Math.floor(r / seg);
  const f = accelDecel(r - s * seg, duration, accel, decel);
  const a = at(seq[s]), b = at(seq[s + 1]);
  return {
    origin: lerp(a.origin, b.origin, f), angles: lerp(a.angles, b.angles, f), keyTime,
    segment: { index: `${k}:${forward ? 'f' : 'b'}:${s}`, from: a.origin, to: b.origin, fromA: a.angles, toA: b.angles },
  };
}
