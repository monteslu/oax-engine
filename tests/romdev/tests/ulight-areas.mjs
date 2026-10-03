// Unified lighting area culling (DESIGN 3.8 check 3): a light whose volume
// lies behind a closed door is culled by the snapshot's areamask.
// The camera in room B looks through door BC at room C. With the door
// closed none of room C's lights may be visible; the player walks into the
// door's trigger, it opens, room C's lights show; the player walks away,
// the door closes, and room C's visible light count drops back to 0.
// Control: r_ulightAreaCull 0 with the door closed must show them.
// Checked on the cart and the native build.

import fs from 'node:fs';
import path from 'node:path';
import { Session, repoRoot } from '../lib/romdev.mjs';
import { loadScene } from '../lib/scenes.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';
import { readEntities } from '../lib/bsp.mjs';
import { nativeHome, nativeBinary, findBaseoa, execNative } from '../lib/native.mjs';
import { execFileSync } from 'node:child_process';
import { CLEAN_VIEW } from '../lib/scenes.mjs';
import { nativeCaptureArgs } from '../lib/capture.mjs';

export const name = 'ulight-areas';

const MAP = 'oax_unified';
const VIEW = '120 0 64 0 0 0';                 // room B, looking +x through door BC
const AWAY = '-100 -250 30 180';               // room B, far from the door
const AT_DOOR = '200 0 30 0';                  // inside door BC's trigger
const STAGES = [
  ['closed', `setviewpos ${AWAY}`, 60],
  ['control (area cull off)', 'r_ulightAreaCull 0', 10],
  ['closed again', 'r_ulightAreaCull 1', 10],
  ['opened', `setviewpos ${AT_DOOR}`, 60],
  ['closed by the game', `setviewpos ${AWAY}`, 240],
];

// ordinals (entity-lump index) of the lights inside room C
function roomCLights() {
  const ents = readEntities(path.join(repoRoot, 'tests', 'maps', 'out', 'baseoa', 'maps', `${MAP}.bsp`));
  return ents.map((e, i) => ({ e, i })).filter(({ e }) => e.classname === 'light' && Number(e.origin.split(' ')[0]) > 320).map(({ i }) => String(i));
}

function countC(values, ids) {
  const vis = String(values.r_ulights_visible_ids || '').split(',');
  return vis.filter((v) => ids.includes(v)).length;
}

async function cartRun() {
  const s = new Session('ulight-areas');
  const vals = [];
  try {
    await loadScene(s, MAP);
    await s.command(`cl_overrideView "${VIEW}"`);
    await s.step(5);
    for (const [, cmd, frames] of STAGES) {
      await s.command(cmd);
      await s.step(frames);
      vals.push(await readValues(s));
    }
  } finally {
    await s.shutdown();
  }
  return vals;
}

function nativeRun() {
  const home = nativeHome('ulight-areas');
  const game = path.join(home, 'baseoa');
  const lines = ['fixedtime 16', ...CLEAN_VIEW.split(';'), 'wait 60', `cl_overrideView "${VIEW}"`, 'wait 10'];
  // a native `wait N` lasts about N/2 frames
  STAGES.forEach(([, cmd, frames], i) => lines.push(cmd, `wait ${frames * 2}`, `debugvalues areas_${i}.txt`));
  lines.push('quit');
  fs.writeFileSync(path.join(game, 'areas.cfg'), lines.join('\n') + '\n');
  execNative([
    '+set', 'fs_basepath', path.dirname(findBaseoa()), '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
    ...nativeCaptureArgs(),
    '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0',
    '+set', 'bot_enable', '0', '+set', 'com_introplayed', '1', '+set', 'fixedtime', '16', '+set', 'com_maxfps', '0',
    '+devmap', MAP, '+wait', '200', '+exec', 'areas.cfg',
  ], { home, timeout: 300000 });
  return STAGES.map((_, i) => {
    const f = path.join(game, `areas_${i}.txt`);
    return fs.existsSync(f) ? parseDebugValues(fs.readFileSync(f, 'utf8')) : {};
  });
}

export async function run() {
  const failures = [];
  const rows = [];
  const ids = roomCLights();
  if (ids.length !== 2) failures.push(`expected 2 room C lights, found ${ids.length}`);
  for (const [build, get] of [['cart', cartRun], ['native', async () => nativeRun()]]) {
    const vals = await get();
    const c = vals.map((v) => countC(v, ids));
    rows.push(`${build}: room C lights visible per stage: ${STAGES.map(([label], i) => `${label} ${c[i]}`).join(', ')} (ids ${vals.map((v) => v.r_ulights_visible_ids).join(' | ')})`);
    if (c[0] !== 0) failures.push(`${build}: door closed but ${c[0]} room C lights visible`);
    if (c[1] === 0) failures.push(`${build}: control: area culling off still showed no room C light`);
    if (c[2] !== 0) failures.push(`${build}: area culling back on, ${c[2]} room C lights visible`);
    if (c[3] === 0) failures.push(`${build}: door open but no room C light visible`);
    if (c[4] !== 0) failures.push(`${build}: door closed again but ${c[4]} room C lights visible`);
  }
  return { ok: failures.length === 0, failures, rows };
}
