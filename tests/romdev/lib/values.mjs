// values.mjs: named debug values ("name value" lines; Com_DebugSet), as the
// cart's debug_values field and the `debugvalues <file>` command write them.

export function parseDebugValues(text) {
  const out = {};
  for (const line of String(text || '').split('\n')) {
    const at = line.indexOf(' ');
    if (at > 0) out[line.slice(0, at)] = line.slice(at + 1);
  }
  return out;
}

export async function readValues(s) {
  return parseDebugValues(await s.read('debug_values'));
}
