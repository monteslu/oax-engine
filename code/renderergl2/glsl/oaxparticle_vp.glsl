/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
/*
oaxparticle_vp.glsl: oax stateless particles (tr_oax_particle.c).

Adapted from DOOM-3 neo/framework/DeclParticle.cpp (idParticleStage::
ParticleOrigin, ParticleVerts, ParticleTexCoords, ParticleColors) and
neo/renderer/Model_prt.cpp (the per-index age, cycle and spawn-bunching
rules of idRenderModelPrt::InstantiateDynamicModel).
Changes: rewritten as a GLSL ES 3.00 vertex shader that evaluates one
particle quad corner per vertex from a static index buffer; an integer hash
per (seed, cycle, index) replaces idRandom; the sphere distribution's
rejection loop is bounded (8 tries); aimed particles are one quad; no cross
faded animation frames.
*/
// Every particle of a stage is a pure function of the stage parameters, the
// system seed, its index and the time.
//
// attr_Position: particle index, quad corner s, quad corner t
// u_P (see RB_SurfaceOAXParticles for the layout)
attribute vec3 attr_Position;

uniform float  u_P[72];
uniform mat4   u_ModelViewProjectionMatrix;
uniform vec3   u_ViewOrigin;
uniform vec3   u_ViewForward;
uniform vec3   u_ViewLeft;
uniform vec3   u_ViewUp;

varying vec2   var_TexCoords;
varying vec4   var_Color;
varying float  var_EyeZ;

uint rngState;

