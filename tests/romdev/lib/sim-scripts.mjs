// sim-scripts.mjs: pad scripts for the zone and warp tests (same format as
// SCRIPTS in movement.mjs; data/make-padscripts.mjs writes both as .pad).

const idle = (frames) => ({ frames, pad: {} });
const fwd = (frames, extra = {}) => ({ frames, pad: { axes: { ly: -1 }, ...extra } });

export const SIM_SCRIPTS = {
  // stand still, one jump (apex in a gravity zone, drift in a current)
  zjump: [idle(10), { frames: 2, pad: { south: true } }, idle(200)],
  // oax_zones from spawn 0: a low-gravity running jump that carries through
  // the current zone, then on into the fog room and a jump there
  ztour: [idle(20), fwd(2, { south: true }), fwd(300), fwd(2, { south: true }), fwd(60), idle(30)],
  // oax_warp: run down corridor A (or C) through the warp (or teleporter)
  wrun: [idle(10), fwd(220), idle(20)],
};

// oax_terrain movement (terrain-parity): runs, strafes and jumps over
// heightmap terrain, into a slope too steep to climb, and off a cliff.
const jump = (frames = 2, axes = { ly: -1 }) => ({ frames, pad: { axes, south: true } });
SIM_SCRIPTS.terrain_run = [
  idle(10), fwd(90),
  { frames: 40, pad: { axes: { ly: -1, lx: 1, rx: 0.3 } } },
  jump(), fwd(40), jump(), fwd(40), jump(2, { ly: -1, lx: -1 }), { frames: 40, pad: { axes: { ly: -1, lx: -1, rx: -0.3 } } },
  { frames: 60, pad: { axes: { ly: -1, rx: -0.4 } } },
  { frames: 40, pad: { axes: { ly: 1 } } },
  idle(30),
];
SIM_SCRIPTS.terrain_bank = [
  idle(10), fwd(110), jump(), fwd(40), jump(), fwd(30),
  { frames: 60, pad: { axes: { ly: -1, lx: 1 } } },
  { frames: 50, pad: { axes: { lx: -1 } } },
  idle(40),
];
SIM_SCRIPTS.terrain_cliff = [
  idle(10), fwd(60), jump(), fwd(80),
  { frames: 40, pad: { axes: { ly: -1, rx: 1 } } },
  fwd(60), idle(40),
];
