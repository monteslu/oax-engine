// Every stock OpenArena map on the cart (phase 5 exit: stock maps pass
// renderer goldens with the unified-lighting code present).
//
// For each of the stock maps: two cameras with real content (chosen from
// the map's spawn points when goldens are created, then fixed in
// goldens/stock/<map>.json) are shot in a fresh session and compared with
// their goldens. A second session shoots the same cameras on the same
// schedule with r_ulight 0: stock maps are lightmapped, so the unified
// lighting path must be inert there and the frames byte-identical.
// Controls: view 0 against view 1's golden must fail; a blank or near-wall
// frame (too few colours) fails.
//
// Slow (two or three sessions per map): runs when named, or with OA_SLOW=1.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { comparePng, readPng, distinctColors, halfSize, writePng } from '../lib/png.mjs';
import { CLEAN_VIEW, loadScene, placeCamera, spawns } from '../lib/scenes.mjs';
import { findBaseoa } from '../lib/native.mjs';
import { execFileSync } from 'node:child_process';

export const name = 'stock-maps';
export const slow = true;

const VIEWS = 2;
const TOLERANCE = 24;
const MAX_BAD_FRACTION = 0.005;
const MIN_COLORS = 2000;

export function stockMaps() {
  const dir = findBaseoa();
  const maps = new Set();
  for (const p of fs.readdirSync(dir).filter((f) => f.endsWith('.pk3'))) {
    const list = execFileSync('unzip', ['-Z1', path.join(dir, p)], { encoding: 'utf8', maxBuffer: 64 << 20 });
    for (const m of list.matchAll(/^maps\/([^/]+)\.bsp$/gm)) maps.add(m[1].toLowerCase());
  }
  return [...maps].sort();
}

async function shoot(s, cam, file) {
  await placeCamera(s, cam);
  await s.screenshot(file);
  return readPng(file);
}

async function chooseCameras(map, out) {
  const s = new Session(`stock-choose-${map}`);
  const chosen = [];
  try {
    await loadScene(s, map);
    const cands = spawns(map).flatMap((p) => [0, 90, 180, 270].map((d) => ({ ...p, yaw: (p.yaw + d) % 360 })));
    for (const cam of cands) {
      if (chosen.length >= VIEWS) break;
      if (chosen.some((c) => Math.hypot(c.x - cam.x, c.y - cam.y) < 1 && c.yaw === cam.yaw)) continue;
      const img = await shoot(s, cam, path.join(out, `stock_${map}_candidate.png`));
      if (distinctColors(img) >= MIN_COLORS) chosen.push(cam);
    }
  } finally {
    await s.shutdown();
  }
  return chosen;
}

async function shootAll(map, cameras, out, tag, view) {
  const s = new Session(`stock-${tag}-${map}`);
  const imgs = [];
  try {
    await loadScene(s, map, view ? { view } : {});
    for (let i = 0; i < cameras.length; i++) imgs.push(await shoot(s, cameras[i], path.join(out, `stock_${map}_${tag}_${i}.png`)));
  } finally {
    await s.shutdown();
  }
  return imgs;
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const maps = process.env.OA_STOCK_MAPS ? process.env.OA_STOCK_MAPS.split(',') : stockMaps();
  let compared = 0, abIdentical = 0;
  // maps run PARALLEL at a time: each session has its own GL context
  const PARALLEL = Number(process.env.OA_STOCK_PARALLEL || 4);
  const queue = [...maps];
  const results = new Map();
  async function one(map) {
    const rows = [], failures = [];
    try {
      const manifest = path.join(goldens, 'stock', `${map}.json`);
      const creating = update || !fs.existsSync(manifest);
      const cameras = creating ? await chooseCameras(map, out) : JSON.parse(fs.readFileSync(manifest, 'utf8')).cameras;
      if (!cameras.length) { failures.push(`${map}: no camera with real content`); results.set(map, { rows, failures }); return; }
      const imgs = await shootAll(map, cameras, out, 'on');
      const off = await shootAll(map, cameras, out, 'off', `${CLEAN_VIEW};r_ulight 0`);
      const cells = [];
      for (let i = 0; i < imgs.length; i++) {
        const colors = distinctColors(imgs[i]);
        if (colors < MIN_COLORS) failures.push(`${map}#${i}: only ${colors} colours`);
        const ab = comparePng(imgs[i], off[i], { tolerance: 0 });
        if (ab.badPixels === 0) abIdentical++;
        else failures.push(`${map}#${i}: r_ulight 0 changes ${ab.badPixels} pixels on a lightmapped map`);
        const golden = path.join(goldens, 'stock', `${map}_${i}.png`);
        if (creating) {
          fs.mkdirSync(path.dirname(golden), { recursive: true });
          writePng(golden, halfSize(imgs[i]));
          cells.push(`#${i} created (${colors} col)`);
          continue;
        }
        const r = comparePng(readPng(golden), halfSize(imgs[i]), { tolerance: TOLERANCE });
        compared++;
        cells.push(`#${i} ${(r.badFraction * 100).toFixed(3)}%`);
        if (!r.sameSize || r.badFraction > MAX_BAD_FRACTION) failures.push(`${map}#${i}: ${(r.badFraction * 100).toFixed(2)}% of pixels differ from the golden`);
      }
      if (creating) fs.writeFileSync(manifest, JSON.stringify({ map, cameras }, null, 2) + '\n');
      if (!creating && imgs.length >= 2) {
        const c = comparePng(readPng(path.join(goldens, 'stock', `${map}_1.png`)), halfSize(imgs[0]), { tolerance: TOLERANCE });
        if (c.badFraction <= MAX_BAD_FRACTION) failures.push(`${map}: control (view 0 vs golden 1) did not fail`);
      }
      rows.push(`${map}: ${cells.join(', ')}; r_ulight A/B identical`);
    } catch (e) {
      failures.push(`${map}: ${e.message.slice(0, 200)}`);
    }
    results.set(map, { rows, failures });
  }
  await Promise.all(Array.from({ length: PARALLEL }, async () => {
    while (queue.length) await one(queue.shift());
  }));
  for (const map of maps) {
    const r = results.get(map);
    rows.push(...r.rows);
    failures.push(...r.failures);
  }
  rows.push(`${maps.length} maps, ${compared} golden comparisons, ${abIdentical} r_ulight A/B frames identical`);
  return { ok: failures.length === 0, failures, rows };
}
