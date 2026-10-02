// gl-trace-hook.mjs: preload into the romdev server (node --import) to find
// which GL call crashes the driver. Wraps webgl-node's bufferData /
// bufferSubData and writes the most recent calls to a file with writeSync
// before each one runs, so the file names the last call even when the
// process dies inside the driver.
//   GL_TRACE_FILE (default /tmp/romdev-gl-trace.txt)

import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const romdev = process.env.ROMDEV_DIR;
if (!romdev) throw new Error("set ROMDEV_DIR to the romdevtools package (its src/mcp/server.js)");
const ctxUrl = pathToFileURL(path.join(romdev, '..', '..', 'node_modules', 'webgl-node', 'lib', 'webgl2-context.mjs')).href;
const mod = await import(ctxUrl);
const Ctx = Object.values(mod).find((v) => typeof v === 'function' && v.prototype && 'bufferData' in v.prototype);
const fd = fs.openSync(process.env.GL_TRACE_FILE || '/tmp/romdev-gl-trace.txt', 'w');
const ring = [];
let n = 0;

function note(line) {
  ring.push(`${n++} ${new Date().toISOString()} ${line}`);
  if (ring.length > 40) ring.shift();
  const text = ring.join('\n') + '\n';
  fs.writeSync(fd, text.padEnd(4096 * 2, ' '), 0);
}

function describe(d) {
  if (typeof d === 'number') return `size=${d}`;
  if (d == null) return 'null';
  const b = d.buffer;
  return `bytes=${d.byteLength} off=${d.byteOffset} bufLen=${b ? b.byteLength : '?'} detached=${b ? b.detached : '?'} ctor=${d.constructor && d.constructor.name}`;
}

if (Ctx) {
  const bd = Ctx.prototype.bufferData;
  Ctx.prototype.bufferData = function (target, src, usage, ...rest) {
    let bound = '';
    try { bound = `bound=${target === 0x8892 ? this._boundArrayBuffer?._id : this._boundElementArrayBuffer?._id}`; } catch {}
    note(`bufferData target=0x${target.toString(16)} ${describe(src)} usage=0x${(usage || 0).toString(16)} ${bound}`);
    return bd.call(this, target, src, usage, ...rest);
  };
  const bsd = Ctx.prototype.bufferSubData;
  Ctx.prototype.bufferSubData = function (target, offset, src, ...rest) {
    note(`bufferSubData target=0x${target.toString(16)} offset=${offset} ${describe(src)}`);
    return bsd.call(this, target, offset, src, ...rest);
  };
  console.error(`[gl-trace-hook] tracing ${Ctx.name}.bufferData`);
} else {
  console.error('[gl-trace-hook] webgl-node context class not found; no tracing');
}
