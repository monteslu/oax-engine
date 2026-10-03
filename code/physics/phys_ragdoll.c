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

/*
===========================================================================
phys_ragdoll.c: a ragdoll from a list of bones.

One dynamic capsule body per bone, from the bone's head to its tail, all
created with identity rotation so a bone's pose is simply

    boneWorld = bodyWorld * inverse( bodyStart ) * boneStart

and gamecode can map the bodies back to skeleton joints with no table from
the engine. Each bone is joined to its parent at its own head:
  - BALL: a spherical joint, cone limit about the bone's start direction,
    twist limits about the bone;
  - HINGE: a revolute joint about the given axis (knees, elbows), limits
    measured from the start pose.
Joint friction is a motor with zero speed and a small max torque, as in
Box3D's own human sample (shared/human.c); an optional soft spring pulls
toward the start pose. Bones of one ragdoll share a negative group index so
they never collide with each other.
===========================================================================
*/

#include "phys_local.h"

static b3Vec3 Phys_Mid( const float *a, const float *b ) {
	b3Vec3 m;
	m.x = 0.5f * ( a[0] + b[0] );
	m.y = 0.5f * ( a[1] + b[1] );
	m.z = 0.5f * ( a[2] + b[2] );
	return m;
}

static b3Vec3 Phys_Dir( const float *from, const float *to, b3Vec3 fallback ) {
	b3Vec3 d;
	float len;

	d.x = to[0] - from[0];
	d.y = to[1] - from[1];
	d.z = to[2] - from[2];
	len = sqrtf( d.x * d.x + d.y * d.y + d.z * d.z );
	if ( len < 1e-4f ) {
		return fallback;
	}
	d.x /= len;
	d.y /= len;
	d.z /= len;
	return d;
}

int Phys_RagdollCreate( physOwner_t owner, int world, const oaxPhysRagdollDef_t *def,
		const oaxPhysRagdollBone_t *bones, int numBones, int *outBodies ) {
	physWorld_t *w = Phys_World( owner, world );
	const float metersCubed = PHYS_UNITS_PER_METER * PHYS_UNITS_PER_METER * PHYS_UNITS_PER_METER;
	const b3Vec3 zAxis = { 0.0f, 0.0f, 1.0f };
	b3BodyId ids[64];
	b3Vec3 mids[64];
	int i, made = 0;

	if ( !w || numBones < 1 || numBones > 64 ) {
		return 0;
	}
	for ( i = 0; i < numBones; i++ ) {
		outBodies[i] = 0;
		if ( bones[i].parent >= i ) {
			return 0;	// parents come first
		}
	}

	for ( i = 0; i < numBones; i++ ) {
		const oaxPhysRagdollBone_t *bone = &bones[i];
		b3BodyDef bd = b3DefaultBodyDef();
		b3ShapeDef sd = b3DefaultShapeDef();
		b3Capsule cap;
		b3Vec3 head, tail;

		mids[i] = Phys_Mid( bone->head, bone->tail );
		bd.type = b3_dynamicBody;
		bd.position = mids[i];
		bd.linearVelocity = Phys_Vec( def->velocity );
		bd.linearDamping = def->linearDamping;
		bd.angularDamping = def->angularDamping;
		ids[i] = b3CreateBody( w->id, &bd );

		head = b3Sub( Phys_Vec( bone->head ), mids[i] );
		tail = b3Sub( Phys_Vec( bone->tail ), mids[i] );
		cap.center1 = head;
		cap.center2 = tail;
		cap.radius = bone->radius > 0.5f ? bone->radius : 0.5f;
		sd.density = ( bone->density > 0.0f ? bone->density : 1000.0f ) / metersCubed;
		sd.baseMaterial.friction = def->friction > 0.0f ? def->friction : 0.6f;
		sd.baseMaterial.rollingResistance = 0.2f;
		sd.filter.groupIndex = def->groupIndex;
		sd.filter.categoryBits = def->categoryBits ? def->categoryBits : 1u;
		sd.filter.maskBits = def->maskBits ? def->maskBits : 0xffffffffu;
		sd.enableContactEvents = false;
		if ( b3Distance( head, tail ) < 0.01f ) {
			b3Sphere sph;
			sph.center = b3Vec3_zero;
			sph.radius = cap.radius;
			b3CreateSphereShape( ids[i], &sd, &sph );
		} else {
			b3CreateCapsuleShape( ids[i], &sd, &cap );
		}
		outBodies[i] = Phys_BodyRegister( owner, world, ids[i], def->userData );
		if ( !outBodies[i] ) {
			break;
		}
		made++;

		if ( bone->parent >= 0 && outBodies[bone->parent] ) {
			int p = bone->parent;
			b3Vec3 anchor = Phys_Vec( bone->head );
			b3Vec3 dir;
			b3Quat q;
			b3Transform fa, fb;

			if ( bone->jointType == PHYS_RAGDOLL_HINGE ) {
				{
					static const float zero[3] = { 0.0f, 0.0f, 0.0f };
					dir = Phys_Dir( zero, bone->axis, zAxis );
				}
			} else {
				dir = Phys_Dir( bone->head, bone->tail, zAxis );
			}
			q = b3ComputeQuatBetweenUnitVectors( zAxis, dir );
			fa.p = b3Sub( anchor, mids[p] );
			fa.q = q;
			fb.p = b3Sub( anchor, mids[i] );
			fb.q = q;

			if ( bone->jointType == PHYS_RAGDOLL_HINGE ) {
				b3RevoluteJointDef jd = b3DefaultRevoluteJointDef();
				jd.base.bodyIdA = ids[p];
				jd.base.bodyIdB = ids[i];
				jd.base.localFrameA = fa;
				jd.base.localFrameB = fb;
				jd.enableLimit = true;
				jd.lowerAngle = bone->lower;
				jd.upperAngle = bone->upper;
				jd.enableMotor = bone->friction > 0.0f;
				jd.maxMotorTorque = bone->friction;
				jd.enableSpring = def->jointHertz > 0.0f;
				jd.hertz = def->jointHertz;
				jd.dampingRatio = def->jointDampingRatio;
				b3CreateRevoluteJoint( w->id, &jd );
			} else {
				b3SphericalJointDef jd = b3DefaultSphericalJointDef();
				jd.base.bodyIdA = ids[p];
				jd.base.bodyIdB = ids[i];
				jd.base.localFrameA = fa;
				jd.base.localFrameB = fb;
				jd.enableConeLimit = bone->coneAngle > 0.0f;
				jd.coneAngle = bone->coneAngle;
				jd.enableTwistLimit = bone->upper > bone->lower;
				jd.lowerTwistAngle = bone->lower;
				jd.upperTwistAngle = bone->upper;
				jd.enableMotor = bone->friction > 0.0f;
				jd.maxMotorTorque = bone->friction;
				jd.enableSpring = def->jointHertz > 0.0f;
				jd.hertz = def->jointHertz;
				jd.dampingRatio = def->jointDampingRatio;
				b3CreateSphericalJoint( w->id, &jd );
			}
		}
	}
	return made;
}
