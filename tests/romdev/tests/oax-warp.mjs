// Seamless warp zones (trigger_teleport spawnflag 4, design 2.9) on
// oax_warp, on the cart and the native build:
// - running through the warp: native and cart traces identical; at the
//   crossing the speed is kept (+-0.5), the yaw turns exactly 90 and there
//   is no 400 ups teleport kick; control: the classic teleporter in
//   corridor C must fail the same speed check (it kicks);
// - a grenade crosses the warp and flies on in corridor B; control: in
//   corridor C (classic teleporter) no missile is warped;
// - looking at the warp shows corridor B: the view through the portal
//   matches the view from the warped eye in B; control: the same eye in
//   corridor C (a plain end wall) does not.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { loadScene, placeAt } from '../lib/scenes.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues, readValues } from '../lib/values.mjs';
import { cartRun, nativeRun, compareRows } from '../lib/sim.mjs';
import { readPng, comparePng, distinctColors } from '../lib/png.mjs';

export const name = 'oax-warp';

const MAP = 'oax_warp';
const A = { x: -560, y: 0, z: 32, yaw: 0 };
const C = { x: -560, y: 1200, z: 32, yaw: 0 };
// corridor B runs x 928..1120, y -352..960
const inB = (x, y) => x > 928 && x < 1120 && y > -352 && y < 960;

const speed = (r) => Math.hypot(r[4], r[5], r[6]);
const yawDelta = (a, b) => ((((b - a) % 360) + 540) % 360) - 180;

// the row pair where the view turns (the warp or teleport)
function crossing(rows) {
  for (let i = 1; i < rows.length; i++) {
    if (Math.abs(yawDelta(rows[i - 1][8], rows[i][8])) > 1) return { before: rows[i - 1], after: rows[i] };
  }
  return null;
}

function checkCrossing(label, rows, fail, out) {
  const c = crossing(rows);
  if (!c) { fail(`${label}: never crossed`); return null; }
  const vb = speed(c.before), va = speed(c.after), dy = yawDelta(c.before[8], c.after[8]);
  out.push(`${label}: crossed at t+${c.after[0]}ms from (${c.before[1].toFixed(2)} ${c.before[2].toFixed(2)}) to (${c.after[1].toFixed(2)} ${c.after[2].toFixed(2)}); speed ${vb.toFixed(3)} -> ${va.toFixed(3)}; yaw ${c.before[8]} -> ${c.after[8]} (${dy})`);
  return { vb, va, dy, after: c.after };
}

function cartRender(out) {
  // the eye 24 units before the warp plane (56 before the portal wall) and the same eye carried
  // through the warp; the player stands in each corridor so its area is in
  // the snapshot's area mask
  const views = {
    a: { place: { x: -300, y: 0, z: 32, yaw: 0 }, view: '-56 0 50 0 0 0' },
    b: { place: { x: 1024, y: 400, z: 32, yaw: 270 }, view: '1024 -344 50 0 90 0' },
    c: { place: { x: -300, y: 1200, z: 32, yaw: 0 }, view: '-56 1200 50 0 0 0' },
  };
  return (async () => {
    const s = new Session('oax-warp-render');
    const shots = {};
    try {
      await loadScene(s, MAP);
      for (const [k, v] of Object.entries(views)) {
        await s.command('cl_overrideView ""');
        await placeAt(s, v.place, 60);
        await s.command(`cl_overrideView "${v.view}"`);
        await s.step(30);
        const f = path.join(out, `oax_warp_view_${k}.png`);
        await s.screenshot(f);
        shots[k] = readPng(f);
      }
    } finally {
      await s.shutdown();
    }
    return shots;
  })();
}

function nativeGrenade(tag, p) {
  const home = runNative(`oax-warp-grenade-${tag}`, MAP, [
    'wait 60', `setviewpos ${p.x} ${p.y} ${p.z} ${p.yaw}`, 'wait 200', 'give all', 'wait 120', 'weapon 4', 'wait 120',
    '+attack', 'wait 6', '-attack', 'wait 400', 'debugvalues grenade.txt',
  ]);
  const f = path.join(home, 'baseoa', 'grenade.txt');
  return fs.existsSync(f) ? parseDebugValues(fs.readFileSync(f, 'utf8')) : {};
}

async function cartGrenade(tag, p) {
  const s = new Session(`oax-warp-grenade-${tag}`);
  try {
    await loadScene(s, MAP);
    await placeAt(s, p, 80);
    // the cgame only switches to a weapon the snapshot says we hold
    await s.command('give all');
    await s.step(30);
    await s.command('weapon 4');
    await s.step(60);
    await s.command('+attack');
    await s.step(3);
    await s.command('-attack');
    await s.step(200);
    return await readValues(s);
  } finally {
    await s.shutdown();
  }
}

