// Engine-extension plumbing, end to end on both builds: the engine
// advertises oax_features, the oax QVMs (game and cgame) publish named debug
// values through their new syscalls, and the game reads the test map's
// OAX_MANIFEST BSPX lump. Control: a stock map has no manifest (-1).

import fs from 'node:fs';
import path from 'node:path';
import { Session, CA_ACTIVE, repoRoot } from '../lib/romdev.mjs';
import { runNative } from '../lib/native.mjs';
import { parseDebugValues, readValues } from '../lib/values.mjs';
import { readBspx } from '../../../misc/tools/bspx.mjs';

export const name = 'oax-infra';

async function cartValues(map) {
  const s = new Session(`oax-infra-${map}`);
  try {
    await s.load();
    await s.command(`bot_enable 0; devmap ${map}`);
    await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
    await s.step(5);
    return await readValues(s);
  } finally {
    await s.shutdown();
  }
}

function nativeValues(map) {
  const home = runNative(`oax-infra-${map}`, map, ['wait 20', 'debugvalues values.txt']);
  const f = path.join(home, 'baseoa', 'values.txt');
  return fs.existsSync(f) ? parseDebugValues(fs.readFileSync(f, 'utf8')) : {};
}

export async function run() {
  const failures = [];
  const rows = [];
  const manifest = path.join(repoRoot, 'tests', 'maps', 'out', 'baseoa', 'maps', 'oax_box.bsp');
  if (!fs.existsSync(manifest)) return { ok: false, failures: ['oax_box not built (node tests/maps/build.mjs)'], rows };
  const lump = readBspx(fs.readFileSync(manifest)).lumps.find((l) => l.name === 'OAX_MANIFEST');

  for (const [build, get] of [['cart', cartValues], ['native', async (m) => nativeValues(m)]]) {
    const box = await get('oax_box');
    const stock = await get('oa_dm1');
    rows.push(`${build}: g_oax ${box.g_oax} cg_oax ${box.cg_oax} manifest ${box.g_manifest_len} bytes (file ${lump.data.length}); oa_dm1 manifest ${stock.g_manifest_len}`);
    if (box.g_oax !== '1') failures.push(`${build}: game QVM published no g_oax (got ${box.g_oax})`);
    if (box.cg_oax !== '1') failures.push(`${build}: cgame QVM published no cg_oax (got ${box.cg_oax})`);
    if (Number(box.g_manifest_len) !== lump.data.length) failures.push(`${build}: game read a ${box.g_manifest_len}-byte manifest, the file has ${lump.data.length}`);
    if (stock.g_manifest_len !== '-1') failures.push(`${build}: control: oa_dm1 reported a manifest (${stock.g_manifest_len})`);
  }
  return { ok: failures.length === 0, failures, rows };
}
