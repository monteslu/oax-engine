// foliage-art.mjs: our own procedural foliage textures for misc_oax_terrain
// maps (renderergl2/tr_terrain.c draws them on generated meshes):
//   textures/oax_terrain/grassblades: alpha-tested grass blades, blade root
//     at the bottom of the image (the grass mesh maps v 1 to the ground)
//   textures/oax_terrain/tree: an atlas, bark in u 0..0.25 (the trunk) and
//     leaves in u 0.25..1 (the canopy cones)

import { tga, image, hash2 } from './texgen.mjs';

function grass() {
  const w = 128, h = 128;
  const blades = [];
  for (let k = 0; k < 22; k++) {
    const x0 = 4 + hash2(k, 1, 9) * (w - 8);
    blades.push({ x0, lean: (hash2(k, 2, 9) - 0.5) * 40, top: 8 + hash2(k, 3, 9) * 50, half: 2.2 + hash2(k, 4, 9) * 2.5, shade: hash2(k, 5, 9) });
  }
  return image(w, h, (x, y) => {
    for (const b of blades) {
      if (y < b.top) continue;
      const t = (h - y) / (h - b.top);              // 0 at the root, 1 at the tip
      const cx = b.x0 + b.lean * t * t;
      const half = b.half * (1 - t) + 0.3;
      if (Math.abs(x + 0.5 - cx) <= half) {
        const g = 0.35 + 0.35 * t + 0.15 * b.shade;
        return [0.18 + 0.2 * t * b.shade, g, 0.08 + 0.05 * t, 1];
      }
    }
    return [0.2, 0.35, 0.1, 0];
  });
}

function tree() {
  const w = 256, h = 128;
  return image(w, h, (x, y) => {
    if (x < 64) {
      // bark: vertical streaks
      const n = hash2(x >> 1, y >> 4, 3) * 0.5 + hash2(x, y, 4) * 0.2;
      const v = 0.22 + 0.18 * n;
      return [v * 1.25, v * 0.9, v * 0.6, 1];
    }
    // leaves: clumpy greens
    const c = hash2(x >> 3, y >> 3, 7) * 0.5 + hash2(x >> 1, y >> 1, 8) * 0.3 + hash2(x, y, 11) * 0.2;
    const v = 0.15 + 0.4 * c;
    return [v * 0.45, v, v * 0.35, 1];
  });
}

// The engine's TGA loader ignores the top-left origin bit and reads rows
// bottom-up, so write a bottom-left image (rows reversed, origin bit clear).
function tgaUpright(w, h, rgba) {
  const flipped = new Uint8Array(rgba.length);
  for (let y = 0; y < h; y++) flipped.set(rgba.subarray(y * w * 4, (y + 1) * w * 4), (h - 1 - y) * w * 4);
  const out = tga(w, h, flipped);
  out[17] = 0x08;
  return out;
}

export function foliageFiles() {
  return {
    'textures/oax_terrain/grassblades.tga': tgaUpright(128, 128, grass()),
    'textures/oax_terrain/tree.tga': tgaUpright(256, 128, tree()),
  };
}
