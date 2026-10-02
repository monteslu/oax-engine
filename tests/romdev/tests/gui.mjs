// In-world GUIs (phase 4, design 2.7) on the cart and the native build.
//
// oax_gui has a door panel (func_oax_gui, guis/oax_door_panel.gui) wired to
// a func_door. The player stands 44 units from the panel:
//   - aimed at OPEN, fire: the server's GUI state g_gui0_state goes 0 -> 1,
//     the door starts moving (mover state 2, MOVER_1TO2) and no shot is
//     fired (ammo unchanged): BUTTON_ATTACK was the GUI's click;
//   - control: aimed at the wall beside the panel, fire: the state and the
//     door stay put and a shot IS fired (so the input really happened);
//   - the GUI event log (focus, click position, commands, state sets) is
//     identical on native and the cart; control: aiming at LOCK instead
//     gives a different log (state 2, runScript);
//   - goldens of the panel before and after the click (the label changes;
//     control: before vs the after golden must differ), and the text is
//     crisp: hard edges in the title, against a 64-pixel GUI target
//     (r_guiSize 64) that must fail the same check.

import fs from 'node:fs';
import path from 'node:path';
import { Session } from '../lib/romdev.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues, readValues } from '../lib/values.mjs';
import { comparePng, distinctColors, readPng, writePng } from '../lib/png.mjs';
import { loadScene } from '../lib/scenes.mjs';

export const name = 'gui';

const MAP = 'oax_gui';
// player spots (feet), facing west at the panel (tests/maps/src/oax_gui.mjs)
const AIM_OPEN = '-460 -28 24 180';
const AIM_LOCK = '-460 28 24 180';
const AIM_WALL = '-460 -200 24 180';
// a camera 160 units in front of the panel's center: the 128 x 96 panel
// covers 40% of the frame's width, about one screen pixel per GUI texel
const CAMERA = '-344 0 56 0 180 0';
const PANEL_HALF_W = 64, PANEL_HALF_H = 48, CAMERA_DIST = 160;
const GOLDEN_TOLERANCE = 24;
const MAX_BAD_FRACTION = 0.002;  // a deterministic replay of the same frame
const LABEL_MIN_CHANGE = 0.05;   // the state label must change this much (control)
const CRISP_MIN = 0.55;          // strongest title edges, as a fraction of the text contrast

// The panel's rectangle on screen: horizontal FOV 90, view centered on it.
export function panelRect(img) {
  const f = img.width / 2;
  const hw = Math.round((PANEL_HALF_W / CAMERA_DIST) * f);
  const hh = Math.round((PANEL_HALF_H / CAMERA_DIST) * f);
  const cx = img.width >> 1, cy = img.height >> 1;
  return { x: cx - hw, y: cy - hh, w: hw * 2, h: hh * 2 };
}

export function crop(img, r) {
  const data = Buffer.alloc(r.w * r.h * 4);
  for (let y = 0; y < r.h; y++) img.data.copy(data, y * r.w * 4, ((r.y + y) * img.width + r.x) * 4, ((r.y + y) * img.width + r.x + r.w) * 4);
  return { width: r.w, height: r.h, data };
}

// A rectangle of the GUI's 640x480 screen inside a panel crop.
function guiCrop(panel, x, y, w, h) {
  const sx = panel.width / 640, sy = panel.height / 480;
  return crop(panel, { x: Math.round(x * sx), y: Math.round(y * sy), w: Math.round(w * sx), h: Math.round(h * sy) });
}

// The title text ("DOOR CONTROL", GUI rect 20,24 600x56) inside a panel crop.
export function titleCrop(panel) {
  return guiCrop(panel, 120, 28, 400, 48);
}

// The state label (GUI rect 20,100 600x56): CLOSED before, OPENING after.
function labelCrop(panel) {
  return guiCrop(panel, 120, 104, 400, 48);
}

// Text crispness: the strongest steps between neighbouring pixels across
// the title, relative to its text-to-background contrast. Crisp text steps
// from background to glyph within a pixel or two; a magnified small target
// smears every edge over several.
export function crispness(img) {
  const L = (i) => 0.299 * img.data[i * 4] + 0.587 * img.data[i * 4 + 1] + 0.114 * img.data[i * 4 + 2];
  let lo = 255, hi = 0;
  const steps = [];
  for (let y = 0; y < img.height; y++) {
    for (let x = 0; x < img.width; x++) {
      const l = L(y * img.width + x);
      lo = Math.min(lo, l);
      hi = Math.max(hi, l);
      if (x > 0) steps.push(Math.abs(l - L(y * img.width + x - 1)));
    }
  }
  steps.sort((a, b) => b - a);
  const top = steps.slice(0, 40);
  const mean = top.reduce((a, b) => a + b, 0) / top.length;
  return { contrast: hi - lo, edge: hi > lo ? mean / (hi - lo) : 0, colors: distinctColors(img) };
}

