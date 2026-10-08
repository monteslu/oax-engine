#!/usr/bin/env node
// oacontent: the driver for the content tools (README.md).
//   node misc/oacontent/oacontent.mjs <command> [--option value ...]
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from './lib/common.mjs';

const COMMANDS = ['audit', 'skyexposure', 'lights', 'materials', 'textures', 'smooth', 'tour', 'pack', 'safety', 'fxpreview', 'pilot', 'fires', 'sky'];

async function main() {
  const [cmd, ...rest] = process.argv.slice(2);
  if (!cmd || cmd === '--help' || cmd === 'help' || !COMMANDS.includes(cmd)) {
    console.log(`usage: oacontent <command> [options]\ncommands: ${COMMANDS.join(', ')}\n(see misc/oacontent/README.md)`);
    process.exit(cmd && !['--help', 'help'].includes(cmd) ? 2 : 0);
  }
  const mod = await import(`./tools/${cmd}.mjs`);
  process.exitCode = (await mod.run(parseArgs(rest))) || 0;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main();
