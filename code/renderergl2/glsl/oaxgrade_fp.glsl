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

// oax colour grading (tr_oax_env.c, worldspawn "oax_grade"), after
// tonemapping: saturation and contrast about mid grey, a colour multiplier
// and a vignette.
//   u_Color        r g b multiplier
//   u_FogDistance  saturation, contrast, vignette (0..1)
uniform sampler2D u_TextureMap;
uniform vec4      u_Color;
uniform vec4      u_FogDistance;

varying vec2      var_TexCoords;

void main()
{
	vec3 c = clamp(texture(u_TextureMap, var_TexCoords).rgb, 0.0, 1.0);
	float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
	c = mix(vec3(lum), c, u_FogDistance.x);
	c = (c - 0.5) * u_FogDistance.y + 0.5;
	c *= u_Color.rgb;
	vec2 q = var_TexCoords - 0.5;
	float v = 1.0 - u_FogDistance.z * smoothstep(0.25, 0.75, length(q) * 1.414);
	gl_FragColor = vec4(clamp(c * v, 0.0, 1.0), 1.0);
}
