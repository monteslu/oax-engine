// movement.mjs: scripted input against a placed player, returning the
// per-frame trace the cart records (origin, velocity, ground, command time).

import { loadScene, placeAt, spawns } from './scenes.mjs';

export const TRACE_ROWS = 4096;
export const TRACE_COLS = 10;
export const COLS = ['frame', 'x', 'y', 'z', 'vx', 'vy', 'vz', 'ground', 'cmdTime', 'yaw'];

// Each step holds a pad state for some frames. Sticks are -1..1 (ly -1 = forward).
export const SCRIPTS = {
  // run forward, strafe, jump while running, turn, back up
  basic: [
    { frames: 60, pad: { axes: { ly: -1 } } },
    { frames: 30, pad: { axes: { ly: -1, lx: 1 } } },
    { frames: 2, pad: { axes: { ly: -1 }, south: true } },
    { frames: 50, pad: { axes: { ly: -1 } } },
    { frames: 30, pad: { axes: { rx: 1 } } },
    { frames: 40, pad: { axes: { ly: 1 } } },
    { frames: 30, pad: {} },
  ],
  // strafe-jump chain: forward + strafe + repeated jumps while turning
  strafejump: [
    { frames: 20, pad: { axes: { ly: -1 } } },
    ...Array.from({ length: 6 }, (_, i) => [
      { frames: 2, pad: { axes: { ly: -1, lx: i % 2 ? -1 : 1, rx: i % 2 ? -0.3 : 0.3 }, south: true } },
      { frames: 28, pad: { axes: { ly: -1, lx: i % 2 ? -1 : 1, rx: i % 2 ? -0.3 : 0.3 } } },
    ]).flat(),
    { frames: 40, pad: {} },
  ],
};

// basic with the strafe reversed: the must-differ control for determinism
SCRIPTS.basic_control = SCRIPTS.basic.map((st, k) => (k === 1 ? { ...st, pad: { axes: { ly: -1, lx: -1 } } } : st));

// Level time pad scripts start at in the movement tests, on every build:
// past the time either build needs to load the map and place the player.
export const PADSCRIPT_MIN_START = 8000;

export async function runScript(s, map, name, { seed = 1, spawn = 0 } = {}) {
  const script = SCRIPTS[name];
  await loadScene(s, map, { seed });
  const p = spawns(map)[spawn];
  await placeAt(s, p);
  const before = await s.read('trace_count');
  // played by the engine from tests/romdev/data/padscripts (packed into the
  // cart), the same file the native build plays
  // the script starts at level time PADSCRIPT_MIN_START (both builds reach
  // the map at different level times; see cl_testscript.c)
  await s.command(`set padscript_minstart ${PADSCRIPT_MIN_START}; padscript padscripts/${name}.pad`);
  await s.stepUntil('pad_start_time', (v) => v >= 0, 2000, 4);
  await s.step(script.reduce((n, st) => n + st.frames, 0) + 30);
  const after = await s.read('trace_count');
  const n = after - before;
  if (n <= 0) throw new Error('no trace rows recorded (is a player state present?)');
  if (n > TRACE_ROWS) throw new Error(`script ran ${n} rows, more than the ${TRACE_ROWS}-row ring`);
  const flat = await s.read('trace');
  const rows = [];
  for (let i = before; i < after; i++) {
    const at = (i % TRACE_ROWS) * TRACE_COLS;
    rows.push(flat.slice(at, at + TRACE_COLS));
  }
  return { start: p, padStart: await s.read('pad_start_time'), rows };
}

// Compare two traces row by row; returns the first row that differs.
export function compareTraces(a, b, eps = 0) {
  if (a.length !== b.length) return { equal: false, reason: `length ${a.length} vs ${b.length}` };
  for (let i = 0; i < a.length; i++) {
    for (let c = 1; c < TRACE_COLS; c++) {   // column 0 is the frame counter, which may be offset
      if (Math.abs(a[i][c] - b[i][c]) > eps) {
        return { equal: false, reason: `row ${i} ${COLS[c]}: ${a[i][c]} vs ${b[i][c]}`, row: i };
      }
    }
  }
  return { equal: true };
}

export function summarize(rows) {
  const first = rows[0], last = rows[rows.length - 1];
  const dist = Math.hypot(last[1] - first[1], last[2] - first[2]);
  const maxZ = Math.max(...rows.map((r) => r[3]));
  const airborne = rows.filter((r) => r[7] === -1).length;
  const topSpeed = Math.max(...rows.map((r) => Math.hypot(r[4], r[5])));
  return { frames: rows.length, distance: dist, rise: maxZ - first[3], airborneFrames: airborne, topSpeed };
}
