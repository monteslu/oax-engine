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

// oax procedural textures (tr_procedural.c). One program per pass type,
// chosen by PROC_* defines. Every pass addresses texels by gl_FragCoord
// and texelFetch, and every random choice is an integer hash of
// (seed, step, texel), so a step computes the same everywhere.
//   u_TextureMap  previous state (ping-pong passes)
//   u_LevelsMap   source image (water, wet, ice)
//   u_ViewInfo    step, seed low 16 bits, seed high 16 bits, 1 / size
//   u_Time        step / 30 (seconds)
//   u_Color       per-type parameters
precision highp int;

uniform sampler2D u_TextureMap;
uniform sampler2D u_LevelsMap;

uniform vec4   u_ViewInfo;
uniform float  u_Time;
uniform vec4   u_Color;

varying vec2   var_TexCoords;

const float TAU = 6.28318530718;

uint Hash(uint a, uint b, uint c)
{
	uint h = a * 0x9E3779B9u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 15;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

float Rnd(uint h)
{
	return float(h >> 8) * (1.0 / 16777216.0);
}

int Size()
{
	return int(1.0 / u_ViewInfo.w + 0.5);
}

uint Seed()
{
	return uint(u_ViewInfo.y) | (uint(u_ViewInfo.z) << 16);
}

uint Step()
{
	return uint(u_ViewInfo.x);
}

vec4 State(ivec2 p)
{
	int n = Size();
	return texelFetch(u_TextureMap, ivec2(p.x & (n - 1), p.y & (n - 1)), 0);
}

// value noise on an integer lattice that wraps every `period` cells
float Noise(vec2 x, int period)
{
	vec2 i = floor(x);
	vec2 f = x - i;
	ivec2 c = ivec2(i);
	uint s = Seed();
	float a = Rnd(Hash(s, uint(c.x & (period - 1)), uint(c.y & (period - 1))));
	float b = Rnd(Hash(s, uint((c.x + 1) & (period - 1)), uint(c.y & (period - 1))));
	float d = Rnd(Hash(s, uint(c.x & (period - 1)), uint((c.y + 1) & (period - 1))));
	float e = Rnd(Hash(s, uint((c.x + 1) & (period - 1)), uint((c.y + 1) & (period - 1))));
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	int n = Size();
	vec2 uv = (vec2(p) + 0.5) * u_ViewInfo.w;

#if defined(PROC_FIRE_STEP)
	// heat rises toward row 0 (t = 0, the top of the texture) and cools;
	// the bottom two rows are the spark bed. u_Color: cooling, spark rate
	float h = (State(p + ivec2(0, 1)).r * 2.0 + State(p + ivec2(-1, 1)).r + State(p + ivec2(1, 1)).r
		+ State(p + ivec2(0, 2)).r) * 0.2;
	float cool = u_Color.x * (0.5 + Rnd(Hash(Seed(), Step(), uint(p.y * n + p.x))));
	h = max(h - cool, 0.0);
	if (p.y >= n - 2)
	{
		uint spark = Hash(Seed() ^ 0x5bd1e995u, Step() / 2u, uint(p.x / 2));
		h = Rnd(spark) < u_Color.y ? 0.75 + 0.25 * Rnd(spark * 747796405u) : 0.0;
	}
	gl_FragColor = vec4(h, 0.0, 0.0, 1.0);
#elif defined(PROC_FIRE_COLOR)
	float h = State(p).r;
	vec3 c = clamp(vec3(h * 3.0, h * 3.0 - 1.0, h * 3.0 - 2.0), 0.0, 1.0);
	gl_FragColor = vec4(c, clamp(h * 4.0, 0.0, 1.0));
#elif defined(PROC_WATER_STEP)
	// damped wave equation, R = height, G = previous height; raindrops
	// land per step. u_Color: damping
	vec4 s = State(p);
	float sum = State(p + ivec2(1, 0)).r + State(p + ivec2(-1, 0)).r + State(p + ivec2(0, 1)).r + State(p + ivec2(0, -1)).r;
	float h = (sum * 0.5 - s.g) * u_Color.x;
	for (int k = 0; k < 2; k++)
	{
		uint d = Hash(Seed(), Step(), uint(1000 + k));
		if (Rnd(d) < 0.6)
		{
			ivec2 c = ivec2(int(Hash(d, 1u, 0u) & uint(n - 1)), int(Hash(d, 2u, 0u) & uint(n - 1)));
			ivec2 o = p - c;
			o = ivec2(o.x - n * int(floor(float(o.x) / float(n) + 0.5)), o.y - n * int(floor(float(o.y) / float(n) + 0.5)));
			if (o.x * o.x + o.y * o.y <= 4)
				h += 0.6;
		}
	}
	gl_FragColor = vec4(h, s.r, 0.0, 1.0);
#elif defined(PROC_WATER_COLOR)
	// the source refracted by the height gradient. u_Color: -, strength
	float gx = State(p + ivec2(1, 0)).r - State(p + ivec2(-1, 0)).r;
	float gy = State(p + ivec2(0, 1)).r - State(p + ivec2(0, -1)).r;
	vec3 c = texture2D(u_LevelsMap, uv + vec2(gx, gy) * u_Color.y).rgb;
	c += vec3(clamp(-(gx + gy) * 0.35, 0.0, 0.35));
	gl_FragColor = vec4(c, 1.0);
#elif defined(PROC_WET)
	// running water over the source. u_Color: amount, speed
	float t = u_Time * u_Color.y;
	vec2 o = vec2(sin(TAU * (3.0 * uv.y + 2.0 * uv.x) + t * 3.1) + 0.5 * sin(TAU * 7.0 * uv.y - t * 4.3),
		cos(TAU * (5.0 * uv.x - uv.y) + t * 2.3) + 0.5 * cos(TAU * 9.0 * uv.x + t * 1.7));
	vec3 c = texture2D(u_LevelsMap, uv + o * u_Color.x).rgb;
	c = c * 0.85 + vec3(0.08) * (0.5 + 0.5 * sin(TAU * 4.0 * uv.y - t * 5.0));
	gl_FragColor = vec4(c, 1.0);
#elif defined(PROC_ICE)
	// frosted, slowly drifting refraction. u_Color: amount, speed
	float t = u_Time * u_Color.y;
	vec2 q = uv * 8.0 + vec2(t, t * 0.6);
	vec2 o = vec2(Noise(q, 8), Noise(q + vec2(3.7, 1.9), 8)) * 2.0 - 1.0;
	vec3 c = texture2D(u_LevelsMap, uv + o * u_Color.x).rgb;
	float frost = Noise(uv * 32.0, 32);
	c = mix(c, vec3(0.80, 0.90, 1.0), 0.30 + 0.20 * frost);
	gl_FragColor = vec4(c, 1.0);
#elif defined(PROC_PLASMA)
	// u_Color: tint rgb
	float t = u_Time;
	float v = sin(TAU * 2.0 * uv.x + t * 1.3) + sin(TAU * 3.0 * uv.y - t * 0.9)
		+ sin(TAU * (2.0 * uv.x + 3.0 * uv.y) + t * 0.7) + sin(TAU * (uv.x - 2.0 * uv.y) - t * 1.1);
	vec3 c = 0.5 + 0.5 * cos(TAU * (v * 0.25 + vec3(0.0, 0.33, 0.67)));
	gl_FragColor = vec4(c * u_Color.rgb, 1.0);
#else
	gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);
#endif
}
