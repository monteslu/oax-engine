// Navigation from intent (docs/navigation.md) on oax_nav_intent:
// a teleporter pair whose arrivals sit inside the partner triggers
// ("noretrigger 1"), a hazard over an item, a translocator-only ledge, a jump
// pad arc and a ladder, on a map with no AAS (navmesh bots).
//
// 1. Teleporter traces: the native client and the cart play the same pad
//    script through the pair (in, off the arrival, back in, out) and their
//    player states must be identical at every command time; the script
//    teleports exactly twice and stands on the tower in between.
//    Control: on oax_nav_intent_classic (no noretrigger) the same script
//    ping-pongs (many teleports, it never walks off the arrival).
// 2. Links and routes: every authored link reaches the navmesh at both ends,
//    the mesh hash and every route are identical native vs cart, and each
//    item routes through its link kind. Controls: the ledge is unreachable
//    without the translocator kind; the hazard item is unreachable when
//    hazards are excluded (bspc's lava); with sv_navLinks 0 the tower,
//    ledge, platform and ladder items are unreachable.
// 3. Translocator: a pad script throws the beacon and ports to it, identical
//    native vs cart; control: with the rule off nothing is thrown. Telefrag:
//    porting onto a standing bot (bot_oaxIdle 1) kills it; control: the same
//    port thrown away from it leaves it alive.
// 4. Bots (g_oaxTranslocator 1, two navmesh bots): every item is touched and
//    every link kind is completed; the bot match is identical native vs
//    cart (hash checkpoints). Control: with the rule off the ledge item is
//    unroutable and never touched.
//
// OA_NAVINTENT_FRAMES (default 6000 = 96 s at 16 ms) sets the bot match length.

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE } from '../lib/romdev.mjs';
import { readValues, parseDebugValues } from '../lib/values.mjs';
import { CLEAN_VIEW } from '../lib/scenes.mjs';
import { runNative, nativeHome, findBaseoa, execNative } from '../lib/native.mjs';
import { TRACE_ROWS, TRACE_COLS } from '../lib/movement.mjs';
import { SIM_SCRIPTS } from '../lib/sim-scripts.mjs';

export const name = 'nav-intent';
// two cart bot matches and a native one of OA_NAVINTENT_FRAMES, plus short runs
export const timeoutSec = 2400;

const MAP = 'oax_nav_intent';
const CLASSIC = 'oax_nav_intent_classic';
const FRAMES = Number(process.env.OA_NAVINTENT_FRAMES || 6000);
const TELE = { x: -704, y: -740, z: 24, yaw: 90 };
const TL = { x: 0, y: -300, z: 24, yaw: 0 };
// measured on both builds: setviewpos slides a player this far forward, and a
// level throw from standing lands this far ahead (section 3 checks the port)
const TL_SLIDE = 117.03;
const TL_THROW = 310;
const TOWER_TOP = 256;
// [label, goal, include, exclude, expect]: expect 'full' (with link kind) or 'none'
const SPAWN = '0 0 8';
const ROUTES = [
  ['tower (teleporter)', '-704 470 256', null, null, 'teleport'],
  ['hazard item', '384 -512 0', null, null, 'walk'],
  ['hazard item, hazards excluded (control)', '384 -512 0', 1 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100, 2, 'none'],
  ['ledge, no translocator (control)', '880 560 192', null, null, 'none'],
  ['ledge with translocator', '880 560 192', 0x11f3, null, 'translocator'],
  ['platform (jump pad)', '0 700 256', null, null, 'jumppad'],
  ['ladder ledge', '-320 -700 160', null, null, 'ladder'],
];
const KINDS = { 0x10: 'teleport', 0x20: 'jumppad', 0x40: 'ladder', 0x80: 'jump', 0x100: 'drop', 0x1000: 'translocator' };

const n = (x) => Number(x ?? 0);

function navPathCmd([, goal, inc, exc]) {
  return `nav_path ${SPAWN} ${goal}${inc != null ? ` ${inc}` : ''}${exc != null ? ` ${exc}` : ''}`;
}

// "count flags x y z ..." -> {count, partial}
function parsePath(s) {
  const p = String(s ?? '').trim().split(/\s+/).map(Number);
  return { count: p[0] || 0, partial: !!((p[1] || 0) & 1), text: String(s) };
}

function linkKinds(links, v) {
  if (!links || links === '-') return [];
  return links.split(' ').map((i) => String(v[`g_nav_link_${i}`] || '?').split(' ')[0]);
}

