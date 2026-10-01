# id Tech 4 code in this engine

This engine is based on ioquake3 and also uses code from the id Tech 4 (Doom 3)
GPL source release:

- Source: https://github.com/id-Software/DOOM-3
- Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
- License: GNU General Public License version 3, with the additional terms in
  DOOM-3's README.txt.

ioquake3 and the OpenArena gamecode are licensed "GPL version 2 or (at your
option) any later version", so they combine with GPLv3 code; once id Tech 4
code is included, the engine as a whole is distributed under the GPLv3.

No Doom 3 game data (maps, models, textures, sounds) is used, only code.

## Rules for adapted code

1. Keep id Software's copyright and license header on every file that contains
   id Tech 4 code, adapted or not.
2. Under that header, name the DOOM-3 file(s) the code came from (for example
   `Adapted from DOOM-3 neo/game/Mover.cpp`) and say what was changed (ported
   to C, ARB program rewritten as GLSL ES 3.00, and so on).
3. Add a row to the table below in the same commit.

## Files

| Engine file | From DOOM-3 | Changes |
| --- | --- | --- |
| (none yet) | | |
