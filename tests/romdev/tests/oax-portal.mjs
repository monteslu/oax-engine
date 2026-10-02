// Area portals that doors and scripts close (oax_portal map), on the cart
// and the native build:
//
// - The map script opens the door 4 s in and closes it 8 s in. While it is
//   closed the far room's area is cut off: the client's snapshot carries
//   fewer entities (cl_snap_entities / snapshot_entities), the renderer
//   draws fewer world surfaces (r_surfs_world), and the far room's speaker
//   is occluded (soundocclusion: snd_occlusionScale instead of 1).
// - The arch is a func_oax_portal with portal_dist: walking near it opens
//   the far room again.
// - Controls: the open-door sample must differ from both closed samples (or
//   the check could not fail), and with snd_occlusion 0 a closed portal
//   does not change the sound.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues, readValues } from '../lib/values.mjs';
import { SPEAKER, ARCH } from '../../maps/src/oax_portal.mjs';

export const name = 'oax-portal';

const OCCLUDE = `soundocclusion ${SPEAKER.join(' ')}`;
const NEAR_ARCH = `setviewpos ${ARCH[0] - 200} ${ARCH[1]} 24 0`;

function sample(v) {
  return {
    time: Number(v.g_script_time),
    ents: Number(v.cl_snap_entities),
    surfs: Number(v.r_surfs_world),
    occl: Number(v.s_occlusion),
  };
}

async function cartRun() {
  const s = new Session('oax-portal');
  const at = async (ms) => s.stepUntil('debug_values', (t) => Number(parseDebugValues(t).g_script_time) >= ms, 3000, 5);
  try {
    await s.load();
    await s.command('bot_enable 0; devmap oax_portal');
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
    await s.command('cg_drawGun 0; cg_draw2D 0');
    const out = {};
    await at(2500);
    await s.command(OCCLUDE);
    await s.step(4);
    out.closed1 = sample(await readValues(s));
    await at(6500);
    await s.command(OCCLUDE);
    await s.step(4);
    out.open = sample(await readValues(s));
    await at(10500);
    await s.command(OCCLUDE);
    await s.step(4);
    out.closed2 = sample(await readValues(s));
    await s.command('snd_occlusion 0');
    await s.step(2);
    await s.command(OCCLUDE);
    await s.step(4);
    out.control = sample(await readValues(s));
    await s.command('snd_occlusion -1');
    await s.step(2);
    await s.command(NEAR_ARCH);
    await s.step(40);
    await s.command(OCCLUDE);
    await s.step(4);
    out.arch = sample(await readValues(s));
    out.archState = (await readValues(s)).g_portal_arch;
    return out;
  } finally {
    await s.shutdown();
  }
}

function nativeRun() {
  const home = runNative('oax-portal', 'oax_portal', [
    'bot_enable 0', 's_useOpenAL 0', 'cg_drawGun 0', 'cg_draw2D 0',
    'wait 10', OCCLUDE, 'wait 4', 'debugvalues p1.txt',
    'wait 520', OCCLUDE, 'wait 4', 'debugvalues p2.txt',
    'wait 520', OCCLUDE, 'wait 4', 'debugvalues p3.txt',
    NEAR_ARCH, 'wait 100', OCCLUDE, 'wait 4', 'debugvalues p4.txt',
  ]);
  const read = (f) => parseDebugValues(fs.existsSync(path.join(home, 'baseoa', f)) ? fs.readFileSync(path.join(home, 'baseoa', f), 'utf8') : '');
  return { closed1: sample(read('p1.txt')), open: sample(read('p2.txt')), closed2: sample(read('p3.txt')), arch: sample(read('p4.txt')), archState: read('p4.txt').g_portal_arch };
}

const fmt = (x) => `t=${x.time} entities ${x.ents} surfaces ${x.surfs} occlusion ${x.occl}`;

export async function run() {
  const failures = [];
  const rows = [];
  const results = [['cart', await cartRun()], ['native', nativeRun()]];

  for (const [label, r] of results) {
    for (const k of ['closed1', 'open', 'closed2', 'arch', ...(r.control ? ['control'] : [])]) rows.push(`${label} ${k}: ${fmt(r[k])}`);
    const { closed1, open, closed2, arch } = r;
    if (!(closed1.time < 4000 && open.time > 5000 && open.time < 8000 && closed2.time > 9000)) failures.push(`${label}: samples at the wrong times (${closed1.time}, ${open.time}, ${closed2.time})`);
    for (const [k, c] of [['closed1', closed1], ['closed2', closed2]]) {
      if (!(c.ents < open.ents)) failures.push(`${label} ${k}: ${c.ents} snapshot entities, not fewer than the open door's ${open.ents}`);
      if (!(c.surfs < open.surfs)) failures.push(`${label} ${k}: ${c.surfs} world surfaces, not fewer than the open door's ${open.surfs}`);
      if (!(c.occl < 1)) failures.push(`${label} ${k}: the far speaker is not occluded (${c.occl})`);
    }
    if (open.occl !== 1) failures.push(`${label}: the far speaker is occluded with the door open (${open.occl})`);
    if (!(arch.ents > closed2.ents) || arch.occl !== 1) failures.push(`${label}: near the arch the far room is still cut off (${fmt(arch)})`);
    if (r.archState !== '1') failures.push(`${label}: the arch portal did not report open (${r.archState})`);
    if (r.control && r.control.occl !== 1) failures.push(`${label} control: snd_occlusion 0 still occludes (${r.control.occl})`);
  }
  return { ok: failures.length === 0, failures, rows };
}
