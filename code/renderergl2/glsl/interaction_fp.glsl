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

// interaction_fp.glsl: unified lighting (phase 5) fragment shader, written
// for this engine from the id Tech 4 interaction parameter list.
//
//   projection = projImage(S/Q, T/Q) * falloffImage(F, 0.5)   (zero outside)
//   diffuse    = diffuseMap * diffuseColor * max(N.L, 0)
//   specular   = specularMap * specularColor * pow(max(N.H, 0), 16) * 2
//   color      = (diffuse + specular) * projection * lightColor * shadow
//
// The 16 / 2 specular constants follow id Tech 4's specular table image.
//
// ULIGHT_PHYSICAL (step 7.5 B, docs/lights.md) replaces the projection with
// a spherical light described by measurable terms, x = d / radius:
//   v     = softcap( intensity * falloff(x) )          (zero at x >= 1)
//   light = min( v * angular * lightColor, ceiling )   per channel
//   color = diffuse * light + specular * v * lightColor
// falloff: piecewise-linear points, an image (red at (x, 0.5)), inverse
// square (m / max(x, m))^2, or 1 - smoothstep(x) (UE1); softcap: a quadratic
// knee of half width k around the cap c; angular: N.L, or 1 on the lit side.
//
// Defines: ULIGHT_DEPTH (shadow map), ULIGHT_REFLECT (oaxMetal's probe
// reflection), ULIGHT_AMBIENT (diffuse * ambient;
// with ULIGHT_ZONEAMBIENT the per-vertex zone ambient times u_AmbientLight),
// USE_SHADOW_CUBE (point lights), USE_SHADOW_2D (projected and parallel
// lights), SWIZZLE_NORMALMAP.

#if defined(ULIGHT_DEPTH)

void main()
{
	gl_FragColor = vec4(0.0);
}

#else

uniform sampler2D u_DiffuseMap;
uniform vec4      u_DiffuseColor;

varying vec2      var_TexCoords;
varying vec3      var_Position;
varying vec3      var_Normal;
varying vec4      var_Tangent;

#if defined(ULIGHT_REFLECT)

// oaxMetal (docs/materials.md): the metal's reflection of the nearest probe,
// added over its lit colour. The probe is a cubemap rendered at map load
// (R_RenderCubemapSide), in the render target's units, so it adds as is;
// rougher metal reads a blurrier mip. The face layout and the parallax term
// are the stock renderer's (lightall_fp): it samples reflect(E, N).
uniform samplerCube u_CubeMap;
uniform vec4      u_CubeMapInfo;    // (probe origin - eye) / radius, 1 / radius
uniform vec4      u_MetalParms;     // reflectance at normal incidence (rgb), roughness
uniform float     u_ReflectLod;     // the probe's blurriest mip to use
uniform vec4      u_GridLightDir;   // a lightmapped map's model: toward its grid light (xyz), on (w)
uniform vec3      u_GridLight;      // that light's colour
uniform vec3      u_ViewOrigin;
uniform sampler2D u_NormalMap;
uniform vec4      u_NormalScale;

void main()
{
	vec3 surfNormal = normalize(gl_FrontFacing ? -var_Normal : var_Normal);
	vec3 N = surfNormal;
	// a model without texture coordinates has no tangents: its normal alone
	// (normalizing a zero tangent would make every term NaN)
	vec3 t = var_Tangent.xyz - surfNormal * dot(surfNormal, var_Tangent.xyz);
	if (dot(t, t) > 1e-8)
	{
		vec3 tangent = normalize(t);
		vec3 bitangent = cross(surfNormal, tangent) * (var_Tangent.w < 0.0 ? -1.0 : 1.0);
		vec3 Nt;
#if defined(SWIZZLE_NORMALMAP)
		Nt.xy = texture(u_NormalMap, var_TexCoords).ag - vec2(0.5);
#else
		Nt.xy = texture(u_NormalMap, var_TexCoords).rg - vec2(0.5);
#endif
		Nt.xy *= u_NormalScale.xy;
		Nt.z = sqrt(clamp((0.25 - Nt.x * Nt.x) - Nt.y * Nt.y, 0.0, 1.0));
		N = normalize(mat3(tangent, bitangent, surfNormal) * Nt);
	}

	vec3 viewDir = u_ViewOrigin - var_Position;
	vec3 E = normalize(viewDir);
	float NE = clamp(dot(N, E), 0.0, 1.0);
	float rough = u_MetalParms.w;

	// Schlick's Fresnel, held down at grazing angles as roughness grows
	vec3 F0 = u_MetalParms.rgb * texture(u_DiffuseMap, var_TexCoords).rgb;
	float fc = pow(1.0 - NE, 5.0);
	vec3 F = F0 + (max(vec3(1.0 - rough), F0) - F0) * fc;

	vec3 R = reflect(E, N) + u_CubeMapInfo.xyz + u_CubeMapInfo.w * viewDir;
	vec3 env = textureLod(u_CubeMap, R, u_ReflectLod * rough).rgb;
	vec3 color = env * F;

	// a model on a lightmapped map has no light passes: the highlight of its
	// light grid's directed light (GGX, the visibility term taken as 1/4),
	// so the metal still shines where the light catches it
	if (u_GridLightDir.w > 0.5)
	{
		vec3 L = normalize(u_GridLightDir.xyz);
		vec3 H = normalize(L + E);
		float NL = max(dot(N, L), 0.0);
		float NH = max(dot(N, H), 0.0);
		float a = max(rough * rough, 0.003);
		float a2 = a * a;
		float d = NH * NH * (a2 - 1.0) + 1.0;
		float D = a2 / (3.14159265 * d * d);
		vec3 FL = F0 + (1.0 - F0) * pow(1.0 - max(dot(E, H), 0.0), 5.0);
		color += FL * min(D * 0.25, 32.0) * NL * u_GridLight;
	}
	gl_FragColor = vec4(color, 1.0);
}

