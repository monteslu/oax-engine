// md3.mjs: header-level facts of Quake III MD3 models for the audit
// (surfaces, frames, vertices, triangles, tags, shaders), no geometry decode.
export function md3Info(buf, name = '') {
  if (buf.toString('latin1', 0, 4) !== 'IDP3') throw new Error(`${name}: not an MD3`);
  const version = buf.readInt32LE(4);
  const numFrames = buf.readInt32LE(76), numTags = buf.readInt32LE(80), numSurfaces = buf.readInt32LE(84);
  let ofs = buf.readInt32LE(100);          // ofsSurfaces
  let verts = 0, tris = 0;
  const shaders = [];
  for (let s = 0; s < numSurfaces; s++) {
    const ns = buf.readInt32LE(ofs + 76), nv = buf.readInt32LE(ofs + 80), nt = buf.readInt32LE(ofs + 84);
    const shOfs = buf.readInt32LE(ofs + 92);
    verts += nv; tris += nt;
    for (let k = 0; k < ns; k++) {
      let e = ofs + shOfs + k * 68; const st = e; while (e < st + 64 && buf[e]) e++;
      shaders.push(buf.toString('latin1', st, e));
    }
    ofs += buf.readInt32LE(ofs + 104);          // ofsEnd
  }
  return { version, frames: numFrames, tags: numTags, surfaces: numSurfaces, vertices: verts, triangles: tris, shaders: [...new Set(shaders)] };
}
