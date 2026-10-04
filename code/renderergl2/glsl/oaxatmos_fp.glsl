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

// oax atmosphere (tr_oax_env.c, worldspawn "oax_atmosphere"): height fog
// and distance haze from the scene depth, before tonemapping.
//   u_Color          fog rgb (scene light), extinction per unit at baseZ
//   u_FogDistance    falloff (per unit of height), baseZ, sun scatter, sky distance
//   u_ViewInfo       zNear, zFar
//   u_NormalScale    tan(fovX/2), tan(fovY/2), viewport origin in texture space
//   u_SpecularScale  viewport size in texture space
// The extinction is density * exp(-falloff * (z - baseZ)), integrated along
// the view ray in closed form; the colour brightens toward the sun.
uniform sampler2D u_ScreenDepthMap;

uniform vec4   u_Color;
uniform vec4   u_FogDistance;
uniform vec4   u_ViewInfo;
uniform vec4   u_NormalScale;
uniform vec4   u_SpecularScale;
uniform vec3   u_ViewOrigin;
uniform vec3   u_ViewForward;
uniform vec3   u_ViewLeft;
uniform vec3   u_ViewUp;
uniform vec4   u_PrimaryLightOrigin;

varying vec2   var_TexCoords;

void main()
{
	float d = texture2D(u_ScreenDepthMap, var_TexCoords).r;
	vec2 ndc = ((var_TexCoords - u_NormalScale.zw) / u_SpecularScale.xy) * 2.0 - 1.0;
	float zNear = u_ViewInfo.x;
	float zFar = u_ViewInfo.y;
	// eye-space depth along the view axis; the sky is a fixed distance away
	float zEye = d >= 0.99999 ? u_FogDistance.w
		: 2.0 * zNear * zFar / (zFar + zNear - (d * 2.0 - 1.0) * (zFar - zNear));
	vec3 dir = u_ViewForward - u_ViewLeft * (ndc.x * u_NormalScale.x) + u_ViewUp * (ndc.y * u_NormalScale.y);
	vec3 P = u_ViewOrigin + dir * zEye;
	float rayLen = zEye * length(dir);

	float f = max(u_FogDistance.x, 1e-6);
	float h0 = max(u_ViewOrigin.z - u_FogDistance.y, 0.0);
	float h1 = max(P.z - u_FogDistance.y, 0.0);
	float e0 = exp(-f * h0), e1 = exp(-f * h1);
	float dh = h1 - h0;
	float od = u_Color.a * rayLen * (abs(dh) > 0.5 ? (e0 - e1) / (f * dh) : e0);
	float amount = 1.0 - exp(-od);

	float sun = pow(max(dot(normalize(dir), u_PrimaryLightOrigin.xyz), 0.0), 8.0);
	gl_FragColor = vec4(u_Color.rgb * (1.0 + u_FogDistance.z * sun), amount);
}
