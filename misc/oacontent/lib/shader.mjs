// shader.mjs: a parser for Quake III shader scripts (scripts/*.shader), with
// the derived facts the content tools use (sky, liquids, surfacelight, sun,
// stage maps and blends). It follows the engine's tokenizer: // and /* */
// comments, words and quoted strings, { } blocks.
export function parseShaderFile(text, file = '') {
  const shaders = [];
  let i = 0;
  const n = text.length;
  const skip = () => {
    for (;;) {
      while (i < n && /\s/.test(text[i])) i++;
      if (text[i] === '/' && text[i + 1] === '/') { while (i < n && text[i] !== '\n') i++; continue; }
      if (text[i] === '/' && text[i + 1] === '*') { const e = text.indexOf('*/', i + 2); i = e < 0 ? n : e + 2; continue; }
      return;
    }
  };
  // a line of words, up to end of line or a brace
  const line = () => {
    const words = [];
    for (;;) {
      while (i < n && (text[i] === ' ' || text[i] === '\t' || text[i] === '\r')) i++;
      if (i >= n || text[i] === '\n') break;
      if (text[i] === '/' && text[i + 1] === '/') { while (i < n && text[i] !== '\n') i++; break; }
      if (text[i] === '{' || text[i] === '}') break;
      if (text[i] === '"') { const e = text.indexOf('"', i + 1); words.push(text.slice(i + 1, e < 0 ? n : e)); i = e < 0 ? n : e + 1; continue; }
      const s = i; while (i < n && !/\s/.test(text[i]) && text[i] !== '{' && text[i] !== '}') i++;
      words.push(text.slice(s, i));
    }
    return words;
  };
  for (;;) {
    skip();
    if (i >= n) break;
    const nameStart = i;
    const nameWords = line();
    const name = nameWords[0];
    skip();
    if (text[i] !== '{') { if (!name) i++; continue; }
    i++;
    const sh = { name, file, directives: [], stages: [], start: nameStart };
    for (;;) {
      skip();
      if (i >= n) break;
      if (text[i] === '}') { i++; break; }
      if (text[i] === '{') {
        i++;
        const stage = { directives: [] };
        for (;;) {
          skip();
          if (i >= n) break;
          if (text[i] === '}') { i++; break; }
          const w = line();
          if (w.length) stage.directives.push(w);
          else i++;
        }
        sh.stages.push(stage);
        continue;
      }
      const w = line();
      if (w.length) sh.directives.push(w); else i++;
    }
    sh.end = i;
    sh.text = text.slice(nameStart, i);
    derive(sh);
    shaders.push(sh);
  }
  return shaders;
}

const lc = (s) => (s || '').toLowerCase();

function derive(sh) {
  const d = sh.directives.map((w) => [lc(w[0]), ...w.slice(1)]);
  const has = (k) => d.some((w) => w[0] === k);
  const get = (k) => d.find((w) => w[0] === k);
  sh.parms = new Set(d.filter((w) => w[0] === 'surfaceparm').map((w) => lc(w[1])));
  sh.sky = sh.parms.has('sky') || has('skyparms');
  sh.skyparms = get('skyparms') ? get('skyparms').slice(1) : null;
  sh.liquid = ['water', 'lava', 'slime'].find((k) => sh.parms.has(k)) || null;
  sh.fog = sh.parms.has('fog');
  sh.nodraw = sh.parms.has('nodraw') || sh.parms.has('nodrawnonsolid');
  sh.noLightmap = sh.parms.has('nolightmap') || has('q3map_nolightmap');
  const sl = get('q3map_surfacelight');
  sh.surfaceLight = sl ? Number(sl[1]) || 0 : 0;
  const sd = get('q3map_lightimage');
  sh.lightImage = sd ? sd[1] : null;
  const sun = get('q3map_sun') || get('q3map_sunext');
  sh.sun = sun ? sun.slice(1).map(Number) : null;       // r g b intensity degrees elevation
  sh.hasGl2Sun = has('q3gl2_sun');
  sh.cull = get('cull') ? lc(get('cull')[1]) : null;
  sh.sort = get('sort') ? lc(get('sort')[1]) : null;
  sh.deform = d.filter((w) => w[0] === 'deformvertexes').map((w) => w.slice(1));
  sh.oax = d.filter((w) => w[0].startsWith('oax')).map((w) => w[0]);
  sh.emissiveHint = sh.surfaceLight > 0;
  sh.stageInfo = sh.stages.map((st) => {
    const sd2 = st.directives.map((w) => [lc(w[0]), ...w.slice(1)]);
    const g = (k) => sd2.find((w) => w[0] === k);
    const map = g('map') || g('clampmap') || g('animmap');
    return {
      map: map ? (map[0] === 'animmap' ? map.slice(2) : [map[1]]) : [],
      lightmap: !!(g('map') && lc(g('map')[1]) === '$lightmap'),
      whiteimage: !!(g('map') && lc(g('map')[1]) === '$whiteimage'),
      blend: g('blendfunc') ? g('blendfunc').slice(1).map(lc) : null,
      alphaFunc: g('alphafunc') ? lc(g('alphafunc')[1]) : null,
      tcgen: g('tcgen') ? lc(g('tcgen')[1]) : null,
      tcmod: sd2.filter((w) => w[0] === 'tcmod').map((w) => w.slice(1)),
      rgbgen: g('rgbgen') ? g('rgbgen').slice(1).map(lc) : null,
    };
  });
  sh.textures = [...new Set(sh.stageInfo.flatMap((s) => s.map).filter((m) => m && !m.startsWith('$')).map((m) => m.replace(/\.(tga|jpg|jpeg|png)$/i, '').toLowerCase()))];
  sh.blended = sh.stageInfo.some((s) => s.blend && !(s.blend[0] === 'gl_one' && s.blend[1] === 'gl_zero') && !(s.blend[0] === 'filter') && s.blend.join(' ') !== 'blend' ? true : false) || sh.sort === 'additive' || sh.sort === 'underwater' || sh.sort === 'banner' || sh.sort === 'nearest';
}

// all shaders of a list of {name, data} script files, in the engine's order:
// files sorted by name, and the first definition of a name wins
export function loadShaders(files) {
  const sorted = [...files].sort((a, b) => (a.name.toLowerCase() < b.name.toLowerCase() ? -1 : a.name.toLowerCase() > b.name.toLowerCase() ? 1 : 0));
  const byName = new Map();
  const all = [];
  for (const f of sorted) {
    for (const sh of parseShaderFile(f.data.toString('latin1'), f.name)) {
      all.push(sh);
      if (sh.name && !byName.has(sh.name.toLowerCase())) byName.set(sh.name.toLowerCase(), sh);
    }
  }
  return { byName, all };
}
