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

// oax bloom (tr_oax_bloom.c), one program per pass:
//   BLOOM_PREFILTER  scene -> half size: 4-tap box, soft-knee threshold
//                    u_Color = threshold, knee, 0, 0
//   BLOOM_DOWN       level -> next smaller level: 13-tap (Jimenez 2014)
//   BLOOM_UP         smaller level -> larger level, added: 9-tap tent
//                    u_Color.x = sample radius in source texels
//   BLOOM_COMPOSITE  half-size result added to the scene: u_Color = intensity
// u_InvTexRes is the source texel size.
uniform sampler2D u_TextureMap;

uniform vec4   u_Color;
uniform vec2   u_InvTexRes;

varying vec2   var_TexCoords;

vec3 Tap(vec2 uv)
{
	return texture2D(u_TextureMap, uv).rgb;
}

void main()
{
	vec2 tc = var_TexCoords;
	vec2 d = u_InvTexRes;
	vec3 c;

#if defined(BLOOM_PREFILTER)
	c = (Tap(tc + d * vec2(-0.5, -0.5)) + Tap(tc + d * vec2(0.5, -0.5)) +
	     Tap(tc + d * vec2(-0.5, 0.5)) + Tap(tc + d * vec2(0.5, 0.5))) * 0.25;
	float threshold = u_Color.x;
	float knee = max(u_Color.y * threshold, 1e-4);
	float br = max(c.r, max(c.g, c.b));
	float soft = clamp(br - threshold + knee, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee);
	float contrib = max(soft, br - threshold) / max(br, 1e-4);
	c *= contrib;
#elif defined(BLOOM_DOWN)
	vec3 a = Tap(tc + d * vec2(-2.0, -2.0));
	vec3 b = Tap(tc + d * vec2( 0.0, -2.0));
	vec3 e = Tap(tc + d * vec2( 2.0, -2.0));
	vec3 f = Tap(tc + d * vec2(-1.0, -1.0));
	vec3 g = Tap(tc + d * vec2( 1.0, -1.0));
	vec3 h = Tap(tc + d * vec2(-2.0,  0.0));
	vec3 i = Tap(tc);
	vec3 j = Tap(tc + d * vec2( 2.0,  0.0));
	vec3 k = Tap(tc + d * vec2(-1.0,  1.0));
	vec3 l = Tap(tc + d * vec2( 1.0,  1.0));
	vec3 m = Tap(tc + d * vec2(-2.0,  2.0));
	vec3 n = Tap(tc + d * vec2( 0.0,  2.0));
	vec3 o = Tap(tc + d * vec2( 2.0,  2.0));
	c = (f + g + k + l) * 0.125 + i * 0.125 +
	    (a + b + h + i) * 0.03125 + (b + e + i + j) * 0.03125 +
	    (h + i + m + n) * 0.03125 + (i + j + n + o) * 0.03125;
#elif defined(BLOOM_UP)
	vec2 r = d * u_Color.x;
	c  = Tap(tc + r * vec2(-1.0, -1.0)) + Tap(tc + r * vec2(1.0, -1.0)) +
	     Tap(tc + r * vec2(-1.0,  1.0)) + Tap(tc + r * vec2(1.0,  1.0));
	c += (Tap(tc + r * vec2(0.0, -1.0)) + Tap(tc + r * vec2(-1.0, 0.0)) +
	      Tap(tc + r * vec2(1.0,  0.0)) + Tap(tc + r * vec2(0.0,  1.0))) * 2.0;
	c += Tap(tc) * 4.0;
	c *= 1.0 / 16.0;
#else
	c = Tap(tc) * u_Color.x;
#endif

	gl_FragColor = vec4(c, 1.0);
}
