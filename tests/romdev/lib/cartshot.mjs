// cartshot.mjs: the cart side of nativeshot.mjs. One romdev session loads
// a map, then runs shots in order: { cmd, settle, name, values } runs cmd,
// steps `settle` frames, then takes screenshot `name` (full size, decoded)
// and/or reads the debug values into valuesAt[values], and/or the debug blob
// (a surface id dump, r_oaxSurfaceIdDump) into blobs[blob].

import path from 'node:path';
import { Session } from './romdev.mjs';
import { loadScene, CLEAN_VIEW } from './scenes.mjs';
import { readValues } from './values.mjs';
import { readPng } from './png.mjs';

export async function cartShots(tag, map, shots, { setup = [], settle = 20, out, seed = 1, picmip } = {}) {
  const s = new Session(tag);
  const images = {};
  const valuesAt = {};
  const blobs = {};
  try {
    await loadScene(s, map, { seed, view: [CLEAN_VIEW, ...setup].join(';'), picmip });
    for (const sh of shots) {
      if (sh.cmd) await s.command(sh.cmd);
      if ((sh.settle ?? settle) > 0) await s.step(sh.settle ?? settle);
      if (sh.name) {
        const f = path.join(out, `${tag}_${sh.name}.png`);
        await s.screenshot(f);
        images[sh.name] = readPng(f);
      }
      if (sh.values) valuesAt[sh.values] = await readValues(s);
      if (sh.blob) blobs[sh.blob] = String(await s.read('debug_blob'));
    }
    return { images, valuesAt, blobs, values: await readValues(s) };
  } finally {
    await s.shutdown();
  }
}
