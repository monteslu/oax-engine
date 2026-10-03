// romdev.mjs: a thin client for romdev's HTTP tool endpoint, plus the cart
// helpers every test uses (debug fields, console commands, input).
//
// Each test gets its own romdev session, so tests never share a host.
// ROMDEV_URL overrides the server (default http://127.0.0.1:7331).

import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { cartBootCommand } from './capture.mjs';

const ROMDEV_URL = process.env.ROMDEV_URL || 'http://127.0.0.1:7331';
const here = path.dirname(fileURLToPath(import.meta.url));
export const repoRoot = path.resolve(here, '..', '..', '..');
export const defaultCart = process.env.OA_CART || path.join(repoRoot, 'build-cart', 'cart');

export class RomdevError extends Error {}

// Infrastructure failure (server down), distinct from a test failure.
export class RomdevUnavailable extends Error {}

export class Session {
  constructor(name) {
    this.id = `${name}-${process.pid}-${Date.now()}`;
    this.fields = null;
  }

  async call(tool, args = {}) {
    let res;
    try {
      res = await fetch(`${ROMDEV_URL}/tool/${tool}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'x-romdev-session': this.id },
        body: JSON.stringify(args),
        // one romdev call should take seconds; a hang should fail fast, not
        // sit for minutes (ROMDEV_TIMEOUT_MS raises it for a slow host)
        signal: AbortSignal.timeout(Number(process.env.ROMDEV_TIMEOUT_MS || 60000)),
      });
    } catch (e) {
      throw new RomdevUnavailable(`romdev unreachable at ${ROMDEV_URL}: ${e.message}`);
    }
    const text = await res.text();
    let body;
    try { body = JSON.parse(text); } catch { body = { raw: text }; }
    if (!res.ok || body.error || body.isError) {
      throw new RomdevError(`${tool} failed: ${text.slice(0, 600)}`);
    }
    return body;
  }

  // picmip: the boot r_picmip (capture.mjs; legacy goldens use 1)
  async load(cart = defaultCart, seed = 1, { picmip } = {}) {
    // 16 ms frames: the engine keeps time in whole milliseconds, and the
    // native reference runs with fixedtime 16, so both see the same msec.
    const stepMs = Number(process.env.OA_STEP_MS || 16);
    const r = await this.call('loadMedia', { platform: 'wasmcart', path: cart, deterministicSeed: seed, deterministicStepMs: stepMs });
    if (!r.capabilities?.hasDeterministic) throw new RomdevError('cart did not load as a deterministic replay');
    // boot cvars: the cart reads console_cmd before its first frame
    const boot = cartBootCommand({ picmip });
    if (boot) await this.command(boot);
    // the cart boots on its first frame
    await this.step(1);
    await this.refreshFields();
    // any engine error halts the cart from here on (com_errorQuit), and
    // step() throws with its message: errors are never swallowed. It rides
    // on the caller's next command or step instead of a frame of its own:
    // an extra frame here moved every later capture by 16 ms, and the
    // console_cmd buffer holds one line, so a separate write would be
    // overwritten by the caller's first command.
    this.pendingCommand = 'set com_errorQuit 1';
    return r;
  }

  // Steps, then checks the cart did not halt: a halted cart renders
  // nothing and would otherwise just look idle to the caller.
  async step(frames) {
    if (this.pendingCommand) await this.command('');
    const r = await this.call('frame', { op: 'step', frames });
    if (this.fields?.has('fatal_error')) {
      const fatal = await this.read('fatal_error');
      if (fatal) throw new RomdevError(`cart halted: ${fatal}`);
    }
    return r;
  }

  async refreshFields() {
    const r = await this.call('wasm', { op: 'debugState' });
    this.fields = new Map(r.fields.map((f) => [f.name, f]));
  }

  async read(name) {
    const r = await this.call('wasm', { op: 'read', name });
    if (r.values) return r.values;
    if (r.hex !== undefined) return r.text ?? r.hex;
    return r.value;
  }

  // Many fields in one call each; returns {name: value}.
  async readAll(names) {
    const out = {};
    for (const n of names) out[n] = await this.read(n);
    return out;
  }

  // Run a console command at the start of the next frame.
  // The field holds one line until the next frame runs it: a second write
  // before a step replaces the first, so join commands with ';'.
  async command(cmd) {
    if (this.pendingCommand) {
      cmd = cmd ? `${this.pendingCommand}; ${cmd}` : this.pendingCommand;
      this.pendingCommand = null;
    }
    const bytes = Buffer.concat([Buffer.from(cmd, 'utf8'), Buffer.from([0])]);
    await this.call('wasm', { op: 'write', name: 'console_cmd', hex: bytes.toString('hex') });
  }

  async events() {
    return this.call('wasm', { op: 'events' });
  }

  async screenshot(outPath) {
    return this.call('frame', { op: 'screenshot', path: outPath });
  }

  async setPad(state) {
    return this.call('input', { op: 'set', ports: [state] });
  }

  async shutdown() {
    try { await this.call('host', { op: 'shutdown' }); } catch { /* best effort */ }
  }

  // Step until a predicate on a debug field holds; returns frames stepped.
  async stepUntil(name, pred, maxFrames, batch = 10) {
    for (let n = 0; n < maxFrames; n += batch) {
      const v = await this.read(name);
      if (pred(v)) return n;
      await this.step(batch);
    }
    throw new RomdevError(`${name} never satisfied the condition within ${maxFrames} frames (last ${JSON.stringify(await this.read(name))})`);
  }
}

// Load a cart and get into a map with the player spawned (CA_ACTIVE).
export const CA_ACTIVE = 8;

export async function bootMap(s, map, { seed = 1, extra = '' } = {}) {
  await s.load(defaultCart, seed);
  await s.command(`${extra}${extra ? ';' : ''}map ${map}`);
  await s.stepUntil('conn_state', (v) => v === CA_ACTIVE, 3000, 20);
}
