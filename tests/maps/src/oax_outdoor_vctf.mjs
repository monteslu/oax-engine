// oax_outdoor_vctf: the vehicle CTF map (phase 8). The outdoor CTF field of
// oax_outdoor_ctf (heightmap valley, two stone forts, groves) with vehicle
// spawners on the flat ground in front of each fort: two buggies (driver
// and gunner) and one hover craft per team. The vehicles appear only under
// the vehicle rule (g_oaxVehicles 1); without it this is the classic CTF
// map. No AAS: bots path on the navmesh, on foot and driving.

import { build as buildCtf, makeTerrain } from './oax_outdoor_ctf.mjs';
import { vehicleFiles, addVehicle } from '../lib/vehicles.mjs';

// [type, x, y] for the red team (x > 0); blue is mirrored
export const VEHICLE_SPOTS = [
  ['buggy', 2200, -1050],
  ['buggy', 2200, 1050],
  ['hover', 2250, -620],
];

export function build() {
  const spec = buildCtf();
  const t = makeTerrain();
  for (const s of [1, -1]) {
    for (const [type, x, y] of VEHICLE_SPOTS) {
      const z = Math.ceil(t.groundZ(s * x, y)) + 60;
      addVehicle(spec.map, type, [s * x, y, z], s > 0 ? 180 : 0, 20);
    }
  }
  spec.map.world.keys.message = 'oax outdoor vehicle CTF';
  return {
    ...spec,
    manifest: { ...spec.manifest, features: [...(spec.manifest?.features || []), 'vehicles'] },
    files: { ...spec.files, ...vehicleFiles() },
  };
}