function nativeRows(file) {
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  const start = lines.find((l) => l.startsWith('# start '));
  return {
    start: start ? Number(start.split(' ')[2]) : null,
    rows: lines.filter((l) => !l.startsWith('#')).map((l) => l.split(' ').map((v, k) => (k >= 1 && k <= 6 ? Math.fround(Number(v)) : Number(v)))),
  };
}

// rows from the script start; null when nothing moved (a port moves without velocity)
function fromStart(rows, t0) {
  if (t0 == null || t0 < 0) return null;
  const xs = rows.filter((r) => r[0] >= t0);
  if (!xs.length || !xs.some((r) => Math.hypot(r[4], r[5]) > 0.5 || Math.hypot(r[1] - xs[0][1], r[2] - xs[0][2]) > 0.5)) return null;
  const out = new Map();
  for (const r of rows) if (r[0] >= t0 && !out.has(r[0] - t0)) out.set(r[0] - t0, r);
  return out;
}

function compare(a, b) {
  let common = 0, firstDiff = null, maxErr = 0;
  for (const [dt, x] of a) {
    const y = b.get(dt);
    if (!y) continue;
    common++;
    for (let k = 1; k <= 7; k++) {
      const e = Math.abs(x[k] - y[k]);
      if (e > maxErr) maxErr = e;
      if (e > 0 && !firstDiff) firstDiff = `t+${dt}ms col ${k}: native ${x[k]} vs cart ${y[k]}`;
    }
  }
  return { common, firstDiff, maxErr };
}

async function boot(s, map, rules = '') {
  await s.load();
  await s.command(`${CLEAN_VIEW};bot_enable 1;g_doWarmup 0;timelimit 0;fraglimit 0;set sv_gameSeed 7${rules ? `;${rules}` : ''}`);
  await s.step(1);
  await s.command(`devmap ${map}`);
  await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 4000, 20);
  await s.command(CLEAN_VIEW);
  await s.step(60);
  const m = await s.read('mapname');
  if (m !== map) throw new Error(`loaded ${m}, not ${map}`);
}

// the pad script on the cart from a placed player; returns {rows, v}
async function cartScript(label, map, script, at, { rules = '', pre = '' } = {}) {
  const s = new Session(label);
  try {
    await boot(s, map, rules);
    await s.command(`setviewpos ${at.x} ${at.y} ${at.z} ${at.yaw}`);
    await s.step(60);
    if (pre) { await s.command(pre); await s.step(40); }
    const v0 = await readValues(s);
    const before = await s.read('trace_count');
    await s.command(`padscript padscripts/${script}.pad`);
    await s.step(SIM_SCRIPTS[script].reduce((k, st) => k + st.frames, 0) + 30);
    const after = await s.read('trace_count');
    if (after - before <= 0 || after - before > TRACE_ROWS) throw new Error(`${label}: ${after - before} trace rows`);
    const flat = await s.read('trace');
    const rows = [];
    for (let i = before; i < after; i++) {
      const at0 = (i % TRACE_ROWS) * TRACE_COLS;
      const r = flat.slice(at0, at0 + TRACE_COLS);
      rows.push([r[8], ...[r[1], r[2], r[3], r[4], r[5], r[6]].map(Math.fround), r[7]]);
    }
    return { rows: fromStart(rows, await s.read('pad_start_time')), v0, v: await readValues(s) };
  } finally {
    await s.shutdown();
  }
}

function nativeScript(label, map, script, at, { set = {}, pre = '' } = {}) {
  const file = `${label}.txt`;
  const home = runNative(label, map, [
    'exec padscripts/padbinds.cfg', 'bot_enable 0', 'wait 120',
    `setviewpos ${at.x} ${at.y} ${at.z} ${at.yaw}`, 'wait 200',
    ...(pre ? [pre, 'wait 100'] : []),
    'debugvalues before.txt',
    `trace ${file}`,
    `padscript padscripts/${script}.pad "wait 8; trace stop; debugvalues after.txt; quit"`,
  ], { quit: false, set });
  const tfile = path.join(home, 'baseoa', file);
  if (!fs.existsSync(tfile)) return null;
  const nr = nativeRows(tfile);
  const read = (f) => (fs.existsSync(path.join(home, 'baseoa', f)) ? parseDebugValues(fs.readFileSync(path.join(home, 'baseoa', f), 'utf8')) : {});
  return { rows: fromStart(nr.rows, nr.start), v0: read('before.txt'), v: read('after.txt'), log: path.join(home, 'native.log') };
}

