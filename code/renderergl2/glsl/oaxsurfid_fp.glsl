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

// oax surface id pass (tr_oax_surfid.c): the generic vertex program, and
// this fragment program, which writes the surface's draw id (24 bits as
// three exact 8-bit channels) into an RGBA8 target. The stage's alpha test
// is kept so masked surfaces (grates, foliage) leave their holes.
uniform sampler2D u_DiffuseMap;

uniform int       u_AlphaTest;
uniform vec4      u_OaxSurfId;

varying vec2      var_DiffuseTex;

varying vec4      var_Color;

void main()
{
	float alpha = texture2D(u_DiffuseMap, var_DiffuseTex).a * var_Color.a;
	if (u_AlphaTest == 1)
	{
		if (alpha == 0.0)
			discard;
	}
	else if (u_AlphaTest == 2)
	{
		if (alpha >= 0.5)
			discard;
	}
	else if (u_AlphaTest == 3)
	{
		if (alpha < 0.5)
			discard;
	}

	gl_FragColor = u_OaxSurfId;
}
