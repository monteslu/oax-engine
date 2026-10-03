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

// oax heightmap terrain: four splatted layers, sun + ambient, screen-space
// sun shadow mask
#if !defined(TERRAIN_DEPTH)
uniform sampler2D u_Layer0;
uniform sampler2D u_Layer1;
uniform sampler2D u_Layer2;
uniform sampler2D u_Layer3;
uniform sampler2D u_Splat;
uniform sampler2D u_ScreenShadow;

uniform vec4   u_LayerScale;    // 1 / world units per repeat, per layer (0: unused)
uniform vec4   u_SplatXform;    // origin - half a cell (xy), 1 / (cellSize * samples) (zw)
uniform vec3   u_SunDir;
uniform vec3   u_SunColor;
uniform vec3   u_Ambient;
uniform vec4   u_ScreenInfo;    // 1 / framebuffer size (xy), w: use the shadow mask
uniform float  u_Debug;

varying vec3   var_Position;
varying vec3   var_Normal;
#endif

void main()
{
#if defined(TERRAIN_DEPTH)
	gl_FragColor = vec4(0.0);
#elif defined(TERRAIN_FLAT)
	// crack test: every terrain fragment is the same colour
	gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);
#else
	vec2 st = (var_Position.xy - u_SplatXform.xy) * u_SplatXform.zw;
	vec4 w = texture2D(u_Splat, st);
	w *= step(vec4(1e-6), u_LayerScale);
	w /= max(dot(w, vec4(1.0)), 1e-3);

	vec3 albedo = texture2D(u_Layer0, var_Position.xy * u_LayerScale.x).rgb * w.x
	            + texture2D(u_Layer1, var_Position.xy * u_LayerScale.y).rgb * w.y
	            + texture2D(u_Layer2, var_Position.xy * u_LayerScale.z).rgb * w.z
	            + texture2D(u_Layer3, var_Position.xy * u_LayerScale.w).rgb * w.w;

	vec3 n = normalize(var_Normal);
	float ndl = max(dot(n, u_SunDir), 0.0);
	float shadow = 1.0;
	if (u_ScreenInfo.w > 0.5)
		shadow = texture2D(u_ScreenShadow, gl_FragCoord.xy * u_ScreenInfo.xy).r;

	vec3 light = u_Ambient * (0.6 + 0.4 * n.z) + u_SunColor * (ndl * shadow);
	gl_FragColor = vec4(albedo * light, 1.0);
	if (u_Debug > 3.5)
		gl_FragColor = vec4(vec3(shadow), 1.0);	// r_oaxTerrainDebug 4: the sun shadow mask
#endif
}
