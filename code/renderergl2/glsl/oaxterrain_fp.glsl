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
uniform float  u_Triplanar;    // 1: steep faces take side projections (OAX_TERRAIN_TRIPLANAR)
uniform vec2   u_SurfaceFx;    // x: macro variation (OAX_TERRAIN_MACRO), y: close detail (OAX_TERRAIN_DETAIL)
uniform vec3   u_ViewOrigin;

varying vec3   var_Position;
varying vec3   var_Normal;

// smooth value noise, 0..1
float Noise(vec2 p)
{
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
	float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
	float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
	float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float Luma(vec3 c)
{
	return dot(c, vec3(0.299, 0.587, 0.114));
}
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

	vec3 n = normalize(var_Normal);
	vec3 albedo = texture2D(u_Layer0, var_Position.xy * u_LayerScale.x).rgb * w.x
	            + texture2D(u_Layer1, var_Position.xy * u_LayerScale.y).rgb * w.y
	            + texture2D(u_Layer2, var_Position.xy * u_LayerScale.z).rgb * w.z
	            + texture2D(u_Layer3, var_Position.xy * u_LayerScale.w).rgb * w.w;
	if (u_Triplanar > 0.5)
	{
		// triplanar: the top-down projection stretches on cliffs; blend in
		// the two side projections by how much the face looks along x and y
		vec3 bw = pow(abs(n), vec3(4.0));
		bw /= (bw.x + bw.y + bw.z);
		if (bw.z < 0.999)
		{
			vec3 sx = texture2D(u_Layer0, var_Position.yz * u_LayerScale.x).rgb * w.x
			        + texture2D(u_Layer1, var_Position.yz * u_LayerScale.y).rgb * w.y
			        + texture2D(u_Layer2, var_Position.yz * u_LayerScale.z).rgb * w.z
			        + texture2D(u_Layer3, var_Position.yz * u_LayerScale.w).rgb * w.w;
			vec3 sy = texture2D(u_Layer0, var_Position.xz * u_LayerScale.x).rgb * w.x
			        + texture2D(u_Layer1, var_Position.xz * u_LayerScale.y).rgb * w.y
			        + texture2D(u_Layer2, var_Position.xz * u_LayerScale.z).rgb * w.z
			        + texture2D(u_Layer3, var_Position.xz * u_LayerScale.w).rgb * w.w;
			albedo = albedo * bw.z + sx * bw.x + sy * bw.y;
		}
	}

	if (u_SurfaceFx.x > 0.5)
	{
		// macro variation: patches a few hundred to a couple of thousand
		// units across, a little lighter or darker and warmer or cooler, so
		// a layer's repeat does not read as a grid from afar
		vec2 p = var_Position.xy;
		float m = Noise(p / 1900.0) * 0.6 + Noise(p / 640.0 + 7.3) * 0.4;
		float k = m - 0.5;
		albedo *= (1.0 + 0.36 * k) * vec3(1.0 + 0.10 * k, 1.0, 1.0 - 0.12 * k);
	}
	if (u_SurfaceFx.y > 0.5)
	{
		// close detail: the layers again at a finer, turned repeat; their
		// brightness against the layer's average (a far mip) modulates the
		// colour, fading out with distance
		float fade = 1.0 - smoothstep(250.0, 1100.0, length(var_Position - u_ViewOrigin));
		if (fade > 0.0)
		{
			vec2 q = mat2(0.8, -0.6, 0.6, 0.8) * var_Position.xy * 5.3;
			float hi = Luma(texture2D(u_Layer0, q * u_LayerScale.x).rgb) * w.x
			         + Luma(texture2D(u_Layer1, q * u_LayerScale.y).rgb) * w.y
			         + Luma(texture2D(u_Layer2, q * u_LayerScale.z).rgb) * w.z
			         + Luma(texture2D(u_Layer3, q * u_LayerScale.w).rgb) * w.w;
			float avg = Luma(texture2D(u_Layer0, q * u_LayerScale.x, 12.0).rgb) * w.x
			          + Luma(texture2D(u_Layer1, q * u_LayerScale.y, 12.0).rgb) * w.y
			          + Luma(texture2D(u_Layer2, q * u_LayerScale.z, 12.0).rgb) * w.z
			          + Luma(texture2D(u_Layer3, q * u_LayerScale.w, 12.0).rgb) * w.w;
			albedo *= mix(1.0, clamp(hi / max(avg, 0.02), 0.55, 1.45), 0.55 * fade * smoothstep(0.45, 0.8, n.z));
		}
	}

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