// the local player watches as a spectator ("team s" in its userinfo): a
// player in the match would be shot at, and its commands follow the
// client's own timing, which differs between builds
function matchLine(seed, tl) {
  return ['setu team s', `set sv_gameSeed ${seed}`, `set g_oaxTranslocator ${tl}`, 'bot_enable 1', 'g_gametype 0', 'g_doWarmup 0', 'timelimit 0', 'fraglimit 0',
    `devmap ${MAP}`, 'addbot Sarge 3', 'addbot Grism 3'].join(';');
}

function checkpoints(v) {
  return Object.fromEntries(Object.entries(v).filter(([k]) => /^g_navbot_hash_\d+$/.test(k)).map(([k, x]) => [Number(k.split('_').pop()), x]));
}

async function cartMatch(label, tl, frames) {
  const s = new Session(label);
  try {
    await s.load();
    await s.command(matchLine(7, tl));
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 4000, 20);
    for (let done = 0; done < frames; done += 750) await s.step(Math.min(750, frames - done));
    return await readValues(s);
  } finally {
    await s.shutdown();
  }
}

function nativeMatch(tl, frames) {
  const home = nativeHome('nav-intent-match');
  fs.writeFileSync(path.join(home, 'baseoa', 'navintent.cfg'), ['fixedtime 16', matchLine(7, tl), `wait ${frames * 2}`, 'debugvalues navintent_native.txt', 'quit'].join('\n') + '\n');
  execNative([
    '+set', 'fs_basepath', path.dirname(findBaseoa()), '+set', 'com_basegame', 'baseoa', '+set', 'fs_homepath', home,
    '+set', 'r_mode', '-1', '+set', 'r_customwidth', '640', '+set', 'r_customheight', '360', '+set', 'r_fullscreen', '0',
    '+set', 'vm_game', '1', '+set', 'vm_cgame', '1', '+set', 'vm_ui', '1', '+set', 'sv_pure', '0',
    // fixed time from the first frame (see lib/native.mjs)
    '+set', 'com_introplayed', '1', '+set', 'com_maxfps', '0', '+set', 'fixedtime', '16', '+exec', 'navintent.cfg',
  ], { home, timeout: 900000 });
  return parseDebugValues(fs.readFileSync(path.join(home, 'baseoa', 'navintent_native.txt'), 'utf8'));
}