export async function run({ out }) {
  const failures = [];
  const rows = [];
  const fail = (m) => failures.push(m);

  // --- running through the warp ---
  let cartA, cartC;
  for (const [k, p] of [['a', A], ['c', C]]) {
    const s = new Session(`oax-warp-run-${k}`);
    try {
      const r = await cartRun(s, MAP, p, 'wrun');
      if (k === 'a') cartA = r; else cartC = r;
    } finally {
      await s.shutdown();
    }
  }
  const natA = nativeRun('oax-warp-run', MAP, A, 'wrun');
  fs.writeFileSync(path.join(out, 'oax_warp_run.cart.json'), JSON.stringify(cartA.rows));

  for (const [b, r] of [['cart', cartA], ['native', natA]]) {
    const x = checkCrossing(`${b} warp`, r.rows, fail, rows);
    rows.push(`${b}: g_warp_count ${r.values.g_warp_count}, g_warp_last "${r.values.g_warp_last}"`);
    if (!x) continue;
    if (Math.abs(x.va - x.vb) > 0.5) fail(`${b}: warp changed speed ${x.vb} -> ${x.va}`);
    if (x.va > 400 - 0.5 && x.va < 400 + 0.5) fail(`${b}: warp kicked the player at 400 ups`);
    if (Math.abs(x.dy - 90) > 1e-4) fail(`${b}: warp turned yaw by ${x.dy}, want 90`);
    if (!inB(x.after[1], x.after[2])) fail(`${b}: after the warp the player is at ${x.after[1]} ${x.after[2]}, not in corridor B`);
    if (r.values.g_warp_count !== '1') fail(`${b}: g_warp_count ${r.values.g_warp_count}, want 1`);
  }
  const cmp = compareRows(natA.rows, cartA.rows);
  rows.push(`warp run: ${cmp.common} common command times (native ${natA.rows.length}, cart ${cartA.rows.length}), ${cmp.firstDiff ? 'first difference ' + cmp.firstDiff : 'identical'}`);
  if (cmp.common < 100) fail(`warp run: only ${cmp.common} rows in common`);
  if (cmp.firstDiff) fail(`warp run: native and cart differ at ${cmp.firstDiff}`);

  // control: the classic teleporter kicks, so the same speed check fails
  const xc = checkCrossing('control (classic teleporter)', cartC.rows, fail, rows);
  if (xc && Math.abs(xc.va - xc.vb) <= 0.5) fail('control: the classic teleporter kept the speed (the speed check cannot fail)');
  if (cartC.values.g_warp_count) fail(`control: classic teleporter counted as a warp (${cartC.values.g_warp_count})`);
  const cc = compareRows(natA.rows, cartC.rows);
  if (!cc.firstDiff) fail('control: corridor C run matched the warp run');

  // --- a grenade through the warp ---
  const gp = { x: -380, y: 0, z: 32, yaw: 0 };
  const gc = { x: -380, y: 1200, z: 32, yaw: 0 };
  for (const [b, warp, ctl] of [
    ['cart', await cartGrenade('a', gp), await cartGrenade('c', gc)],
    ['native', nativeGrenade('a', gp), nativeGrenade('c', gc)],
  ]) {
    rows.push(`${b} grenade: g_warp_missiles ${warp.g_warp_missiles}, "${warp.g_warp_missile}"; control (corridor C) ${ctl.g_warp_missiles || 'none'}`);
    const m = String(warp.g_warp_missile || '').split(' ').map(Number);
    if (warp.g_warp_missiles !== '1') fail(`${b}: grenade warps ${warp.g_warp_missiles}, want 1`);
    else {
      if (!inB(m[1], m[2])) fail(`${b}: warped grenade at ${m[1]} ${m[2]}, not in corridor B`);
      if (!(m[5] > 100 && Math.abs(m[4]) < 1)) fail(`${b}: warped grenade velocity ${m[4]} ${m[5]}, want along +Y`);
    }
    if (ctl.g_warp_missiles) fail(`${b}: control grenade in corridor C was warped`);
  }

  // --- the view through the warp ---
  const v = await cartRender(out);
  const through = comparePng(v.a, v.b, { tolerance: 24 });
  const plain = comparePng(v.c, v.b, { tolerance: 24 });
  rows.push(`render: portal view vs warped eye in B: ${(through.badFraction * 100).toFixed(2)}% differ (mean ${through.meanDiff.toFixed(2)}); ${distinctColors(v.a)} colours`);
  rows.push(`render control: the same eye in corridor C (plain end wall) vs the B view: ${(plain.badFraction * 100).toFixed(2)}% differ (mean ${plain.meanDiff.toFixed(2)})`);
  if (through.badFraction > 0.05) fail(`render: the warp does not show corridor B (${(through.badFraction * 100).toFixed(2)}% differ)`);
  if (plain.badFraction < 0.2) fail('render control: a plain wall matched the far corridor (the check cannot fail)');
  if (distinctColors(v.a) < 200) fail('render: the portal view is flat');

  return { ok: failures.length === 0, failures, rows };
}