#elif defined(ULIGHT_AMBIENT)

uniform vec3      u_AmbientLight;
#if defined(ULIGHT_ZONEAMBIENT)
varying vec4      var_Color;
#endif

void main()
{
	vec4 diffuse = texture(u_DiffuseMap, var_TexCoords) * u_DiffuseColor;
#if defined(ULIGHT_ZONEAMBIENT)
	gl_FragColor = vec4(diffuse.rgb * var_Color.rgb * u_AmbientLight, 1.0);
#else
	gl_FragColor = vec4(diffuse.rgb * u_AmbientLight, 1.0);
#endif
}

#else

uniform sampler2D u_NormalMap;
uniform sampler2D u_SpecularMap;
uniform sampler2D u_LightProjMap;
uniform sampler2D u_LightFalloffMap;

uniform vec4      u_LightProjS;
uniform vec4      u_LightProjT;
uniform vec4      u_LightProjQ;
uniform vec4      u_LightFalloffS;
uniform vec3      u_LightOriginW;
uniform vec3      u_LightColor;
uniform vec3      u_ViewOrigin;
uniform vec4      u_SpecularColor;
uniform vec4      u_NormalScale;
uniform vec4      u_ShadowParams;   // near (0: orthographic), far, depth bias (fraction), normal offset

#if defined(ULIGHT_PHYSICAL)
uniform vec4      u_PhysLight;      // 1 / radius, intensity, cap (0: none), knee half width
uniform vec4      u_PhysLight2;     // falloff mode (0 table, 1 image, 2 inverse square, 3 smoothstep), inverse square clamp (x), lambert (1) or none (0), ceiling
uniform vec2      u_PhysCurve[16];  // falloff points (x, y), x ascending
uniform vec4      u_PhysSpot;       // a cone: the direction it faces (xyz), 1 / (1 - cos(edge)) (w; 0: no cone)

// UE1 LE_StaticSpot, measured in UE1 (LightCone 32, 64, 128): with
// u = (1 - cos(angle off the axis)) / (1 - cos(edge)), the light is
// smoothstep(1 - u), none past the edge
float PhysCone(float cosAngle)
{
	return smoothstep(0.0, 1.0, 1.0 - (1.0 - cosAngle) * u_PhysSpot.w);
}
uniform int       u_PhysCurveCount;

float PhysFalloff(float x)
{
	if (u_PhysLight2.x > 2.5)
		return 1.0 - smoothstep(0.0, 1.0, x);
	if (u_PhysLight2.x > 1.5)
	{
		float m = u_PhysLight2.y;
		float r = max(x, m);
		return (m * m) / (r * r);
	}
	if (u_PhysLight2.x > 0.5)
		return texture(u_LightFalloffMap, vec2(x, 0.5)).r;
	vec2 prev = u_PhysCurve[0];
	if (x <= prev.x)
		return prev.y;
	for (int i = 1; i < 16; i++)
	{
		if (i >= u_PhysCurveCount)
			break;
		vec2 p = u_PhysCurve[i];
		if (x <= p.x)
			return mix(prev.y, p.y, (x - prev.x) / max(p.x - prev.x, 1e-6));
		prev = p;
	}
	return prev.y;
}

float PhysCap(float v)
{
	float c = u_PhysLight.z, k = u_PhysLight.w;
	if (c <= 0.0)
		return v;
	if (k <= 0.0)
		return min(v, c);
	if (v <= c - k)
		return v;
	if (v >= c + k)
		return c;
	float t = v - c + k;
	return v - t * t / (4.0 * k);
}
#endif

#if defined(USE_SHADOW_CUBE)
uniform samplerCubeShadow u_ShadowCube;
#elif defined(USE_SHADOW_2D)
uniform sampler2DShadow u_Shadow2D;
uniform mat4      u_ShadowMatrix;
#endif

// perspective depth of a point at distance m along the shadow view axis,
// pulled toward the light by a fraction of m (the depth bias)
float ShadowDepth(float m)
{
	float n = u_ShadowParams.x, f = u_ShadowParams.y;
	m *= 1.0 - u_ShadowParams.z;
	return 0.5 * ((f + n) / (f - n) - (2.0 * f * n) / ((f - n) * m)) + 0.5;
}

