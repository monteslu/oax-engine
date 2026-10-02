// Unified lighting frame time (DESIGN 3.6 measurement, 3.8 check 5).
//
// The cart cannot read a wall clock in deterministic mode (the engine's
// clock only moves between frames, so the renderer's own r_frame_ms reads
// 0 there): the frame time is measured by the host, around romdev's frame
// steps, in batches of 30 frames along a fixed camera path. GL work is
// queued, so each batch ends with a screenshot (a pixel read waits for the
// GPU); the cost of a screenshot alone, taken right after, is subtracted. The renderer's
// own counters (interaction draws, shadow passes, stencil triangles) come
// from its debug values.
//
// Gate: oax_unified with its own settings (shadow maps), p95 <= OA_ULIGHT_P95_MS
// (default 16.6 ms).
// OA_ULIGHT_PERF_FULL=1 also runs the shadow-map vs stencil A/B: 4/8/16/32
// shadowed lights, static and moving, 0 and 4 MD3 players (bots), and
// writes ulight-perf.json to the test output directory.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, CLEAN_VIEW } from '../lib/scenes.mjs';
import { readValues } from '../lib/values.mjs';

export const name = 'ulight-perf';

const P95_MS = Number(process.env.OA_ULIGHT_P95_MS || 16.6);
const BATCH = 30;

// the camera path: around the pillar hall, deterministic
function pathView(i, n) {
  const a = (i / n) * Math.PI * 2;
  const x = -820 + Math.cos(a) * 380, y = Math.sin(a) * 260;
  const yaw = (a * 180 / Math.PI + 180) % 360;
  return `${x.toFixed(1)} ${y.toFixed(1)} 120 12 ${yaw.toFixed(1)} 0`;
}

function p95(xs) {
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor(s.length * 0.95))];
}

async function measure(map, { frames, preload = '', pre = '', bots = 0, label, out }) {
  const s = new Session(`ulight-perf-${label}`);
  try {
    await loadScene(s, map, { view: preload ? `${CLEAN_VIEW};${preload}` : CLEAN_VIEW });
    const cmds = ['setviewpos -820 -150 30 90', pre];
    for (let b = 0; b < bots; b++) cmds.push(`addbot sarge 1 free ${50 * b}`);
    await s.command(cmds.filter(Boolean).join(';'));
    await s.step(120);
    const times = [];
    const batches = Math.ceil(frames / BATCH);
    const shot = path.join(out, `ulight-perf-${label}.png`);
    const shotCosts = [];
    for (let i = 0; i < 6; i++) {
      const t0 = performance.now();
      await s.screenshot(shot);
      shotCosts.push(performance.now() - t0);
    }
    shotCosts.sort((a, b) => a - b);
    const shotCost = shotCosts[2];
    const counters = { draws: 0, shadowPasses: 0, stencilTris: 0 };
    for (let i = 0; i < batches; i++) {
      await s.command(`cl_overrideView "${pathView(i, batches)}"`);
      // the batch ends with a screenshot (waits for the GPU); a second one
      // right after costs the same encode without any frames to wait for
      const t0 = performance.now();
      await s.step(BATCH);
      await s.screenshot(shot);
      const t1 = performance.now();
      await s.screenshot(shot);
      const t2 = performance.now();
      times.push(Math.max(0, (t1 - t0) - (t2 - t1)) / BATCH);
      if (i % 10 === 0) {
        const v = await readValues(s);
        counters.draws = Math.max(counters.draws, Number(v.r_ulight_draws || 0));
        counters.shadowPasses = Math.max(counters.shadowPasses, Number(v.r_shadow_passes || 0));
        counters.stencilTris = Math.max(counters.stencilTris, Number(v.r_stencil_tris || 0));
        counters.visible = v.r_ulights_visible;
      }
    }
    return { p95: p95(times), mean: times.reduce((a, b) => a + b, 0) / times.length, shotCost, ...counters };
  } finally {
    await s.shutdown();
  }
}

export async function run({ out }) {
  const failures = [];
  const rows = [];

  const main = await measure('oax_unified', { frames: 600, label: 'main', out });
  rows.push(`oax_unified (shadow maps): p95 ${main.p95.toFixed(2)} ms, mean ${main.mean.toFixed(2)} ms per frame (host wall time, GPU-synced); max ${main.draws} interaction draws, ${main.shadowPasses} shadow passes per frame`);
  if (!(main.p95 <= P95_MS)) failures.push(`oax_unified p95 ${main.p95.toFixed(2)} ms > ${P95_MS} ms`);
  if (!main.draws) failures.push('no interaction draws counted (is the map unified?)');

  if (process.env.OA_ULIGHT_PERF_FULL) {
    const results = [];
    for (const n of [4, 8, 16, 32]) {
      for (const moving of [false, true]) {
        for (const bots of [0, 4]) {
          const map = `oax_ulight_perf${n}${moving ? 'm' : ''}`;
          const maps = await measure(map, { frames: 600, bots, label: `${n}${moving ? 'm' : ''}-${bots}-maps`, out, pre: 'r_ulightShadowMode 1' });
          const sten = await measure(map, { frames: 600, bots, label: `${n}${moving ? 'm' : ''}-${bots}-stencil`, out, preload: 'r_ulightStencil 1', pre: 'r_ulightShadowMode 2' });
          const r = { lights: n, moving, bots, maps, stencil: sten, stencilFaster: (maps.p95 - sten.p95) / maps.p95 };
          results.push(r);
          rows.push(`${String(n).padStart(2)} ${moving ? 'moving' : 'static'} lights, ${bots} players: maps p95 ${maps.p95.toFixed(2)} ms (${maps.shadowPasses} passes), stencil p95 ${sten.p95.toFixed(2)} ms (${sten.stencilTris} tris): stencil ${(r.stencilFaster * 100).toFixed(0)}% faster`);
        }
      }
    }
    // decision rule (3.6): stencil only if at least 20% faster at p95 everywhere
    const worst = Math.min(...results.map((r) => r.stencilFaster));
    const pick = worst >= 0.2 ? 'stencil' : 'shadow maps';
    rows.push(`decision: ${pick} (stencil's worst p95 advantage ${(worst * 100).toFixed(0)}%; the rule needs >= 20%)`);
    fs.writeFileSync(path.join(out, 'ulight-perf.json'), JSON.stringify({ main, results, pick }, null, 2));
  }
  return { ok: failures.length === 0, failures, rows };
}
