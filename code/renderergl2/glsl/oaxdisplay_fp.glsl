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

// oax display curve (tr_oax_display.c): a brightness + gamma display
// curve. u_Color.x = shader brightness B (0.05 to 2.99), u_Color.y = gamma exponent 1 / (1 + GammaOffset).
uniform sampler2D u_TextureMap;
uniform vec4      u_Color;

varying vec2      var_TexCoords;

void main()
{
	vec3 c = clamp(texture(u_TextureMap, var_TexCoords).rgb, 0.0, 1.0);
	float B = u_Color.x;
	float v = max(max(c.r, c.g), c.b);
	if (B > 1.0)
	{
		float w = max(v, 0.001);
		c = clamp(c * ((w + (1.0 - (2.0 * w - 1.0) * (2.0 * w - 1.0)) * 0.25 * (B - 1.0)) / w), 0.0, 1.0);
	}
	else if (B < 1.0)
	{
		c *= B;
	}
	gl_FragColor = vec4(pow(c, vec3(u_Color.y)), 1.0);
}
