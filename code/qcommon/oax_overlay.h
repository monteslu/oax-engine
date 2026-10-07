/*
===========================================================================
oax engine
Copyright (C) 2026 Luis Montes

This file is part of the oax engine, a fork of ioquake3.
It is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version. The combined engine is distributed under GPLv3 (see
COPYING-GPLv3.txt).

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
more details.
===========================================================================
*/

/*
===========================================================================
oax_overlay.h: the map sidecar (docs/map-format.md, "Map overlay").

maps/<name>.oaxmap holds entity blocks in the entity lump's own text format.
At load it is merged onto the map's entity string, so an existing BSP can
get lights, worldspawn keys (oax_lighting, fog, sky, grading) and extra
entities without being recompiled:

  - a worldspawn block: its keys are set on the map's worldspawn (added, or
    replacing the same key);
  - any other block is appended after the map's own entities, so the
    ordinals of the map's entities never change.

The merge is a pure text function shared by the collision model (the server
and the game module's entity tokens) and the renderer (lights, world keys).
com_oaxEnhanced 0 turns the whole thing off: stock maps load as they always
did.
===========================================================================
*/
#ifndef OAX_OVERLAY_H
#define OAX_OVERLAY_H

// the overlay's path for a map name ("oa_dm1" -> "maps/oa_dm1.oaxmap")
#define OAX_OVERLAY_EXT		".oaxmap"

// "maps/oa_dm1.bsp" -> "maps/oa_dm1.oaxmap"
void OAX_OverlayPath( const char *bspName, char *out, int outSize );

// Merges overlay (NUL-terminated entity text) onto base (baseLen bytes of
// the map's entity string, NUL-terminated or not). Writes the result to
// out (outSize bytes, NUL-terminated) when out is not NULL, and returns the
// length the full result needs, not counting the NUL: call once with
// out == NULL to size the buffer, then again to fill it. Returns -1 when the
// overlay does not parse (unbalanced braces); the caller then keeps the
// map's own string.
int OAX_OverlayMerge( const char *base, int baseLen, const char *overlay, char *out, int outSize );

// the number of entity blocks the overlay adds (not counting worldspawn),
// for the load message; -1 when it does not parse
int OAX_OverlayCountAdded( const char *overlay );

#endif
