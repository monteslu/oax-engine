// Renderer goldens: stock maps seen from fixed cameras, compared against
// approved screenshots with a tolerance.
//
// Cameras come from each map's own spawn points, turned to the four compass
// directions. Many of those face a wall, which proves nothing about the
// renderer, so when goldens are (re)created the views with real content are
// chosen and written to <map>.json; later runs use exactly those cameras.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { comparePng, readPng, distinctColors, halfSize, writePng } from '../lib/png.mjs';
import { loadScene, placeCamera, spawns } from '../lib/scenes.mjs';

export const name = 'render-goldens';

const MAPS = (process.env.OA_GOLDEN_MAPS || 'oa_dm1,oa_dm4,oa_ctf2').split(',');
const VIEWS_PER_MAP = 6;
const MAX_BAD_FRACTION = 0.005;   // 0.5% of pixels may differ beyond tolerance
const TOLERANCE = 24;
const MIN_COLORS = 3000;          // a view of a near wall or a blank frame is not a test

function candidates(map) {
  return spawns(map).flatMap((p) => [0, 90, 180, 270].map((d) => ({ ...p, yaw: (p.yaw + d) % 360 })));
}

async function shoot(s, cam, file) {
  await placeCamera(s, cam);
  await s.screenshot(file);
  return readPng(file);
}

// Choose cameras: candidates in order, keeping views with real content.
async function chooseCameras(map, out) {
  const s = new Session(`choose-${map}`);
  const chosen = [];
  try {
    await loadScene(s, map);
    for (const cam of candidates(map)) {
      if (chosen.length >= VIEWS_PER_MAP) break;
      const img = await shoot(s, cam, path.join(out, `${map}_candidate.png`));
      if (distinctColors(img) >= MIN_COLORS) chosen.push(cam);
    }
  } finally {
    await s.shutdown();
  }
  return chosen;
}

// Shoot every camera in order in a fresh session. Goldens and checks both
// come from this, so animated content (flames, lava, items) is at the same
// phase in both.
async function shootAll(map, cameras, out) {
  const s = new Session(`golden-${map}`);
  const files = [];
  try {
    await loadScene(s, map);
    for (let i = 0; i < cameras.length; i++) {
      const file = path.join(out, `${map}_${i}.png`);
      await shoot(s, cameras[i], file);
      files.push(file);
    }
  } finally {
    await s.shutdown();
  }
  return files;
}

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  for (const map of MAPS) {
    const manifest = path.join(goldens, 'render', `${map}.json`);
    const creating = update || !fs.existsSync(manifest);
    const cameras = creating ? await chooseCameras(map, out) : JSON.parse(fs.readFileSync(manifest, 'utf8')).cameras;
    if (creating && cameras.length < 3) failures.push(`${map}: only ${cameras.length} views with real content`);
    const files = await shootAll(map, cameras, out);
    for (let i = 0; i < files.length; i++) {
      const img = readPng(files[i]);
      const colors = distinctColors(img);
      const golden = path.join(goldens, 'render', `${map}_${i}.png`);
      if (colors < MIN_COLORS) failures.push(`${map}#${i}: only ${colors} colours, not a rendered map`);
      const half = halfSize(img);
      if (creating) {
        fs.mkdirSync(path.dirname(golden), { recursive: true });
        writePng(golden, half);
        const c = cameras[i];
        rows.push(`${map}#${i}: golden ${update ? 'updated' : 'created'} at ${c.x} ${c.y} ${c.z} yaw ${c.yaw} (${colors} colours)`);
        continue;
      }
      const r = comparePng(readPng(golden), half, { tolerance: TOLERANCE, diffPath: path.join(out, `${map}_${i}.diff.png`) });
      rows.push(`${map}#${i}: ${(r.badFraction * 100).toFixed(3)}% over tolerance, mean ${r.meanDiff?.toFixed(2)}, ${colors} colours`);
      if (!r.sameSize || r.badFraction > MAX_BAD_FRACTION) failures.push(`${map}#${i}: ${(r.badFraction * 100).toFixed(2)}% of pixels differ`);
    }
    if (creating) fs.writeFileSync(manifest, JSON.stringify({ map, cameras }, null, 2) + '\n');

    // Control: a view compared with a DIFFERENT camera's golden must fail,
    // or a passing comparison above proves nothing.
    if (files.length >= 2) {
      const r = comparePng(readPng(path.join(goldens, 'render', `${map}_1.png`)), halfSize(readPng(files[0])), { tolerance: TOLERANCE });
      if (r.badFraction <= MAX_BAD_FRACTION) failures.push(`${map}: control did not fail (view 0 vs golden 1 differ by only ${(r.badFraction * 100).toFixed(2)}%)`);
      else rows.push(`${map}: control fails as it must (${(r.badFraction * 100).toFixed(1)}% differ)`);
    }
  }
  return { ok: failures.length === 0, failures, rows };
}