uint hashu(uint x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// idRandom::RandomFloat: [0, 1)
float RandomFloat()
{
	rngState = hashu(rngState + 0x9e3779b9u);
	return float(rngState >> 8) * (1.0 / 16777216.0);
}

// idRandom::CRandomFloat: [-1, 1)
float CRandomFloat()
{
	return 2.0 * RandomFloat() - 1.0;
}

float Integrate(float from, float to, float frac)
{
	return (from + frac * (to - from) * 0.5) * frac;
}

vec3 FxToWorld(vec3 v)
{
	return v.x * vec3(u_P[60], u_P[61], u_P[62]) + v.y * vec3(u_P[64], u_P[65], u_P[66]) + v.z * vec3(u_P[68], u_P[69], u_P[70]);
}

// idParticleStage::ParticleOrigin, in world space; consumes randoms from seed
vec3 ParticleOrigin(uint seed, float age, float frac)
{
	vec3 origin;
	float life = u_P[1] * 0.001;
	float scale = u_P[54];
	int pathType = int(u_P[19]);
	bool randomDistribution = u_P[9] != 0.0;

	rngState = seed;

	if (pathType == 0)
	{
		float radiusSqr;
		int distType = int(u_P[8]);
		vec4 dp = vec4(u_P[12], u_P[13], u_P[14], u_P[15]);

		if (distType == 0)
		{
			// rect ( sizeX sizeY sizeZ )
			origin.x = (randomDistribution ? CRandomFloat() : 1.0) * dp.x;
			origin.y = (randomDistribution ? CRandomFloat() : 1.0) * dp.y;
			origin.z = (randomDistribution ? CRandomFloat() : 1.0) * dp.z;
		}
		else if (distType == 1)
		{
			// cylinder ( sizeX sizeY sizeZ ringFraction )
			float angle1 = (randomDistribution ? CRandomFloat() : 1.0) * 2.0 * M_PI;
			origin.x = sin(angle1);
			origin.y = cos(angle1);
			origin.z = randomDistribution ? CRandomFloat() : 1.0;
			if (dp.w > 0.0)
			{
				radiusSqr = origin.x * origin.x + origin.y * origin.y;
				if (radiusSqr < dp.w * dp.w)
				{
					float f = sqrt(radiusSqr) / dp.w;
					float newRadius = dp.w + f * (1.0 - dp.w);
					origin.xy *= newRadius / max(f, 1e-6);
				}
			}
			origin *= dp.xyz;
		}
		else
		{
			// sphere ( sizeX sizeY sizeZ ringFraction ): rejection, bounded
			if (randomDistribution)
			{
				origin = vec3(0.0);
				radiusSqr = 0.0;
				for (int i = 0; i < 8; i++)
				{
					origin = vec3(CRandomFloat(), CRandomFloat(), CRandomFloat());
					radiusSqr = dot(origin, origin);
					if (radiusSqr <= 1.0)
						break;
				}
				if (radiusSqr > 1.0)
				{
					origin *= inversesqrt(radiusSqr);
					radiusSqr = 1.0;
				}
			}
			else
			{
				origin = vec3(1.0);
				radiusSqr = 3.0;
			}
			if (dp.w > 0.0 && radiusSqr < dp.w * dp.w)
			{
				float f = sqrt(radiusSqr) / dp.w;
				float newRadius = dp.w + f * (1.0 - dp.w);
				origin *= newRadius / max(f, 1e-6);
			}
			origin *= dp.xyz;
		}

		// offset affects every distribution, before velocity and gravity
		origin += vec3(u_P[28], u_P[29], u_P[30]);

		vec3 dir;
		if (int(u_P[10]) == 0)
		{
			// cone: parm0 is the full angle
			float angle1 = CRandomFloat() * u_P[16] * (M_PI / 180.0);
			float angle2 = CRandomFloat() * M_PI;
			float s1 = sin(angle1), c1 = cos(angle1);
			float s2 = sin(angle2), c2 = cos(angle2);
			dir = vec3(s1 * c2, s1 * s2, c1);
		}
		else
		{
			// outward, parm0 is an upward bias
			float l = length(origin);
			dir = l > 0.0 ? origin / l : vec3(0.0);
			dir.z += u_P[16];
		}

		origin += dir * Integrate(u_P[32], u_P[33], frac) * life;
	}
	else
	{
		// custom paths replace the origin and velocity, still use gravity
		float angle1, angle2, speed1, speed2;
		if (pathType == 1)
		{
			// helix ( sizeX sizeY sizeZ radialSpeed climbSpeed )
			speed1 = CRandomFloat();
			speed2 = CRandomFloat();
			angle1 = RandomFloat() * 2.0 * M_PI + u_P[23] * speed1 * age;
			origin.x = cos(angle1) * u_P[20];
			origin.y = sin(angle1) * u_P[21];
			origin.z = RandomFloat() * u_P[22] + u_P[24] * speed2 * age;
		}
		else if (pathType == 2)
		{
			// flies ( radialSpeed axialSpeed size )
			speed1 = clamp(CRandomFloat(), 0.4, 1.0);
			speed2 = clamp(CRandomFloat(), 0.4, 1.0);
			angle1 = RandomFloat() * M_PI * 2.0 + u_P[20] * speed1 * age;
			angle2 = RandomFloat() * M_PI * 2.0 + u_P[21] * speed1 * age;
			origin = vec3(cos(angle1) * cos(angle2), sin(angle1) * cos(angle2), -sin(angle2)) * u_P[22];
		}
		else if (pathType == 3)
		{
			// spherical orbit ( radius speed )
			angle1 = RandomFloat() * 2.0 * M_PI + u_P[21] * age;
			origin = vec3(cos(angle1) * u_P[20], sin(angle1) * u_P[20], 0.0);
		}
		else
		{
			// drip ( speed )
			origin = vec3(0.0, 0.0, -(age * u_P[20]));
		}
		origin += vec3(u_P[28], u_P[29], u_P[30]);
	}

	// system scale and axis, then gravity (DOOM-3: gravity * age^2)
	vec3 world = vec3(u_P[56], u_P[57], u_P[58]) + FxToWorld(origin * scale);
	if (u_P[18] != 0.0)
		world.z -= u_P[17] * scale * age * age;
	else
		world -= vec3(u_P[68], u_P[69], u_P[70]) * (u_P[17] * scale * age * age);
	return world;
}

void Kill()
{
	gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
	var_TexCoords = vec2(0.0);
	var_Color = vec4(0.0);
	var_EyeZ = 0.0;
}

void main()
{
	float index = attr_Position.x;
	vec2 corner = attr_Position.yz;
	float count = u_P[0];
	float lifeMs = u_P[1];
	float cycleMsec = u_P[2];

	if (index >= count)
	{
		Kill();
		return;
	}

	// Model_prt.cpp: the particle's own age and cycle, rebased on the current
	// stage cycle so large clocks keep their precision
	float bunchOffset = floor(lifeMs * u_P[3] * index / count);
	float particleAge = u_P[5] - bunchOffset;
	float cycleOff = floor(particleAge / cycleMsec);
	float cycle = u_P[4] + cycleOff;
	float inCycleTime = particleAge - cycleOff * cycleMsec;

	if (cycle < 0.0 || (u_P[6] > 0.0 && cycle >= u_P[6]) || u_P[5] - inCycleTime >= u_P[7] || inCycleTime > lifeMs)
	{
		Kill();
		return;
	}

	float frac = inCycleTime / lifeMs;
	float age = frac * lifeMs * 0.001;
	uint seed = hashu(uint(u_P[53]) ^ hashu(uint(int(cycle)) * 0x45d9f3bu) ^ (uint(int(index)) * 0x27d4eb2du));

	// ParticleColors
	float fade = 1.0;
	if (frac < u_P[48])
		fade *= frac / u_P[48];
	if (1.0 - frac < u_P[49])
		fade *= (1.0 - frac) / u_P[49];
	if (u_P[50] > 0.0)
	{
		float indexFrac = (count - index) / count;
		if (indexFrac < u_P[50])
			fade *= indexFrac / u_P[50];
	}
	vec4 color = vec4(u_P[40], u_P[41], u_P[42], u_P[43]) * fade + vec4(u_P[44], u_P[45], u_P[46], u_P[47]) * (1.0 - fade);
	color = clamp(color, 0.0, 1.0);
	if (color == vec4(0.0))
	{
		Kill();
		return;
	}

	vec3 origin = ParticleOrigin(seed, age, frac);

	// ParticleTexCoords
	float s0 = 0.0, w = 1.0;
	float frames = u_P[51];
	if (frames > 1.0)
	{
		float floatFrame = u_P[52] != 0.0 ? age * u_P[52] : frac * frames;
		w = 1.0 / frames;
		s0 = w * floor(mod(floatFrame, frames));
	}
	var_TexCoords = vec2(s0 + corner.x * w, corner.y);

	// ParticleVerts
	float scale = u_P[54];
	float psize = mix(u_P[36], u_P[37], frac) * scale;
	float paspect = mix(u_P[38], u_P[39], frac);
	vec3 left, up;
	vec3 pos;
	int orientation = int(u_P[11]);

	if (orientation == 1)
	{
		// aimed: from where the particle was trailTime ago to where it is
		float oldAge = max(age - u_P[55], 0.0);
		vec3 oldOrigin = ParticleOrigin(seed, oldAge, oldAge / (lifeMs * 0.001));
		up = origin - oldOrigin;
		up -= dot(up, u_ViewForward) * u_ViewForward;
		float l = length(up);
		up = l > 0.0 ? up / l : u_ViewUp;
		left = cross(up, u_ViewForward) * psize;
		pos = mix(origin, oldOrigin, corner.y) + left * (corner.x * 2.0 - 1.0);
	}
	else
	{
		// constant rotation; half the particles turn each way
		float angle = u_P[31] != 0.0 ? u_P[31] : 360.0 * RandomFloat();
		float angleMove = Integrate(u_P[34], u_P[35], frac) * lifeMs * 0.001;
		if (mod(index, 2.0) >= 1.0)
			angle += angleMove;
		else
			angle -= angleMove;
		angle = angle / 180.0 * M_PI;
		float c = cos(angle), s = sin(angle);

		if (orientation == 4)
		{
			left = FxToWorld(vec3(s, c, 0.0));
			up = FxToWorld(vec3(c, -s, 0.0));
		}
		else if (orientation == 2)
		{
			left = FxToWorld(vec3(0.0, c, s));
			up = FxToWorld(vec3(0.0, -s, c));
		}
		else if (orientation == 3)
		{
			left = FxToWorld(vec3(c, 0.0, s));
			up = FxToWorld(vec3(-s, 0.0, c));
		}
		else
		{
			left = u_ViewLeft * c + u_ViewUp * s;
			up = u_ViewUp * c - u_ViewLeft * s;
		}
		left *= psize;
		up *= psize * paspect;
		// 0 = origin - left + up, 1 = + left + up, 2 = - left - up, 3 = + left - up
		pos = origin + left * (corner.x * 2.0 - 1.0) + up * (1.0 - corner.y * 2.0);
	}

	var_Color = color;
	var_EyeZ = dot(pos - u_ViewOrigin, u_ViewForward);
	gl_Position = u_ModelViewProjectionMatrix * vec4(pos, 1.0);
}
