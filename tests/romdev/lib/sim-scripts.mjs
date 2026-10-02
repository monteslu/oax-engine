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
