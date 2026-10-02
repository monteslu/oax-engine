// Phase 6 cosmetic physics on the cart: gibs as Box3D bodies, explosions
// that push them, and a ragdoll for a skeletal (IQM) player.
//
// oax_phys, the player as models/players/iqmguy (the gamecode's CC0 IQM
// player, skeleton.cfg):
//   - `cg_physTest gib` throws a set of stock gibs as bodies in the cgame's
//     cosmetic world; after a few seconds they must all be at rest on the
//     floor (lowest body above the floor, none awake);
//   - `cg_physTest explode` beside the resting ragdoll must wake it again;
//   - the player walks into the map's hurt volume (140 damage: dead, not
//     gibbed): a ragdoll must start, fall and come to rest lying on the
//     floor in that corner, and the frame must show it (the model's yellow);
// Controls that must differ: the same death with cg_physics 0 makes no
// world and no ragdoll; an MD3 player (sarge) dies with the stock
// animation and no ragdoll either.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { readValues } from '../lib/values.mjs';
import { readPng } from '../lib/png.mjs';

export const name = 'physics-cosmetic';

const MAP = 'oax_phys';
const HURT = 'setviewpos -840 840 40 135';                 // into the hurt corner, facing it
const CORNER_VIEW = 'cl_overrideView "-820 820 140 50 135 0"';

async function boot(s, extra) {
  // the model change has to reach the server as userinfo before the map
  // change, or the server keeps the old one across it
  await s.command(`${extra}; bot_enable 0; g_doWarmup 0`);
  await s.step(10);
  await s.command(`devmap ${MAP}`);
  await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
  await s.step(30);
  await s.command('cg_thirdPerson 1; cg_drawGun 0; cg_draw2D 0; cg_drawFPS 0');
  await s.step(5);
}

function yellowPixels(img) {
  let n = 0;
  for (let i = 0; i < img.width * img.height; i++) {
    const r = img.data[i * 4], g = img.data[i * 4 + 1], b = img.data[i * 4 + 2];
    if (r > 120 && g > 110 && b < 70 && Math.abs(r - g) < 70) n++;
  }
  return n;
}

