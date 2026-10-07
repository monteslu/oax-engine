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

// interaction_vp.glsl: unified lighting (phase 5) vertex shader, written for
// this engine from the id Tech 4 interaction parameter list (light
// projection S/T/Q, falloff S, bump/diffuse/specular texture coordinates).
// Lighting is computed per pixel in world space, so this stage only
// transforms: position to clip space, and position/normal/tangent frame and
// texture coordinates for the fragment stage.
//
// Defines: ULIGHT_DEPTH (shadow map: position only), ULIGHT_VOLUME (stencil
// shadow volume: clamped to the far plane), ULIGHT_ZONEAMBIENT (the vertex color
// carries the zone ambient), USE_VERTEX_ANIMATION,
// USE_BONE_ANIMATION (MAX_GLSL_BONES).

// ULIGHT_TEXCOORDS: the texture coordinates are needed (every pass but the
// plain shadow depth one; the masked depth pass samples the diffuse alpha)
#if !defined(ULIGHT_DEPTH) || defined(ULIGHT_DEPTH_MASKED)
#define ULIGHT_TEXCOORDS
#endif

attribute vec3 attr_Position;
attribute vec3 attr_Normal;
#if !defined(ULIGHT_DEPTH)
attribute vec4 attr_Tangent;
#endif
#if defined(ULIGHT_TEXCOORDS)
attribute vec4 attr_TexCoord0;
#endif

#if defined(USE_VERTEX_ANIMATION)
attribute vec3 attr_Position2;
attribute vec3 attr_Normal2;
#if !defined(ULIGHT_DEPTH)
attribute vec4 attr_Tangent2;
#endif
uniform float  u_VertexLerp;
#elif defined(USE_BONE_ANIMATION)
attribute vec4 attr_BoneIndexes;
attribute vec4 attr_BoneWeights;
uniform mat4   u_BoneMatrix[MAX_GLSL_BONES];
#endif

uniform mat4   u_ModelViewProjectionMatrix;
uniform mat4   u_ModelMatrix;

uniform int    u_DeformGen;
uniform float  u_DeformParams[5];
uniform float  u_Time;

#if defined(ULIGHT_TEXCOORDS)
uniform vec4   u_DiffuseTexMatrix0;
uniform vec4   u_DiffuseTexMatrix1;
uniform vec4   u_DiffuseTexMatrix2;
uniform vec4   u_DiffuseTexMatrix3;
uniform vec4   u_DiffuseTexMatrix4;
uniform vec4   u_DiffuseTexMatrix5;
uniform vec4   u_DiffuseTexMatrix6;
uniform vec4   u_DiffuseTexMatrix7;

varying vec2   var_TexCoords;
#endif
#if !defined(ULIGHT_DEPTH)
varying vec3   var_Position;
varying vec3   var_Normal;
varying vec4   var_Tangent;     // w: bitangent sign
#endif
#if defined(ULIGHT_ZONEAMBIENT)
attribute vec4 attr_Color;
varying vec4   var_Color;       // the zone ambient of the vertex (step 7.5 B)
#endif

vec3 DeformPosition(const vec3 pos, const vec3 normal, const vec2 st)
{
	float base =      u_DeformParams[0];
	float amplitude = u_DeformParams[1];
	float phase =     u_DeformParams[2];
	float frequency = u_DeformParams[3];
	float spread =    u_DeformParams[4];

	if (u_DeformGen == 0)
		return pos;

	if (u_DeformGen == DGEN_BULGE)
		phase *= st.x;
	else
		phase += dot(pos.xyz, vec3(spread));

	float value = phase + (u_Time * frequency);
	float func;

	if (u_DeformGen == DGEN_WAVE_SIN)
		func = sin(value * 2.0 * M_PI);
	else if (u_DeformGen == DGEN_WAVE_SQUARE)
		func = sign(fract(0.5 - value));
	else if (u_DeformGen == DGEN_WAVE_TRIANGLE)
		func = abs(fract(value + 0.75) - 0.5) * 4.0 - 1.0;
	else if (u_DeformGen == DGEN_WAVE_SAWTOOTH)
		func = fract(value);
	else if (u_DeformGen == DGEN_WAVE_INVERSE_SAWTOOTH)
		func = (1.0 - fract(value));
	else
		func = sin(value);

	return pos + normal * (base + func * amplitude);
}

