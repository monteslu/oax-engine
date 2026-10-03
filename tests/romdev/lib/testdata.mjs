// Where the test references live: goldens and native reference renders are
// kept in a separate repo (oax-engine-testdata) so that a plain clone of the
// engine stays small. OAX_TESTDATA names it; the default is a sibling checkout
// next to the engine repo.
//
// Tests write a golden when it is missing, so a missing testdata checkout must
// stop the run: otherwise every image test would pass by writing new goldens.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');

export const testdataDir = path.resolve(process.env.OAX_TESTDATA || path.join(repoRoot, '..', 'oax-engine-testdata'));
export const goldensDir = path.join(testdataDir, 'goldens');
export const referenceDir = path.join(testdataDir, 'reference');

/** Throw unless the testdata checkout is there (any run that is not --update). */
export function requireTestdata() {
  if (!fs.existsSync(goldensDir) || !fs.existsSync(referenceDir)) {
    throw new Error(`test references not found at ${testdataDir}: clone https://github.com/monteslu/oax-engine-testdata next to the engine, or set OAX_TESTDATA`);
  }
}
