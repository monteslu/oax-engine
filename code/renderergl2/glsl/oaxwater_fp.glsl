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

// oax water (tr_oax_water.c): refraction of the opaque scene (a copy of
// its colour and depth), a planar reflection, two scrolling normal-map
// wave layers and a depth tint.
//   u_NormalScale    normal map repeats per unit, layer speeds, distortion
//   u_SpecularScale  tint rgb, absorption per unit of thickness
//   u_ViewInfo       zNear, zFar, reflectivity (0 = no reflection), fresnel at normal incidence
//   u_CubeMapInfo    viewport x, y, 1/w, 1/h (pixels): the reflection target covers the viewport
//   u_InvTexRes      1 / scene copy size
//   u_VertexLerp     wave normal strength
//   u_FogDistance    foam depth, foam strength (0 = none), caustics strength (0 = none), caustics repeats per unit
uniform sampler2D u_DiffuseMap;
uniform sampler2D u_LightMap;
uniform sampler2D u_NormalMap;
uniform sampler2D u_ScreenDepthMap;

uniform vec3   u_ViewOrigin;
uniform vec3   u_ViewForward;
uniform float  u_Time;
uniform vec4   u_NormalScale;
uniform vec4   u_SpecularScale;
uniform vec4   u_ViewInfo;
uniform vec4   u_CubeMapInfo;
uniform vec2   u_InvTexRes;
uniform float  u_VertexLerp;	// wave strength
uniform vec4   u_FogDistance;	// foam and caustics

varying vec3   var_Position;
varying vec3   var_Normal;

float EyeDepth(float d)
{
	float zNear = u_ViewInfo.x;
	float zFar = u_ViewInfo.y;
	return 2.0 * zNear * zFar / (zFar + zNear - (d * 2.0 - 1.0) * (zFar - zNear));
}

// the two wave layers' normal xy at a point of the plane (texture repeats)
vec2 WaveXY(vec2 uv, float t)
{
	return texture2D(u_NormalMap, uv + vec2(t * u_NormalScale.y, t * u_NormalScale.y * 0.7)).xy
	     + texture2D(u_NormalMap, uv * 1.37 + vec2(-t * u_NormalScale.z * 0.6, t * u_NormalScale.z)).xy - 1.0;
}

// light the waves would focus onto the bottom: two wave layers moving
// apart cross where their slopes cancel, and the refracted sunlight
// gathers along those crossings in thin bright lines
float Caustics(vec2 uv, float t)
{
	vec2 a = WaveXY(uv, t);
	vec2 b = WaveXY(uv * 1.31 + vec2(0.37, 0.71), -t * 0.83);
	float l1 = 1.0 - clamp(abs(a.x - b.x) * 2.2, 0.0, 1.0);
	float l2 = 1.0 - clamp(abs(a.y - b.y) * 2.2, 0.0, 1.0);
	return pow(l1, 5.0) + pow(l2, 5.0);
}

void main()
{
	vec3 N = normalize(var_Normal);
	vec3 T = normalize(abs(N.z) < 0.9 ? cross(vec3(0.0, 0.0, 1.0), N) : cross(vec3(0.0, 1.0, 0.0), N));
	vec3 B = cross(N, T);
	vec2 uv = vec2(dot(var_Position, T), dot(var_Position, B)) * u_NormalScale.x;
	float t = u_Time;

	vec3 n1 = texture2D(u_NormalMap, uv + vec2(t * u_NormalScale.y, t * u_NormalScale.y * 0.7)).xyz * 2.0 - 1.0;
	vec3 n2 = texture2D(u_NormalMap, uv * 1.37 + vec2(-t * u_NormalScale.z * 0.6, t * u_NormalScale.z)).xyz * 2.0 - 1.0;
	vec3 wn = normalize(vec3((n1.xy + n2.xy) * u_VertexLerp, n1.z * n2.z));
	vec3 Nw = normalize(T * wn.x + B * wn.y + N * wn.z);
	vec2 offs = wn.xy * u_NormalScale.w;

	vec2 screen = gl_FragCoord.xy * u_InvTexRes;
	float surfZ = dot(var_Position - u_ViewOrigin, u_ViewForward);

	// seen from below (a two-sided water shader): the world above shows
	// through a window overhead, refracted and wobbling; past the critical
	// angle (about 49 degrees from straight up) total internal reflection
	// leaves the water's own colour
	if (dot(u_ViewOrigin - var_Position, N) < 0.0)
	{
		vec3 Vu = normalize(u_ViewOrigin - var_Position);
		float cosU = clamp(dot(-Nw, Vu), 0.0, 1.0);
		vec3 above = texture2D(u_LightMap, screen + offs * 2.0).rgb;
		vec3 deep = u_SpecularScale.rgb * 0.7;
		float window = smoothstep(0.58, 0.72, cosU);
		gl_FragColor = vec4(mix(deep, mix(u_SpecularScale.rgb, above, 0.7), window), 1.0);
		return;
	}

	// refraction: offset only where what is seen there is behind the surface
	vec2 refrUV = screen + offs;
	float sceneZ = EyeDepth(texture2D(u_ScreenDepthMap, refrUV).r);
	if (sceneZ < surfZ)
	{
		refrUV = screen;
		sceneZ = EyeDepth(texture2D(u_ScreenDepthMap, screen).r);
	}
	vec3 refr = texture2D(u_LightMap, refrUV).rgb;

	// thickness along the view ray, absorbed toward the tint
	float rayLen = length(var_Position - u_ViewOrigin);
	float thick = max(sceneZ - surfZ, 0.0) * rayLen / max(surfZ, 1.0);
	vec3 rayDir = (var_Position - u_ViewOrigin) / max(rayLen, 0.001);
	float depthBelow = thick * abs(dot(rayDir, N));	// the bottom's depth under the surface

	if (u_FogDistance.z > 0.0 && thick > 0.0)
	{
		vec3 bottom = var_Position + rayDir * thick;
		float c = Caustics(vec2(dot(bottom, T), dot(bottom, B)) * u_FogDistance.w, t);
		refr *= 1.0 + u_FogDistance.z * c * exp(-depthBelow * 0.015);
	}
	refr = mix(u_SpecularScale.rgb, refr, exp(-u_SpecularScale.w * thick));

	// reflection, Schlick fresnel
	vec3 V = (u_ViewOrigin - var_Position) / max(rayLen, 0.001);
	float cosT = clamp(dot(Nw, V), 0.0, 1.0);
	float fres = u_ViewInfo.w + (1.0 - u_ViewInfo.w) * pow(1.0 - cosT, 5.0);
	vec2 reflUV = (gl_FragCoord.xy - u_CubeMapInfo.xy) * u_CubeMapInfo.zw + offs;
	vec3 refl = texture2D(u_DiffuseMap, reflUV).rgb;

	vec3 color = mix(refr, refl, clamp(fres * u_ViewInfo.z, 0.0, 1.0));

	// shore foam: a broken band over shallow water, drifting with the waves
	if (u_FogDistance.y > 0.0)
	{
		float edge = 1.0 - clamp(depthBelow / u_FogDistance.x, 0.0, 1.0);
		vec2 f = WaveXY(uv * 3.1, t * 1.5);
		float pattern = 0.5 + 0.5 * (f.x - f.y);
		float foam = smoothstep(0.5, 0.85, edge * edge + (pattern - 0.5) * 0.9) * edge;
		vec3 foamCol = vec3(0.35 + dot(refl, vec3(0.3)));
		color = mix(color, foamCol, clamp(foam * u_FogDistance.y, 0.0, 1.0));
	}

	gl_FragColor = vec4(color, 1.0);
}