#if defined(ULIGHT_TEXCOORDS)
vec2 ModTexCoords(vec2 st, vec3 position, vec4 texMatrix[8])
{
	vec2 st2 = st;
	vec2 offsetPos = vec2(position.x + position.z, position.y);

	st2 = vec2(st2.x * texMatrix[0].x + st2.y * texMatrix[0].y + texMatrix[0].z,
	           st2.x * texMatrix[1].x + st2.y * texMatrix[1].y + texMatrix[1].z);
	st2 += texMatrix[0].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[1].w * 2.0 * M_PI));

	st2 = vec2(st2.x * texMatrix[2].x + st2.y * texMatrix[2].y + texMatrix[2].z,
	           st2.x * texMatrix[3].x + st2.y * texMatrix[3].y + texMatrix[3].z);
	st2 += texMatrix[2].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[3].w * 2.0 * M_PI));

	st2 = vec2(st2.x * texMatrix[4].x + st2.y * texMatrix[4].y + texMatrix[4].z,
	           st2.x * texMatrix[5].x + st2.y * texMatrix[5].y + texMatrix[5].z);
	st2 += texMatrix[4].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[5].w * 2.0 * M_PI));

	st2 = vec2(st2.x * texMatrix[6].x + st2.y * texMatrix[6].y + texMatrix[6].z,
	           st2.x * texMatrix[7].x + st2.y * texMatrix[7].y + texMatrix[7].z);
	st2 += texMatrix[6].w * sin(offsetPos * (2.0 * M_PI / 1024.0) + vec2(texMatrix[7].w * 2.0 * M_PI));

	return st2;
}
#endif

void main()
{
#if defined(USE_VERTEX_ANIMATION)
	vec3 position  = mix(attr_Position, attr_Position2, u_VertexLerp);
	vec3 normal    = mix(attr_Normal,   attr_Normal2,   u_VertexLerp);
  #if !defined(ULIGHT_DEPTH)
	vec3 tangent   = mix(attr_Tangent.xyz, attr_Tangent2.xyz, u_VertexLerp);
  #endif
#elif defined(USE_BONE_ANIMATION)
	mat4 vtxMat  = u_BoneMatrix[int(attr_BoneIndexes.x)] * attr_BoneWeights.x;
	     vtxMat += u_BoneMatrix[int(attr_BoneIndexes.y)] * attr_BoneWeights.y;
	     vtxMat += u_BoneMatrix[int(attr_BoneIndexes.z)] * attr_BoneWeights.z;
	     vtxMat += u_BoneMatrix[int(attr_BoneIndexes.w)] * attr_BoneWeights.w;
	mat3 nrmMat = mat3(cross(vtxMat[1].xyz, vtxMat[2].xyz), cross(vtxMat[2].xyz, vtxMat[0].xyz), cross(vtxMat[0].xyz, vtxMat[1].xyz));

	vec3 position  = vec3(vtxMat * vec4(attr_Position, 1.0));
	vec3 normal    = normalize(nrmMat * attr_Normal);
  #if !defined(ULIGHT_DEPTH)
	vec3 tangent   = normalize(nrmMat * attr_Tangent.xyz);
  #endif
#else
	vec3 position  = attr_Position;
	vec3 normal    = attr_Normal;
  #if !defined(ULIGHT_DEPTH)
	vec3 tangent   = attr_Tangent.xyz;
  #endif
#endif

#if defined(ULIGHT_TEXCOORDS)
	position = DeformPosition(position, normal, attr_TexCoord0.st);
#else
	position = DeformPosition(position, normal, vec2(0.0));
#endif

	gl_Position = u_ModelViewProjectionMatrix * vec4(position, 1.0);

#if defined(ULIGHT_VOLUME)
	// shadow volumes: clamp to the far plane (ES 3.0 has no depth clamp), so a
	// far cap past it still counts as behind the scene
	gl_Position.z = min(gl_Position.z, gl_Position.w * 0.999999);
#endif

#if defined(ULIGHT_TEXCOORDS)
	vec4 texMatrix[8];
	texMatrix[0] = u_DiffuseTexMatrix0;
	texMatrix[1] = u_DiffuseTexMatrix1;
	texMatrix[2] = u_DiffuseTexMatrix2;
	texMatrix[3] = u_DiffuseTexMatrix3;
	texMatrix[4] = u_DiffuseTexMatrix4;
	texMatrix[5] = u_DiffuseTexMatrix5;
	texMatrix[6] = u_DiffuseTexMatrix6;
	texMatrix[7] = u_DiffuseTexMatrix7;
	var_TexCoords = ModTexCoords(attr_TexCoord0.st, position, texMatrix);
#endif
#if !defined(ULIGHT_DEPTH)
	var_Position = (u_ModelMatrix * vec4(position, 1.0)).xyz;
	var_Normal   = (u_ModelMatrix * vec4(normal,   0.0)).xyz;
	var_Tangent  = vec4((u_ModelMatrix * vec4(tangent, 0.0)).xyz, attr_Tangent.w);
#if defined(ULIGHT_ZONEAMBIENT)
	var_Color    = attr_Color;
#endif
#endif
}