export async function run({ out }) {
  const failures = [];
  const rows = [];

  // ---- 1. teleporter traces --------------------------------------------------
  {
    const nat = nativeScript('navtele', MAP, 'nav_tele', TELE);
    const cart = await cartScript('nav-intent-tele', MAP, 'nav_tele', TELE);
    if (!nat || !nat.rows || !cart.rows) {
      failures.push(`teleporter: no movement in the ${!nat || !nat.rows ? 'native' : 'cart'} trace`);
    } else {
      fs.writeFileSync(path.join(out, 'navtele.cart.json'), JSON.stringify([...cart.rows.entries()]));
      const { common, firstDiff, maxErr } = compare(nat.rows, cart.rows);
      const ports = n(cart.v.g_teleports_0) - n(cart.v0.g_teleports_0);
      const nports = n(nat.v.g_teleports_0) - n(nat.v0.g_teleports_0);
      const zs = [...cart.rows.values()].map((r) => r[3]);
      const onTower = zs.filter((z) => z > TOWER_TOP + 20).length;
      const last = [...cart.rows.values()].pop();
      rows.push(`teleporter pair (noretrigger): ${common} common command times (native ${nat.rows.size}, cart ${cart.rows.size}), max difference ${maxErr}; ` +
        `teleports cart ${ports} native ${nports}; ${onTower} rows on the tower; ends at ${last[1].toFixed(1)} ${last[2].toFixed(1)} ${last[3].toFixed(1)}`);
      if (common < nat.rows.size) failures.push(`teleporter: only ${common} of native's ${nat.rows.size} command times recorded by the cart`);
      if (common < 100) failures.push(`teleporter: only ${common} command times in common`);
      if (firstDiff) failures.push(`teleporter: native and cart differ at ${firstDiff}`);
      if (ports !== 2 || nports !== 2) failures.push(`teleporter: expected exactly 2 teleports (in, back), cart ${ports} native ${nports}`);
      if (!(onTower > 20)) failures.push(`teleporter: the player never stood on the tower (${onTower} rows)`);
      if (!(last[3] < 40 && (last[2] < -559 || last[2] > -465))) failures.push('teleporter: the player did not end on the ground outside trigger A');
    }
    const ctl = await cartScript('nav-intent-classic', CLASSIC, 'nav_tele', TELE);
    const cports = n(ctl.v.g_teleports_0) - n(ctl.v0.g_teleports_0);
    rows.push(`control, classic teleporters (${CLASSIC}): ${cports} teleports in the same script, links ${ctl.v.sv_nav_links}, teleporters left out as ping-pong ${ctl.v.g_nav_links_pingpong}`);
    if (!(cports > 10)) failures.push(`control: classic teleporters did not ping-pong (${cports} teleports)`);
    if (n(ctl.v.g_nav_links_pingpong) !== 2) failures.push(`control: the classic pair should give no teleporter links (pingpong ${ctl.v.g_nav_links_pingpong})`);
  }

  // ---- 2. links and routes -------------------------------------------------------
  const cartRoutes = [];
  let cv = null;
  {
    const s = new Session('nav-intent-routes');
    try {
      await boot(s, MAP);
      for (const r of ROUTES) {
        await s.command(navPathCmd(r));
        await s.step(2);
        const v = await readValues(s);
        cartRoutes.push(`${v.nav_path} | ${v.nav_path_links}`);
        cv = v;
        const p = parsePath(v.nav_path);
        const kinds = linkKinds(v.nav_path_links, v);
        const got = p.count === 0 || p.partial ? 'none' : (kinds[0] || 'walk');
        rows.push(`route ${r[0]}: ${p.count} points${p.partial ? ' (partial)' : ''}, links [${kinds.join(', ')}] -> ${got}`);
        if (got !== r[4]) failures.push(`route ${r[0]}: expected ${r[4]}, got ${got}`);
      }
    } finally {
      await s.shutdown();
    }
    const kindsAuthored = Object.keys(cv).filter((k) => /^g_nav_link_\d+$/.test(k)).map((k) => cv[k].split(' ')[0]);
    rows.push(`navmesh ${cv.sv_nav_polys} polys, hash ${cv.sv_nav_hash}, links ${cv.sv_nav_links} (${kindsAuthored.join(' ')}), not connected: ${cv.sv_nav_links_open}, hazard volumes ${cv.g_nav_hazards}`);
    const [ok, total] = String(cv.sv_nav_links).split('/').map(Number);
    if (!(total >= 8 && ok === total)) failures.push(`links: ${cv.sv_nav_links} connected (open: ${cv.sv_nav_links_open})`);
    for (const k of ['teleport', 'jumppad', 'ladder', 'translocator', 'drop']) if (!kindsAuthored.includes(k)) failures.push(`links: no ${k} link authored`);
    // the jump pad's landing comes from the real push: on the platform top
    const pad = Object.keys(cv).filter((k) => /^g_nav_link_\d+$/.test(k)).map((k) => cv[k].split(' ')).find((f) => f[0] === 'jumppad');
    if (pad && Math.abs(Number(pad[6]) - 256) > 2) failures.push(`jump pad link lands at z ${pad[6]}, not on the platform (256)`);

    // native: the same mesh and the same routes
    const home = runNative('navroutes', MAP, ['wait 100', ...ROUTES.flatMap((r) => [navPathCmd(r), 'wait 4']), 'debugvalues routes.txt']);
    const nv = parseDebugValues(fs.readFileSync(path.join(home, 'baseoa', 'routes.txt'), 'utf8'));
    const nlines = fs.readFileSync(path.join(home, 'native.log'), 'utf8').split('\n').filter((l) => l.startsWith('nav_path: ')).map((l) => l.slice(10));
    const cartPaths = cartRoutes.map((x) => x.split(' | ')[0]);
    const same = nlines.length === cartPaths.length && nlines.every((l, i) => l === cartPaths[i]);
    rows.push(`native: hash ${nv.sv_nav_hash}, links ${nv.sv_nav_links}; ${nlines.length} routes, ${same ? 'identical to the cart' : 'DIFFERENT from the cart'}`);
    if (nv.sv_nav_hash !== cv.sv_nav_hash) failures.push(`navmesh hash native ${nv.sv_nav_hash} vs cart ${cv.sv_nav_hash}`);
    if (!same) failures.push('routes differ native vs cart');

    // control: sv_navLinks 0 (plain walkable mesh)
    const c = new Session('nav-intent-nolinks');
    try {
      await boot(c, MAP, 'set sv_navLinks 0');
      const cut = [];
      for (const r of ROUTES.filter((x) => ['teleport', 'translocator', 'jumppad', 'ladder'].includes(x[4]))) {
        await c.command(navPathCmd(r));
        await c.step(2);
        const v = await readValues(c);
        const p = parsePath(v.nav_path);
        if (p.count === 0 || p.partial) cut.push(r[0]);
      }
      rows.push(`control sv_navLinks 0: links ${(await readValues(c)).sv_nav_links}, unreachable: ${cut.join('; ')}`);
      if (cut.length !== 4) failures.push(`control: with sv_navLinks 0 only ${cut.length} of 4 link-only items became unreachable`);
    } finally {
      await c.shutdown();
    }
  }

  // ---- 3. translocator --------------------------------------------------------
  {
    const pre = 'weapon 10';
    const nat = nativeScript('navtl', MAP, 'nav_tl', TL, { set: { g_oaxTranslocator: 1 }, pre });
    const cart = await cartScript('nav-intent-tl', MAP, 'nav_tl', TL, { rules: 'set g_oaxTranslocator 1', pre });
    if (!nat || !nat.rows || !cart.rows) {
      failures.push(`translocator: no movement in the ${!nat || !nat.rows ? 'native' : 'cart'} trace`);
    } else {
      const { common, firstDiff, maxErr } = compare(nat.rows, cart.rows);
      const first = [...cart.rows.values()][0], last = [...cart.rows.values()].pop();
      const moved = Math.hypot(last[1] - first[1], last[2] - first[2]);
      rows.push(`translocator throw + port: ${common} common command times, max difference ${maxErr}; throws ${cart.v.g_tl_throws} ports ${cart.v.g_tl_ports} (native ${nat.v.g_tl_throws}/${nat.v.g_tl_ports}); moved ${moved.toFixed(1)} units`);
      if (common < 50) failures.push(`translocator: only ${common} command times in common`);
      if (firstDiff) failures.push(`translocator: native and cart differ at ${firstDiff}`);
      if (n(cart.v.g_tl_throws) !== 1 || n(cart.v.g_tl_ports) !== 1) failures.push(`translocator: expected 1 throw and 1 port, got ${cart.v.g_tl_throws}/${cart.v.g_tl_ports}`);
      if (!(moved > 150)) failures.push(`translocator: the port moved the player only ${moved.toFixed(1)} units`);
    }
    const off = await cartScript('nav-intent-tl-off', MAP, 'nav_tl', TL, { rules: 'set g_oaxTranslocator 0', pre });
    const first = off.rows ? [...off.rows.values()][0] : null, last = off.rows ? [...off.rows.values()].pop() : null;
    const moved = first ? Math.hypot(last[1] - first[1], last[2] - first[2]) : 0;
    rows.push(`control g_oaxTranslocator 0: throws ${off.v.g_tl_throws ?? 0}, ports ${off.v.g_tl_ports ?? 0}, moved ${moved.toFixed(1)}`);
    if (n(off.v.g_tl_throws) || n(off.v.g_tl_ports) || moved > 1) failures.push('control: the translocator worked with the rule off');
  }

  // ---- 3b. telefrag: port onto a standing bot (bot_oaxIdle 1 keeps it still) ----
  {
    const tf = async (label, away) => {
      const s = new Session(label);
      try {
        await boot(s, MAP, 'set g_oaxTranslocator 1;set bot_oaxIdle 1');
        await s.command('addbot Sarge 1');
        await s.step(150);
        const v0 = await readValues(s);
        const key = Object.keys(v0).find((k) => /^g_navbot_pos_\d+$/.test(k));
        if (!key) throw new Error(`${label}: the bot published no position`);
        const [bx, by] = v0[key].split(' ').map(Number);
        // setviewpos slides the player TL_SLIDE forward; a level throw lands TL_THROW ahead
        const x = away ? bx + 40 : bx - TL_THROW - TL_SLIDE;
        await s.command(`setviewpos ${x.toFixed(2)} ${by.toFixed(2)} 24 ${away ? 180 : 0}`);
        await s.step(60);
        await s.command('weapon 10');
        await s.step(40);
        await s.command('padscript padscripts/nav_tl.pad');
        await s.step(SIM_SCRIPTS.nav_tl.reduce((k, st) => k + st.frames, 0) + 30);
        const v = await readValues(s);
        return { bot: v0[key], after: v[key], telefrags: n(v.g_tl_telefrags), ports: n(v.g_tl_ports) };
      } finally {
        await s.shutdown();
      }
    };
    const hit = await tf('nav-intent-telefrag', false);
    const miss = await tf('nav-intent-telefrag-away', true);
    const dead = (p) => Number(String(p).split(' ')[3]) <= 0;
    rows.push(`telefrag: bot at ${hit.bot}, port onto it: telefrags ${hit.telefrags}, ports ${hit.ports}, bot after "${hit.after}"; control (thrown away from it): telefrags ${miss.telefrags}, ports ${miss.ports}, bot after "${miss.after}"`);
    if (hit.telefrags !== 1 || hit.ports !== 1 || !dead(hit.after)) failures.push('telefrag: porting onto the bot did not telefrag it');
    if (miss.telefrags !== 0 || miss.ports !== 1 || dead(miss.after)) failures.push('control: a port away from the bot telefragged it (or did not port)');
  }

  // ---- 4. bots --------------------------------------------------------------
  {
    const v = await cartMatch('nav-intent-bots', 1, FRAMES);
    const nv = nativeMatch(1, FRAMES);
    fs.writeFileSync(path.join(out, 'nav-intent-bots.json'), JSON.stringify({ cart: v, native: nv }, null, 1));
    const kinds = Object.entries(KINDS).filter(([bit]) => n(v.g_navbot_link_kinds) & Number(bit)).map(([, k]) => k);
    rows.push(`bots, g_oaxTranslocator 1, ${n(v.g_level_time) / 1000}s: items touched ${v.g_items_reached}/${v.g_items_total}, routable ${v.g_items_routable}/${v.g_items_routed_total}; ` +
      `links taken ${v.g_navbot_links_taken}, done ${v.g_navbot_links_done}, failed ${v.g_navbot_links_failed}; kinds done: ${kinds.join(' ')}`);
    rows.push(`per kind (taken done failed): ${['teleport', 'jumppad', 'ladder', 'drop', 'translocator'].map((k) => `${k} ${v[`g_navbot_lk_${k}`]}`).join(', ')}; translocator throws ${v.g_tl_throws} ports ${v.g_tl_ports}`);
    if (!(n(v.g_items_total) === 5 && v.g_items_reached === v.g_items_total)) failures.push(`bots: items touched ${v.g_items_reached}/${v.g_items_total} (missing ${v.g_items_missing})`);
    if (n(v.g_items_routable) !== n(v.g_items_routed_total)) failures.push(`bots: unroutable items ${v.g_items_unroutable}`);
    for (const k of ['teleport', 'jumppad', 'ladder', 'drop', 'translocator']) if (!kinds.includes(k)) failures.push(`bots: no ${k} link completed`);
    const cc = checkpoints(v), nc = checkpoints(nv);
    const common = Object.keys(cc).filter((t) => nc[t] !== undefined).map(Number).sort((a, b) => a - b);
    const diff = common.filter((t) => cc[t] !== nc[t]);
    rows.push(`bots native vs cart: ${common.length} hash checkpoints in common, ${diff.length} differ${diff.length ? ` (first at ${diff[0]}s)` : ''}; native items ${nv.g_items_reached}/${nv.g_items_total}`);
    if (common.length < 5) failures.push(`bots: only ${common.length} checkpoints in common`);
    if (diff.length) failures.push(`bots: the match differs native vs cart from ${diff[0]}s`);
    if (new Set(common.map((t) => cc[t])).size < common.length - 1) failures.push('control: the bot hash did not move');

    const off = await cartMatch('nav-intent-bots-off', 0, FRAMES);
    const ledgeMissing = String(off.g_items_missing || '').includes('weapon_railgun');
    rows.push(`control g_oaxTranslocator 0: items touched ${off.g_items_reached}/${off.g_items_total} (missing ${off.g_items_missing}), routable ${off.g_items_routable}/${off.g_items_routed_total} (unroutable ${off.g_items_unroutable}); throws ${off.g_tl_throws ?? 0}`);
    if (!ledgeMissing) failures.push('control: the ledge item was touched with the translocator rule off');
    if (!String(off.g_items_unroutable || '').includes('weapon_railgun')) failures.push('control: the ledge item was routable with the translocator rule off');
    if (n(off.g_tl_throws)) failures.push('control: a beacon was thrown with the rule off');
  }
  return { ok: failures.length === 0, failures, rows };
}
