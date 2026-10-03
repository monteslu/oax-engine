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

varying vec3   var_Position;
varying vec3   var_Normal;

float EyeDepth(float d)
{
	float zNear = u_ViewInfo.x;
	float zFar = u_ViewInfo.y;
	return 2.0 * zNear * zFar / (zFar + zNear - (d * 2.0 - 1.0) * (zFar - zNear));
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
	refr = mix(u_SpecularScale.rgb, refr, exp(-u_SpecularScale.w * thick));

	// reflection, Schlick fresnel
	vec3 V = (u_ViewOrigin - var_Position) / max(rayLen, 0.001);
	float cosT = clamp(dot(Nw, V), 0.0, 1.0);
	float fres = u_ViewInfo.w + (1.0 - u_ViewInfo.w) * pow(1.0 - cosT, 5.0);
	vec2 reflUV = (gl_FragCoord.xy - u_CubeMapInfo.xy) * u_CubeMapInfo.zw + offs;
	vec3 refl = texture2D(u_DiffuseMap, reflUV).rgb;

	gl_FragColor = vec4(mix(refr, refl, clamp(fres * u_ViewInfo.z, 0.0, 1.0)), 1.0);
}
