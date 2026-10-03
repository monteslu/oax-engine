// oax_nav_intent: navigation from authored intent (docs/navigation.md).
// One closed hall with no AAS (the navmesh bots run) and five places only
// intent can route to:
//
//   tower        a 256-high pillar reached only by a teleporter pair
//                whose arrivals sit INSIDE the partner trigger
//                ("noretrigger 1", no-retrigger arrival)
//   hazard       a trigger_hurt (dmg 5) over a health item: a cost volume,
//                not lava, so the item stays routable
//   ledge        192 high, reachable only by translocator (info_oax_route
//                kind translocator); a drop route leads back down
//   platform     256 high, reached by a jump pad whose landing is flown
//                from the pad's real push velocity; a drop route back
//   ladder ledge 160 high with a ladder volume (func_oax_zone "ladder")
//                on its face; a drop route back
//
// oax_nav_intent_classic is the same map with classic teleporters (no
// noretrigger): the control that must ping-pong.

import { MapFile, room, box } from '../mapwriter.mjs';

const SOLID = { top: 'base_floor/clang_floor', sides: 'gothic_block/blocks18c' };

export const NAV_INTENT = {
  teleStart: { x: -704, y: -700, z: 24, yaw: 90 },	// in front of trigger A, facing it
  triggerA: { lo: [-736, -544, 0], hi: [-672, -480, 96] },
  triggerB: { lo: [-736, 280, 256], hi: [-672, 344, 352] },
  towerTop: 256,
  items: {
    tower: [-704, 470, 280],
    hazard: [384, -512, 24],
    ledge: [880, 560, 216],
    platform: [0, 700, 280],
    ladder: [-320, -700, 184],
  },
  spawn: [0, 0, 32],
};

export function buildNavIntent({ noretrigger = true, message = 'oax test: navigation from intent' } = {}) {
  const map = new MapFile({ message });
  map.brush(room([-1024, -768, 0], [1024, 768, 512], { floor: 'base_floor/clang_floor', ceiling: 'base_floor/concrete', walls: 'gothic_wall/oct20c' }));

  // tower: teleporters only
  map.brush(box([-832, 256, 0], [-576, 512, 256], SOLID));
  const tele = noretrigger ? { noretrigger: 1 } : {};
  map.entity('trigger_teleport', { target: 'tele_up', ...tele }, [box(NAV_INTENT.triggerA.lo, NAV_INTENT.triggerA.hi, 'common/trigger')]);
  map.entity('trigger_teleport', { target: 'tele_down', ...tele }, [box(NAV_INTENT.triggerB.lo, NAV_INTENT.triggerB.hi, 'common/trigger')]);
  // each arrival inside the partner trigger (no-retrigger arrival)
  map.entity('misc_teleporter_dest', { targetname: 'tele_up', origin: [-704, 312, 280], angle: 90 });
  map.entity('misc_teleporter_dest', { targetname: 'tele_down', origin: [-704, -512, 24], angle: 270 });
  map.entity('item_armor_combat', { origin: NAV_INTENT.items.tower });

  // hazard over an item
  map.entity('trigger_hurt', { dmg: 5 }, [box([256, -640, 0], [512, -384, 96], 'common/trigger')]);
  map.entity('item_health_large', { origin: NAV_INTENT.items.hazard });

  // translocator-only ledge, with a drop route back down
  map.brush(box([640, 256, 0], [1024, 768, 192], SOLID));
  map.entity('weapon_railgun', { origin: NAV_INTENT.items.ledge });
  map.entity('info_oax_route', { origin: [200, 512, 8], target: 'ledge_top', kind: 'translocator' });
  map.entity('info_oax_route', { origin: [760, 512, 200], targetname: 'ledge_top' });
  map.entity('info_oax_route', { origin: [700, 400, 200], target: 'ledge_down', kind: 'drop' });
  map.entity('info_oax_route', { origin: [560, 400, 8], targetname: 'ledge_down' });

  // jump pad platform
  map.brush(box([-160, 576, 0], [160, 768, 256], SOLID));
  map.entity('trigger_push', { target: 'pad_apex' }, [box([-48, 352, 0], [48, 448, 16], 'common/trigger')]);
  map.entity('target_position', { targetname: 'pad_apex', origin: [0, 660, 330] });
  map.entity('weapon_rocketlauncher', { origin: NAV_INTENT.items.platform });
  map.entity('info_oax_route', { origin: [130, 610, 264], target: 'pad_down', kind: 'drop' });
  map.entity('info_oax_route', { origin: [200, 480, 8], targetname: 'pad_down' });

  // ladder ledge
  map.brush(box([-448, -768, 0], [-192, -576, 160], SOLID));
  map.entity('func_oax_zone', { ladder: 200 }, [box([-352, -576, 0], [-288, -536, 224], 'common/trigger')]);
  map.entity('item_health_mega', { origin: NAV_INTENT.items.ladder });
  map.entity('info_oax_route', { origin: [-250, -600, 168], target: 'ladder_down', kind: 'drop' });
  map.entity('info_oax_route', { origin: [-250, -480, 8], targetname: 'ladder_down' });

  for (const [x, y, a] of [[0, 0, 0], [-400, 0, 0], [400, 0, 180], [0, -300, 90]]) map.entity('info_player_deathmatch', { origin: [x, y, 32], angle: a });
  for (const [x, y] of [[-600, -400], [-600, 400], [0, 0], [600, -400], [600, 400], [0, 600]]) map.entity('light', { origin: [x, y, 420], light: 700 });
  return map;
}

export function build() {
  return { map: buildNavIntent(), aas: false, manifest: { features: ['nav_intent'] } };
}
