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

// oax stateless particles: the stage material's first texture times the
// particle colour, and soft particles: a depth fade against the opaque
// scene's depth (a copy, so the pass reads no buffer it draws to).
//   u_ViewInfo   zNear, zFar, soft distance (0 = off), soft mode
//                (1 fade alpha, 2 fade rgba, 3 fade toward white for
//                modulating blends)
//   u_InvTexRes  1 / scene depth size
// Heat haze (mode 4, u_ViewInfo.z = strength as a fraction of the screen): the
// particle shows the scene colour behind it (u_ScreenImageMap, a copy) shifted
// outward from the particle's centre by strength * alpha.
uniform sampler2D u_DiffuseMap;
uniform sampler2D u_ScreenDepthMap;
uniform sampler2D u_ScreenImageMap;

uniform vec4   u_ViewInfo;
uniform vec2   u_InvTexRes;

varying vec2   var_TexCoords;
varying vec4   var_Color;
varying float  var_EyeZ;

void main()
{
	vec4 color = texture2D(u_DiffuseMap, var_TexCoords) * var_Color;

	if (u_ViewInfo.w == 4.0)
	{
		vec2 uv = gl_FragCoord.xy * u_InvTexRes;
		vec2 d = var_TexCoords * 2.0 - 1.0;
		vec2 shift = d * color.a * u_ViewInfo.z * vec2(u_InvTexRes.x / u_InvTexRes.y, 1.0);
		gl_FragColor = vec4(texture2D(u_ScreenImageMap, uv + shift).rgb, color.a);
		return;
	}

	if (u_ViewInfo.z > 0.0)
	{
		float zNear = u_ViewInfo.x;
		float zFar = u_ViewInfo.y;
		float d = texture2D(u_ScreenDepthMap, gl_FragCoord.xy * u_InvTexRes).r;
		float zScene = 2.0 * zNear * zFar / (zFar + zNear - (d * 2.0 - 1.0) * (zFar - zNear));
		float fade = clamp((zScene - var_EyeZ) / u_ViewInfo.z, 0.0, 1.0);

		if (u_ViewInfo.w == 1.0)
			color.a *= fade;
		else if (u_ViewInfo.w == 3.0)
			color = mix(vec4(1.0), color, fade);
		else
			color *= fade;
	}

	gl_FragColor = color;
}