function ragdoll(v) {
  const p = String(v.cg_skel_ragdoll || 'none').split(' ');
  return p.length >= 5 ? { x: +p[0], y: +p[1], z: +p[2], awake: p[3] === '1' } : null;
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const s = new Session('physics-cosmetic');
  try {
    await s.load();

    // gibs and an explosion, iqmguy, physics on
    await boot(s, 'model iqmguy; headmodel iqmguy; cg_physics 1');
    let v = await readValues(s);
    if (!(Number(v.cg_phys_world) > 0)) failures.push('no cosmetic world');
    const staticBodies = Number(v.phys_cgame_bodies || 0);
    await s.command('cg_physTest gib');
    await s.step(10);
    v = await readValues(s);
    const flying = Number(v.cg_phys_fragments);
    rows.push(`gibs: ${flying} fragments with bodies, ${v.cg_phys_awake} awake bodies just after`);
    if (!(flying >= 8)) failures.push(`only ${flying} gib bodies`);
    await s.step(360);
    v = await readValues(s);
    const rest = { awake: Number(v.cg_phys_awake), minz: Number(v.cg_phys_frag_minz), frags: Number(v.cg_phys_fragments) };
    rows.push(`gibs after 6 s: ${rest.frags} fragments, ${rest.awake} awake, lowest at z ${rest.minz.toFixed(1)} (floor 0)`);
    if (rest.awake > 0) failures.push(`${rest.awake} gib bodies still awake after 6 s`);
    if (!(rest.minz > -2)) failures.push(`a gib fell through the floor (z ${rest.minz})`);
    // the ragdoll
    await s.step(400);	// the gibs time out
    await s.command(`${CORNER_VIEW}; ${HURT}`);
    await s.step(8);
    v = await readValues(s);
    const early = ragdoll(v);
    await s.step(300);
    v = await readValues(s);
    const settled = ragdoll(v);
    const shot = path.join(out, 'physics-cosmetic_ragdoll.png');
    await s.screenshot(shot);
    const yellow = yellowPixels(readPng(shot));
    rows.push(`ragdoll: ${v.cg_phys_ragdolls} ragdoll(s); pelvis ${early ? `z ${early.z.toFixed(1)}` : 'none'} after 8 frames, ` +
      `${settled ? `${settled.x.toFixed(0)} ${settled.y.toFixed(0)} ${settled.z.toFixed(1)}, ${settled.awake ? 'awake' : 'asleep'}` : 'none'} after 5 s; ` +
      `${Number(v.phys_cgame_bodies) - staticBodies} ragdoll bodies, ${v.phys_cgame_joints} joints; ${yellow} model pixels in the shot`);
    if (Number(v.cg_phys_ragdolls) !== 1) failures.push(`${v.cg_phys_ragdolls} ragdolls, wanted 1`);
    if (!settled) failures.push('no ragdoll state');
    else {
      if (settled.awake) failures.push('the ragdoll never came to rest');
      if (!(settled.z > 0 && settled.z < 20)) failures.push(`the ragdoll's pelvis rests at z ${settled.z}, not lying on the floor`);
      if (!(settled.x < -700 && settled.y > 700)) failures.push(`the ragdoll ended at ${settled.x} ${settled.y}, not in the hurt corner`);
      if (early && !(early.z > settled.z + 5)) failures.push('the ragdoll did not fall (started no higher than it ended)');
    }
    if (!(Number(v.phys_cgame_joints) >= 5)) failures.push(`only ${v.phys_cgame_joints} ragdoll joints`);
    if (!(yellow > 200)) failures.push(`the ragdoll is not in the frame (${yellow} model pixels)`);
    // an explosion beside the resting ragdoll throws it again
    if (settled) {
      await s.command(`cg_physTest explode ${settled.x + 20} ${settled.y - 20} 4`);
      await s.step(3);
      v = await readValues(s);
      const blown = ragdoll(v);
      rows.push(`explosion beside it: ragdoll ${blown && blown.awake ? 'awake' : 'still asleep'}, ${v.cg_phys_awake} bodies awake`);
      if (!blown || !blown.awake) failures.push('the explosion did not wake the ragdoll');
    }

    // control 1: no cosmetic physics, no ragdoll
    await boot(s, 'cg_physics 0');
    await s.command(`${CORNER_VIEW}; ${HURT}`);
    await s.step(120);
    v = await readValues(s);
    rows.push(`control cg_physics 0: world ${v.cg_phys_world}, ragdoll ${v.cg_skel_ragdoll}, ${v.cg_phys_ragdolls} ragdolls`);
    if (Number(v.cg_phys_world) !== 0 || v.cg_skel_ragdoll !== 'none') failures.push('cg_physics 0 still made a world or a ragdoll');
    const lying = path.join(out, 'physics-cosmetic_nophysics.png');
    await s.screenshot(lying);

    // control 2: an MD3 player keeps the stock death
    await boot(s, 'model sarge; headmodel sarge; cg_physics 1');
    await s.command(`${CORNER_VIEW}; ${HURT}`);
    await s.step(120);
    v = await readValues(s);
    rows.push(`control MD3 sarge: world ${v.cg_phys_world}, ${v.cg_phys_ragdolls} ragdolls, ragdoll ${v.cg_skel_ragdoll}`);
    if (!(Number(v.cg_phys_world) > 0)) failures.push('MD3 control: no world (cannot tell)');
    if (Number(v.cg_phys_ragdolls) !== 0) failures.push('an MD3 player got a ragdoll');
  } finally {
    await s.shutdown();
  }
  fs.writeFileSync(path.join(out, 'physics-cosmetic.txt'), rows.join('\n') + '\n');
  return { ok: failures.length === 0, failures, rows };
}