function logOf(values) {
  const n = Number(values.g_guilog_n || 0);
  return Array.from({ length: n }, (_, i) => values[`g_guilog_${i}`]);
}

// ---- cart ----------------------------------------------------------------

async function cartFire(s, aim) {
  await s.command(`cl_overrideView "";setviewpos ${aim}`);
  await s.step(60);
  await s.command('+attack');
  await s.step(20);
  await s.command('-attack');
  await s.step(40);
  return readValues(s);
}

async function cartShot(s, file) {
  await s.command(`cl_overrideView "${CAMERA}"`);
  await s.step(20);
  await s.screenshot(file);
  await s.command('cl_overrideView ""');
  await s.step(2);
  return readPng(file);
}

async function cartRun(out) {
  const s = new Session('gui');
  try {
    await loadScene(s, MAP);
    const start = await readValues(s);

    // control: fire at the wall beside the panel
    const wall = await cartFire(s, AIM_WALL);

    // aim at OPEN, picture, fire, picture
    await s.command(`setviewpos ${AIM_OPEN}`);
    await s.step(60);
    const before = await cartShot(s, path.join(out, 'gui_cart_before.png'));
    const aimed = await readValues(s);
    const open = await cartFire(s, AIM_OPEN);
    const after = await cartShot(s, path.join(out, 'gui_cart_after.png'));

    // crispness control: the same view through a 64-pixel GUI target
    await s.command('r_guiSize 64');
    await s.step(2);
    const blurred = await cartShot(s, path.join(out, 'gui_cart_size64.png'));
    await s.command('r_guiSize 512');
    return { start, wall, aimed, open, before, after, blurred };
  } finally {
    await s.shutdown();
  }
}

async function cartLockLog() {
  const s = new Session('gui-lock');
  try {
    await loadScene(s, MAP);
    return await cartFire(s, AIM_LOCK);
  } finally {
    await s.shutdown();
  }
}

// ---- native ----------------------------------------------------------------

export function tgaToImage(file) {
  const b = fs.readFileSync(file);
  const idLen = b[0], type = b[2], w = b.readUInt16LE(12), h = b.readUInt16LE(14), bpp = b[16] >> 3, desc = b[17];
  if (type !== 2) throw new Error(`${file}: TGA type ${type}`);
  const data = Buffer.alloc(w * h * 4);
  let p = 18 + idLen;
  for (let i = 0; i < w * h; i++, p += bpp) {
    const row = desc & 0x20 ? Math.floor(i / w) : h - 1 - Math.floor(i / w);
    const o = (row * w + (i % w)) * 4;
    data[o] = b[p + 2]; data[o + 1] = b[p + 1]; data[o + 2] = b[p]; data[o + 3] = 255;
  }
  return { width: w, height: h, data };
}

export function nativeRun(label, aim, out, { shots = false } = {}) {
  const view = ['bot_enable 0', 'cg_drawGun 0', 'cg_draw2D 0', 'cg_drawFPS 0', 'g_doWarmup 0', 'con_notifytime 0', 'r_fixedShaderTime 100', 'wait 100'];
  const fire = (a) => [`cl_overrideView ""`, `setviewpos ${a}`, 'wait 120', '+attack', 'wait 40', '-attack', 'wait 80'];
  const shot = (n) => [`cl_overrideView "${CAMERA}"`, 'wait 40', `screenshot ${n}`, 'wait 4', 'cl_overrideView ""', 'wait 4'];
  const lines = shots
    ? [...view, ...fire(AIM_WALL), 'debugvalues wall.txt', `setviewpos ${aim}`, 'wait 120', ...shot('gui_before'),
      ...fire(aim), ...shot('gui_after'), 'debugvalues values.txt']
    : [...view, ...fire(aim), 'debugvalues values.txt'];
  const home = runNative(`gui-${label}`, MAP, lines);
  const game = path.join(home, 'baseoa');
  const read = (f) => (fs.existsSync(path.join(game, f)) ? parseDebugValues(fs.readFileSync(path.join(game, f), 'utf8')) : null);
  const r = { values: read('values.txt'), wall: read('wall.txt') };
  if (shots) {
    for (const n of ['gui_before', 'gui_after']) {
      const f = path.join(game, 'screenshots', `${n}.tga`);
      if (fs.existsSync(f)) {
        r[n] = tgaToImage(f);
        writePng(path.join(out, `${n.replace('gui_', 'gui_native_')}.png`), r[n]);
      }
    }
  }
  return r;
}

