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

// oax heightmap terrain (tr_terrain.c): world-space vertices, so no model matrix
attribute vec3 attr_Position;
attribute vec3 attr_Normal;

uniform mat4   u_ModelViewProjectionMatrix;

#if !defined(TERRAIN_DEPTH)
varying vec3   var_Position;
varying vec3   var_Normal;
#endif

// the depth prepass and the colour pass must produce the same depths
invariant gl_Position;

void main()
{
	gl_Position = u_ModelViewProjectionMatrix * vec4(attr_Position, 1.0);
#if !defined(TERRAIN_DEPTH)
	var_Position = attr_Position;
	var_Normal = attr_Normal;
#endif
}
