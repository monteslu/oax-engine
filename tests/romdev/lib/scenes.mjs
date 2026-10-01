// scenes.mjs: the setups the tests share. A scene is a map loaded with cheats
// (so setviewpos works), no bots and no HUD, then a player placed somewhere.

import path from 'node:path';
import { bootMap, defaultCart } from './romdev.mjs';
import { spawnPoints } from './bsp.mjs';

export const CLEAN_VIEW = [
  'bot_enable 0',
  'cg_drawGun 0',
  'cg_draw2D 0',
  'cg_drawFPS 0',
  'g_doWarmup 0',
  'con_notifytime 0',
].join(';');

export function mapPath(map, cart = defaultCart) {
  return path.join(cart, 'assets', 'baseoa', 'maps', `${map}.bsp`);
}

export async function loadScene(s, map, { seed = 1, view = CLEAN_VIEW } = {}) {
  await s.load(defaultCart, seed);
  await s.command(`${view};devmap ${map}`);
  const { CA_ACTIVE } = await import('./romdev.mjs');
  await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
  // cgame registers its own cvars when the map loads; set the view again
  // once it has, and let it settle
  await s.command(view);
  await s.step(60);
}

// Put the player at a spawn point (feet on its origin) facing its angle.
export async function placeAt(s, p, settle = 30) {
  await s.command(`setviewpos ${p.x} ${p.y} ${p.z} ${p.yaw}`);
  await s.step(settle);
}

export function spawns(map) {
  return spawnPoints(mapPath(map));
}

export { bootMap };