// ---- checks ----------------------------------------------------------------

export async function run({ goldens, out, update }) {
  const failures = [];
  const rows = [];
  const fail = (m) => failures.push(m);

  const c = await cartRun(out);
  rows.push(`cart start: gui_count ${c.start.g_gui_count}, g_gui0_state "${c.start.g_gui0_state}", door state ${c.start.g_gui0_target_state}`);
  if (c.start.g_gui_count !== '1') fail(`cart: ${c.start.g_gui_count} GUIs loaded, want 1`);
  if (c.start.g_gui0_state !== '0') fail(`cart: initial g_gui0_state "${c.start.g_gui0_state}", want "0"`);

  // control: firing at the wall fires and changes nothing
  rows.push(`cart wall (control): state "${c.wall.g_gui0_state}", door ${c.wall.g_gui0_target_state}, ammo ${c.start.g_p0_ammo} -> ${c.wall.g_p0_ammo}, log ${c.wall.g_guilog_n || 0} rows`);
  if (c.wall.g_gui0_state !== '0') fail('cart control: firing at the wall changed the GUI state');
  if (c.wall.g_gui0_target_state !== '0') fail('cart control: firing at the wall moved the door');
  if (!(Number(c.wall.g_p0_ammo) < Number(c.start.g_p0_ammo))) fail('cart control: no shot was fired at the wall (the input never arrived)');
  if (c.wall.g_guilog_n) fail(`cart control: the GUI logged ${c.wall.g_guilog_n} events with nobody aiming at it`);

  // aimed at OPEN
  rows.push(`cart OPEN: state "${c.open.g_gui0_state}" status "${c.open.g_gui0_status}", door ${c.open.g_gui0_target_state} z ${c.open.g_gui0_target_z}, ammo ${c.aimed.g_p0_ammo} -> ${c.open.g_p0_ammo}, cg focus ${c.aimed.cg_gui_focus}`);
  if (c.open.g_gui0_state !== '1') fail(`cart: g_gui0_state "${c.open.g_gui0_state}" after the click, want "1"`);
  if (c.open.g_gui0_target_state !== '2' && c.open.g_gui0_target_state !== '1') fail(`cart: door mover state ${c.open.g_gui0_target_state}, want 2 (opening) or 1 (open)`);
  if (!(Number(c.open.g_gui0_target_z) > 0)) fail(`cart: the door did not rise (z ${c.open.g_gui0_target_z})`);
  if (c.open.g_p0_ammo !== c.aimed.g_p0_ammo) fail(`cart: the click fired the weapon (ammo ${c.aimed.g_p0_ammo} -> ${c.open.g_p0_ammo})`);
  if (c.aimed.cg_gui_focus !== '1') fail(`cart: the client did not show focus (cg_gui_focus ${c.aimed.cg_gui_focus})`);
  const cartLog = logOf(c.open);
  rows.push(`cart log: ${cartLog.join(' | ')}`);

  // goldens of the panel, before and after
  const panels = {};
  for (const [k, img] of [['before', c.before], ['after', c.after], ['size64', c.blurred]]) {
    panels[k] = crop(img, panelRect(img));
    writePng(path.join(out, `gui_panel_${k}.png`), panels[k]);
  }
  const gdir = path.join(goldens, 'gui');
  for (const k of ['before', 'after']) {
    const golden = path.join(gdir, `panel_${k}.png`);
    if (update || !fs.existsSync(golden)) {
      fs.mkdirSync(gdir, { recursive: true });
      writePng(golden, panels[k]);
      rows.push(`golden panel_${k}: ${update ? 'updated' : 'created'} (${distinctColors(panels[k])} colours)`);
      continue;
    }
    const r = comparePng(readPng(golden), panels[k], { tolerance: GOLDEN_TOLERANCE, diffPath: path.join(out, `gui_panel_${k}.diff.png`) });
    rows.push(`cart panel ${k} vs golden: ${(r.badFraction * 100).toFixed(3)}% over tolerance, mean ${r.meanDiff?.toFixed(2)}`);
    if (!r.sameSize || r.badFraction > MAX_BAD_FRACTION) fail(`cart panel ${k}: ${(r.badFraction * 100).toFixed(2)}% differs from the golden`);
  }
  {
    // control: the click must show; the state label of the before picture
    // against the after golden differs (the rest of the panel barely does)
    const r = comparePng(labelCrop(readPng(path.join(gdir, 'panel_after.png'))), labelCrop(panels.before), { tolerance: GOLDEN_TOLERANCE });
    rows.push(`control: state label before vs the after golden: ${(r.badFraction * 100).toFixed(1)}% differ`);
    if (r.badFraction < LABEL_MIN_CHANGE) fail('control: the state label did not change with the click');
  }

  // crisp text, and the 64-pixel target that must not pass
  const crisp = crispness(titleCrop(panels.before));
  const blur = crispness(titleCrop(panels.size64));
  rows.push(`title text r_guiSize 512: edge ${crisp.edge.toFixed(2)} of contrast ${crisp.contrast.toFixed(0)}, ${crisp.colors} colours; r_guiSize 64 (control): edge ${blur.edge.toFixed(2)}, ${blur.colors} colours`);
  if (crisp.contrast < 100) fail(`title text missing (contrast ${crisp.contrast.toFixed(0)})`);
  if (crisp.edge < CRISP_MIN) fail(`title text is not crisp (edge ${crisp.edge.toFixed(2)} < ${CRISP_MIN})`);
  if (blur.edge >= CRISP_MIN) fail(`control: a 64-pixel GUI target passed the crispness check (edge ${blur.edge.toFixed(2)})`);

  // native: the same steps
  const n = nativeRun('open', AIM_OPEN, out, { shots: true });
  if (!n.values || !n.wall) {
    fail('native: no debug values written');
  } else {
    rows.push(`native wall (control): state "${n.wall.g_gui0_state}", door ${n.wall.g_gui0_target_state}, log ${n.wall.g_guilog_n || 0} rows`);
    rows.push(`native OPEN: state "${n.values.g_gui0_state}", door ${n.values.g_gui0_target_state} z ${n.values.g_gui0_target_z}`);
    if (n.wall.g_gui0_state !== '0' || n.wall.g_gui0_target_state !== '0' || n.wall.g_guilog_n) fail('native control: firing at the wall changed the GUI or the door');
    if (n.values.g_gui0_state !== '1') fail(`native: g_gui0_state "${n.values.g_gui0_state}" after the click, want "1"`);
    if (n.values.g_gui0_target_state !== '2' && n.values.g_gui0_target_state !== '1') fail(`native: door mover state ${n.values.g_gui0_target_state}`);
    const nativeLog = logOf(n.values);
    rows.push(`native log: ${nativeLog.join(' | ')}`);
    if (nativeLog.length < 4) fail(`native: only ${nativeLog.length} GUI log rows`);
    const same = nativeLog.length === cartLog.length && nativeLog.every((l, i) => l === cartLog[i]);
    rows.push(`GUI state log native vs cart: ${same ? 'IDENTICAL' : 'DIFFERENT'} (${cartLog.length} rows)`);
    if (!same) fail('the GUI state log differs between native and the cart');

    for (const k of ['before', 'after']) {
      const img = n[`gui_${k}`];
      if (!img) { fail(`native: no ${k} screenshot`); continue; }
      const panel = crop(img, panelRect(img));
      writePng(path.join(out, `gui_native_panel_${k}.png`), panel);
      const r = comparePng(panels[k], panel, { tolerance: 32 });
      rows.push(`native panel ${k} vs cart: ${(r.badFraction * 100).toFixed(3)}% over tolerance 32, mean ${r.meanDiff?.toFixed(2)}`);
      if (!r.sameSize || r.badFraction > 0.02) fail(`native panel ${k} differs from the cart's (${(r.badFraction * 100).toFixed(2)}%)`);
      if (k === 'before') {
        const nc = crispness(titleCrop(panel));
        rows.push(`native title text: edge ${nc.edge.toFixed(2)}, ${nc.colors} colours`);
        if (nc.edge < CRISP_MIN) fail(`native title text is not crisp (edge ${nc.edge.toFixed(2)})`);
      }
    }
  }

  // identity control: aiming at LOCK must give a different log
  const lock = await cartLockLog();
  const lockLog = logOf(lock);
  rows.push(`control: LOCK log: ${lockLog.join(' | ')}`);
  if (lock.g_gui0_state !== '2') fail(`LOCK: g_gui0_state "${lock.g_gui0_state}", want "2"`);
  if (lockLog.join('|') === cartLog.join('|')) fail('control: the LOCK run logged the same as the OPEN run');
  if (!lockLog.some((l) => l.includes('runScript oax_gui_lock'))) fail('LOCK: no runScript in the log');

  return { ok: failures.length === 0, failures, rows };
}