float Shadow(vec3 position, vec3 normal)
{
#if defined(USE_SHADOW_CUBE)
	// offset along the normal by about a shadow map texel at this distance
	vec3 d = position - u_LightOriginW;
	float dist = max(max(abs(d.x), abs(d.y)), abs(d.z));
	position += normal * (u_ShadowParams.w * dist);
	d = position - u_LightOriginW;
	float m = max(max(abs(d.x), abs(d.y)), abs(d.z));
	return texture(u_ShadowCube, vec4(d, ShadowDepth(m)));
#elif defined(USE_SHADOW_2D)
	// rows: S, T, depth, Q; u_ShadowParams.x == 0 marks an orthographic map
	vec4 sc = u_ShadowMatrix * vec4(position, 1.0);
	position += normal * (u_ShadowParams.w * (u_ShadowParams.x > 0.0 ? sc.w : 1.0));
	sc = u_ShadowMatrix * vec4(position, 1.0);
	float depth;
	if (u_ShadowParams.x > 0.0)
	{
		sc.xy /= sc.w;
		depth = ShadowDepth(sc.w);
	}
	else
	{
		depth = sc.z - u_ShadowParams.z;
	}
	if (sc.x <= 0.0 || sc.x >= 1.0 || sc.y <= 0.0 || sc.y >= 1.0)
		return 1.0;
	return texture(u_Shadow2D, vec3(sc.xy, depth));
#else
	return 1.0;
#endif
}

void main()
{
#if defined(ULIGHT_PHYSICAL)
	float physX = length(u_LightOriginW - var_Position) * u_PhysLight.x;
	if (physX >= 1.0)
		discard;
#else
	vec4 P = vec4(var_Position, 1.0);
	vec4 lp = vec4(dot(P, u_LightProjS), dot(P, u_LightProjT), dot(P, u_LightProjQ), dot(P, u_LightFalloffS));

	// zero outside the light volume (id Tech 4 clamps these images to zero)
	if (lp.z <= 0.0)
		discard;
	vec2 st = lp.xy / lp.z;
	if (st.x < 0.0 || st.x > 1.0 || st.y < 0.0 || st.y > 1.0 || lp.w < 0.0 || lp.w > 1.0)
		discard;

	vec3 proj = texture(u_LightProjMap, st).rgb * texture(u_LightFalloffMap, vec2(lp.w, 0.5)).rgb;
#endif

	// Q3 winds front faces the other way round: same test as lightall_fp
	vec3 surfNormal = normalize(gl_FrontFacing ? -var_Normal : var_Normal);
	vec3 tangent = normalize(var_Tangent.xyz - surfNormal * dot(surfNormal, var_Tangent.xyz));
	vec3 bitangent = cross(surfNormal, tangent) * (var_Tangent.w < 0.0 ? -1.0 : 1.0);

	vec3 N;
#if defined(SWIZZLE_NORMALMAP)
	N.xy = texture(u_NormalMap, var_TexCoords).ag - vec2(0.5);
#else
	N.xy = texture(u_NormalMap, var_TexCoords).rg - vec2(0.5);
#endif
	N.xy *= u_NormalScale.xy;
	N.z = sqrt(clamp((0.25 - N.x * N.x) - N.y * N.y, 0.0, 1.0));
	N = normalize(mat3(tangent, bitangent, surfNormal) * N);

	vec3 L = normalize(u_LightOriginW - var_Position);
	vec3 V = normalize(u_ViewOrigin - var_Position);
	vec3 H = normalize(L + V);

	float NL = max(dot(N, L), 0.0);
	float NH = max(dot(N, H), 0.0);
	float surfNL = dot(surfNormal, L);

#if defined(ULIGHT_PHYSICAL)
	vec4 diffuse = texture(u_DiffuseMap, var_TexCoords) * u_DiffuseColor;
	float v = PhysCap(u_PhysLight.y * PhysFalloff(physX));
	if (u_PhysSpot.w > 0.0)
		v *= PhysCone(dot(-L, u_PhysSpot.xyz));
	float ang = u_PhysLight2.z > 0.5 ? NL : step(0.0, surfNL);
	vec3 lightRGB = min(v * ang * u_LightColor, vec3(u_PhysLight2.w));
	vec3 color = diffuse.rgb * lightRGB;
	color += texture(u_SpecularMap, var_TexCoords).rgb * u_SpecularColor.rgb * (pow(NH, 16.0) * 2.0 * step(0.0, surfNL)) * v * u_LightColor;

	float shadow = 1.0;
	if (surfNL > 0.0)
		shadow = Shadow(var_Position, surfNormal);

	gl_FragColor = vec4(color * shadow, 1.0);
#else
	vec4 diffuse = texture(u_DiffuseMap, var_TexCoords) * u_DiffuseColor;
	vec3 color = diffuse.rgb * NL;
	color += texture(u_SpecularMap, var_TexCoords).rgb * u_SpecularColor.rgb * (pow(NH, 16.0) * 2.0 * step(0.0, surfNL));

	float shadow = 1.0;
	if (surfNL > 0.0)
		shadow = Shadow(var_Position, surfNormal);

	gl_FragColor = vec4(color * proj * u_LightColor * shadow, 1.0);
#endif
}

#endif
#endif
