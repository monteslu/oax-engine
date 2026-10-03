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
phys_syscalls.c: the physics syscall block (1200-1249), shared by the game
and the cgame (qcommon/oax.h oaxPhysImport_t, structs in oax_phys.h).

The server registers SV_PhysGameCalls and the client CL_PhysCgameCalls;
both land here with their owner, so a VM can only reach its own worlds.
Every pointer from the VM is bounds checked with VM_CheckBlock; a pointer
argument of 0 means "none" where the ABI allows it.
===========================================================================
*/

#include "phys_local.h"
#include "../qcommon/oax.h"

#define CHECK( n, size, what ) VM_CheckBlock( args[n], (int)( size ), what )
#define OPT( n ) ( args[n] ? VMA( n ) : NULL )

static int Phys_FloatBits( float f ) {
	floatint_t fi;
	fi.f = f;
	return fi.i;
}

typedef struct {
	physOwner_t	owner;
	int			ignoreBody;
	oaxPhysHit_t *hit;
} physCastCtx_t;

static float Phys_CastHit( b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t userMaterialId,
		int triangleIndex, int childIndex, void *context ) {
	physCastCtx_t *ctx = context;
	b3BodyId body = b3Shape_GetBody( shapeId );
	int handle = (int)(intptr_t)b3Body_GetUserData( body );
	physBody_t *b;

	(void)userMaterialId; (void)triangleIndex; (void)childIndex;
	if ( ctx->ignoreBody && handle == ctx->ignoreBody ) {
		return -1.0f;
	}
	// equal fractions: the lower body handle wins, so the answer never
	// depends on the order the tree hands shapes over
	if ( ctx->hit->hit && ( fraction > ctx->hit->fraction
		|| ( fraction == ctx->hit->fraction && handle >= ctx->hit->body ) ) ) {
		return ctx->hit->fraction;
	}
	ctx->hit->hit = 1;
	ctx->hit->fraction = fraction;
	ctx->hit->point[0] = point.x;
	ctx->hit->point[1] = point.y;
	ctx->hit->point[2] = point.z;
	ctx->hit->normal[0] = normal.x;
	ctx->hit->normal[1] = normal.y;
	ctx->hit->normal[2] = normal.z;
	ctx->hit->body = handle;
	b = Phys_Body( ctx->owner, handle );
	ctx->hit->bodyUserData = b ? b->userData : 0;
	ctx->hit->shapeUserData = (int)(intptr_t)b3Shape_GetUserData( shapeId );
	return fraction;
}

static b3QueryFilter Phys_QueryFilter( unsigned maskBits ) {
	b3QueryFilter f = b3DefaultQueryFilter();
	f.categoryBits = B3_DEFAULT_CATEGORY_BITS;
	f.maskBits = maskBits ? maskBits : B3_DEFAULT_MASK_BITS;
	return f;
}

static int Phys_Raycast( physOwner_t owner, physWorld_t *w, const oaxPhysRay_t *ray, oaxPhysHit_t *out ) {
	physCastCtx_t ctx;
	b3Vec3 start = Phys_Vec( ray->start );
	b3Vec3 delta = b3Sub( Phys_Vec( ray->end ), start );

	Com_Memset( out, 0, sizeof( *out ) );
	out->fraction = 1.0f;
	VectorCopy( ray->end, out->point );
	if ( b3Dot( delta, delta ) <= 0.0f ) {
		return 0;
	}
	ctx.owner = owner;
	ctx.ignoreBody = ray->ignoreBody;
	ctx.hit = out;
	b3World_CastRay( w->id, start, delta, Phys_QueryFilter( ray->maskBits ), Phys_CastHit, &ctx );
	return out->hit;
}

// a shape def as a query proxy: points relative to `origin`, rotated by q
static qboolean Phys_Proxy( const oaxPhysShapeDef_t *def, const float *points, int numPoints, b3Quat q,
		b3Vec3 *buf, b3ShapeProxy *proxy ) {
	b3Transform xf;
	int i, n = 0;

	proxy->radius = 0.0f;
	xf.p = Phys_Vec( def->offset );
	xf.q = Phys_Quat( def->quat );
	switch ( def->type ) {
	case PHYS_SHAPE_SPHERE:
		buf[n++] = Phys_Vec( def->params );
		proxy->radius = def->params[3];
		xf = b3Transform_identity;
		break;
	case PHYS_SHAPE_CAPSULE:
		buf[n++] = Phys_Vec( def->params );
		buf[n++] = Phys_Vec( def->params + 3 );
		proxy->radius = def->params[6];
		xf = b3Transform_identity;
		break;
	case PHYS_SHAPE_BOX:
		for ( i = 0; i < 8; i++ ) {
			buf[n].x = ( i & 1 ) ? def->params[0] : -def->params[0];
			buf[n].y = ( i & 2 ) ? def->params[1] : -def->params[1];
			buf[n].z = ( i & 4 ) ? def->params[2] : -def->params[2];
			n++;
		}
		break;
	case PHYS_SHAPE_HULL:
		if ( !points || numPoints < 1 || numPoints > B3_MAX_SHAPE_CAST_POINTS ) {
			return qfalse;
		}
		for ( i = 0; i < numPoints; i++ ) {
			buf[n++] = Phys_Vec( points + i * 3 );
		}
		break;
	default:
		return qfalse;
	}
	for ( i = 0; i < n; i++ ) {
		buf[i] = b3RotateVector( q, b3TransformPoint( xf, buf[i] ) );
	}
	proxy->points = buf;
	proxy->count = n;
	return qtrue;
}

typedef struct {
	physOwner_t	owner;
	int			*bodies;
	int			count, max;
} physOverlapCtx_t;

static bool Phys_OverlapHit( b3ShapeId shapeId, void *context ) {
	physOverlapCtx_t *ctx = context;
	int handle = (int)(intptr_t)b3Body_GetUserData( b3Shape_GetBody( shapeId ) );
	int i, j;

	for ( i = 0; i < ctx->count; i++ ) {
		if ( ctx->bodies[i] == handle ) {
			return true;
		}
	}
	if ( ctx->count < ctx->max ) {
		// keep the list sorted: the tree's visiting order is not part of the answer
		for ( i = 0; i < ctx->count && ctx->bodies[i] < handle; i++ ) {
		}
		for ( j = ctx->count; j > i; j-- ) {
			ctx->bodies[j] = ctx->bodies[j - 1];
		}
		ctx->bodies[i] = handle;
		ctx->count++;
	}
	return true;
}

static void Phys_SetBodyParam( physBody_t *b, int param, float value ) {
	switch ( param ) {
	case PHYS_BP_LINEAR_DAMPING: b3Body_SetLinearDamping( b->id, value ); break;
	case PHYS_BP_ANGULAR_DAMPING: b3Body_SetAngularDamping( b->id, value ); break;
	case PHYS_BP_GRAVITY_SCALE: b3Body_SetGravityScale( b->id, value ); break;
	case PHYS_BP_AWAKE: b3Body_SetAwake( b->id, value != 0.0f ); break;
	case PHYS_BP_ENABLED:
		if ( value != 0.0f ) b3Body_Enable( b->id ); else b3Body_Disable( b->id );
		break;
	case PHYS_BP_TYPE:
		b3Body_SetType( b->id, value == PHYS_BODY_DYNAMIC ? b3_dynamicBody
			: value == PHYS_BODY_KINEMATIC ? b3_kinematicBody : b3_staticBody );
		break;
	case PHYS_BP_BULLET: b3Body_SetBullet( b->id, value != 0.0f ); break;
	case PHYS_BP_USERDATA: b->userData = (int)value; break;
	case PHYS_BP_SLEEP_ENABLED: b3Body_EnableSleep( b->id, value != 0.0f ); break;
	}
}

static void Phys_SetJointParam( physJoint_t *j, int param, float v ) {
	b3JointType type = b3Joint_GetType( j->id );

	switch ( type ) {
	case b3_revoluteJoint:
		switch ( param ) {
		case PHYS_JP_MOTOR_SPEED: b3RevoluteJoint_SetMotorSpeed( j->id, v ); break;
		case PHYS_JP_MAX_MOTOR_FORCE: b3RevoluteJoint_SetMaxMotorTorque( j->id, v ); break;
		case PHYS_JP_SPRING_HERTZ: b3RevoluteJoint_SetSpringHertz( j->id, v ); break;
		case PHYS_JP_SPRING_DAMPING: b3RevoluteJoint_SetSpringDampingRatio( j->id, v ); break;
		case PHYS_JP_TARGET: b3RevoluteJoint_SetTargetAngle( j->id, v ); break;
		case PHYS_JP_LOWER: b3RevoluteJoint_SetLimits( j->id, v, b3RevoluteJoint_GetUpperLimit( j->id ) ); break;
		case PHYS_JP_UPPER: b3RevoluteJoint_SetLimits( j->id, b3RevoluteJoint_GetLowerLimit( j->id ), v ); break;
		case PHYS_JP_ENABLE_MOTOR: b3RevoluteJoint_EnableMotor( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_LIMIT: b3RevoluteJoint_EnableLimit( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_SPRING: b3RevoluteJoint_EnableSpring( j->id, v != 0.0f ); break;
		}
		break;
	case b3_prismaticJoint:
		switch ( param ) {
		case PHYS_JP_MOTOR_SPEED: b3PrismaticJoint_SetMotorSpeed( j->id, v ); break;
		case PHYS_JP_MAX_MOTOR_FORCE: b3PrismaticJoint_SetMaxMotorForce( j->id, v ); break;
		case PHYS_JP_SPRING_HERTZ: b3PrismaticJoint_SetSpringHertz( j->id, v ); break;
		case PHYS_JP_SPRING_DAMPING: b3PrismaticJoint_SetSpringDampingRatio( j->id, v ); break;
		case PHYS_JP_TARGET: b3PrismaticJoint_SetTargetTranslation( j->id, v ); break;
		case PHYS_JP_LOWER: b3PrismaticJoint_SetLimits( j->id, v, b3PrismaticJoint_GetUpperLimit( j->id ) ); break;
		case PHYS_JP_UPPER: b3PrismaticJoint_SetLimits( j->id, b3PrismaticJoint_GetLowerLimit( j->id ), v ); break;
		case PHYS_JP_ENABLE_MOTOR: b3PrismaticJoint_EnableMotor( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_LIMIT: b3PrismaticJoint_EnableLimit( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_SPRING: b3PrismaticJoint_EnableSpring( j->id, v != 0.0f ); break;
		}
		break;
	case b3_wheelJoint:
		switch ( param ) {
		case PHYS_JP_MOTOR_SPEED: b3WheelJoint_SetSpinMotorSpeed( j->id, v ); break;
		case PHYS_JP_MAX_MOTOR_FORCE: b3WheelJoint_SetMaxSpinTorque( j->id, v ); break;
		case PHYS_JP_SPRING_HERTZ: b3WheelJoint_SetSuspensionHertz( j->id, v ); break;
		case PHYS_JP_SPRING_DAMPING: b3WheelJoint_SetSuspensionDampingRatio( j->id, v ); break;
		case PHYS_JP_TARGET: b3WheelJoint_SetTargetSteeringAngle( j->id, v ); break;
		case PHYS_JP_LOWER: b3WheelJoint_SetSuspensionLimits( j->id, v, b3WheelJoint_GetUpperSuspensionLimit( j->id ) ); break;
		case PHYS_JP_UPPER: b3WheelJoint_SetSuspensionLimits( j->id, b3WheelJoint_GetLowerSuspensionLimit( j->id ), v ); break;
		case PHYS_JP_ENABLE_MOTOR: b3WheelJoint_EnableSpinMotor( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_LIMIT: b3WheelJoint_EnableSuspensionLimit( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_SPRING: b3WheelJoint_EnableSuspension( j->id, v != 0.0f ); break;
		}
		break;
	case b3_sphericalJoint:
		switch ( param ) {
		case PHYS_JP_MAX_MOTOR_FORCE: b3SphericalJoint_SetMaxMotorTorque( j->id, v ); break;
		case PHYS_JP_SPRING_HERTZ: b3SphericalJoint_SetSpringHertz( j->id, v ); break;
		case PHYS_JP_SPRING_DAMPING: b3SphericalJoint_SetSpringDampingRatio( j->id, v ); break;
		case PHYS_JP_LOWER: b3SphericalJoint_SetTwistLimits( j->id, v, b3SphericalJoint_GetUpperTwistLimit( j->id ) ); break;
		case PHYS_JP_UPPER: b3SphericalJoint_SetTwistLimits( j->id, b3SphericalJoint_GetLowerTwistLimit( j->id ), v ); break;
		case PHYS_JP_ENABLE_MOTOR: b3SphericalJoint_EnableMotor( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_LIMIT: b3SphericalJoint_EnableConeLimit( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_SPRING: b3SphericalJoint_EnableSpring( j->id, v != 0.0f ); break;
		}
		break;
	case b3_distanceJoint:
		switch ( param ) {
		case PHYS_JP_MOTOR_SPEED: b3DistanceJoint_SetMotorSpeed( j->id, v ); break;
		case PHYS_JP_MAX_MOTOR_FORCE: b3DistanceJoint_SetMaxMotorForce( j->id, v ); break;
		case PHYS_JP_SPRING_HERTZ: b3DistanceJoint_SetSpringHertz( j->id, v ); break;
		case PHYS_JP_SPRING_DAMPING: b3DistanceJoint_SetSpringDampingRatio( j->id, v ); break;
		case PHYS_JP_LENGTH: b3DistanceJoint_SetLength( j->id, v ); break;
		case PHYS_JP_LOWER: b3DistanceJoint_SetLengthRange( j->id, v, b3DistanceJoint_GetMaxLength( j->id ) ); break;
		case PHYS_JP_UPPER: b3DistanceJoint_SetLengthRange( j->id, b3DistanceJoint_GetMinLength( j->id ), v ); break;
		case PHYS_JP_ENABLE_MOTOR: b3DistanceJoint_EnableMotor( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_LIMIT: b3DistanceJoint_EnableLimit( j->id, v != 0.0f ); break;
		case PHYS_JP_ENABLE_SPRING: b3DistanceJoint_EnableSpring( j->id, v != 0.0f ); break;
		}
		break;
	case b3_weldJoint:
		switch ( param ) {
		case PHYS_JP_SPRING_HERTZ: b3WeldJoint_SetLinearHertz( j->id, v ); break;
		case PHYS_JP_SPRING_DAMPING: b3WeldJoint_SetLinearDampingRatio( j->id, v ); break;
		}
		break;
	default:
		break;
	}
}

static float Phys_GetJointParam( physJoint_t *j, int param ) {
	b3JointType type = b3Joint_GetType( j->id );

	if ( param == PHYS_JP_CONSTRAINT_FORCE ) {
		return b3Length( b3Joint_GetConstraintForce( j->id ) );
	}
	switch ( type ) {
	case b3_revoluteJoint:
		switch ( param ) {
		case PHYS_JP_ANGLE: return b3RevoluteJoint_GetAngle( j->id );
		case PHYS_JP_MOTOR_SPEED: return b3RevoluteJoint_GetMotorSpeed( j->id );
		case PHYS_JP_MAX_MOTOR_FORCE: return b3RevoluteJoint_GetMaxMotorTorque( j->id );
		case PHYS_JP_MOTOR_OUTPUT: return b3RevoluteJoint_GetMotorTorque( j->id );
		}
		break;
	case b3_prismaticJoint:
		switch ( param ) {
		case PHYS_JP_TRANSLATION: return b3PrismaticJoint_GetTranslation( j->id );
		case PHYS_JP_SPEED: return b3PrismaticJoint_GetSpeed( j->id );
		case PHYS_JP_MOTOR_SPEED: return b3PrismaticJoint_GetMotorSpeed( j->id );
		case PHYS_JP_MAX_MOTOR_FORCE: return b3PrismaticJoint_GetMaxMotorForce( j->id );
		case PHYS_JP_MOTOR_OUTPUT: return b3PrismaticJoint_GetMotorForce( j->id );
		}
		break;
	case b3_wheelJoint:
		switch ( param ) {
		case PHYS_JP_SPEED: return b3WheelJoint_GetSpinSpeed( j->id );
		case PHYS_JP_MOTOR_SPEED: return b3WheelJoint_GetSpinMotorSpeed( j->id );
		case PHYS_JP_MAX_MOTOR_FORCE: return b3WheelJoint_GetMaxSpinTorque( j->id );
		case PHYS_JP_MOTOR_OUTPUT: return b3WheelJoint_GetSpinTorque( j->id );
		}
		break;
	case b3_sphericalJoint:
		switch ( param ) {
		case PHYS_JP_ANGLE: return b3SphericalJoint_GetConeAngle( j->id );
		case PHYS_JP_MAX_MOTOR_FORCE: return b3SphericalJoint_GetMaxMotorTorque( j->id );
		}
		break;
	case b3_distanceJoint:
		switch ( param ) {
		case PHYS_JP_LENGTH: return b3DistanceJoint_GetCurrentLength( j->id );
		case PHYS_JP_MOTOR_SPEED: return b3DistanceJoint_GetMotorSpeed( j->id );
		case PHYS_JP_MAX_MOTOR_FORCE: return b3DistanceJoint_GetMaxMotorForce( j->id );
		case PHYS_JP_MOTOR_OUTPUT: return b3DistanceJoint_GetMotorForce( j->id );
		}
		break;
	default:
		break;
	}
	return 0.0f;
}

/*
==================
Phys_Syscall
==================
*/
qboolean Phys_Syscall( physOwner_t owner, intptr_t *args, intptr_t *ret ) {
	physWorld_t *w;
	physBody_t *b;
	physJoint_t *j;

	if ( args[0] >= PHYS_VEHICLE_CREATE && args[0] < PHYS_VEHICLE_CREATE + 10 ) {
		*ret = 0;
		return Phys_VehicleSyscall( owner, args, ret );	// 1260-1269, phys_vehicle.c
	}
	if ( args[0] < OAX_PHYS_BASE || args[0] >= OAX_PHYS_BASE + 50 ) {
		return qfalse;
	}
	*ret = 0;
	switch ( args[0] ) {
	case PHYS_WORLD_CREATE:
		CHECK( 1, sizeof( oaxPhysWorldDef_t ), "PHYSWORLD" );
		*ret = Phys_WorldCreate( owner, VMA( 1 ) );
		return qtrue;
	case PHYS_WORLD_DESTROY:
		Phys_WorldDestroy( owner, args[1] );
		return qtrue;
	case PHYS_WORLD_STEP:
		*ret = Phys_WorldStep( owner, args[1], args[2] );
		return qtrue;
	case PHYS_WORLD_ADD_BSP:
		if ( args[4] ) CHECK( 4, sizeof( oaxPhysShapeDef_t ), "PHYSBSP" );
		*ret = Phys_AddBSP( owner, args[1], args[2], args[3], OPT( 4 ) );
		return qtrue;
	case PHYS_WORLD_ADD_HEIGHTFIELD: {
		const oaxPhysHeightField_t *hf;
		CHECK( 2, sizeof( oaxPhysHeightField_t ), "PHYSHF" );
		hf = VMA( 2 );
		if ( hf->countX < 2 || hf->countY < 2 || hf->countX > 4096 || hf->countY > 4096 ) {
			return qtrue;
		}
		CHECK( 3, sizeof( float ) * hf->countX * hf->countY, "PHYSHFDATA" );
		if ( args[4] ) CHECK( 4, sizeof( oaxPhysShapeDef_t ), "PHYSHFMAT" );
		*ret = Phys_AddHeightField( owner, args[1], hf, VMA( 3 ), OPT( 4 ) );
		return qtrue;
	}
	case PHYS_WORLD_SET_GRAVITY:
		CHECK( 2, sizeof( vec3_t ), "PHYSGRAV" );
		if ( ( w = Phys_World( owner, args[1] ) ) ) {
			b3World_SetGravity( w->id, Phys_Vec( VMA( 2 ) ) );
		}
		return qtrue;
	case PHYS_WORLD_STATS:
		CHECK( 2, sizeof( oaxPhysStats_t ), "PHYSSTATS" );
		Phys_WorldStats( owner, args[1], VMA( 2 ) );
		*ret = Phys_World( owner, args[1] ) ? 1 : 0;
		return qtrue;
	case PHYS_WORLD_HASH:
		*ret = (int)Phys_WorldHash( owner, args[1] );
		return qtrue;
	case PHYS_WORLD_EXPLODE:
		CHECK( 2, sizeof( vec3_t ), "PHYSEXPLODE" );
		if ( ( w = Phys_World( owner, args[1] ) ) ) {
			b3ExplosionDef ed = b3DefaultExplosionDef();
			ed.position = Phys_Vec( VMA( 2 ) );
			ed.radius = VMF( 3 );
			ed.falloff = VMF( 4 );
			ed.impulsePerArea = VMF( 5 );
			ed.maskBits = args[6] ? (uint64_t)(uint32_t)args[6] : B3_DEFAULT_MASK_BITS;
			b3World_Explode( w->id, &ed );
		}
		return qtrue;
	case PHYS_WORLD_CONTACT_EVENTS:
		if ( args[3] <= 0 ) {
			return qtrue;
		}
		CHECK( 2, sizeof( oaxPhysContact_t ) * args[3], "PHYSEVENTS" );
		*ret = Phys_WorldContactEvents( owner, args[1], VMA( 2 ), args[3] );
		return qtrue;

	case PHYS_BODY_CREATE:
		CHECK( 2, sizeof( oaxPhysBodyDef_t ), "PHYSBODY" );
		*ret = Phys_BodyCreate( owner, args[1], VMA( 2 ) );
		return qtrue;
	case PHYS_BODY_DESTROY:
		Phys_BodyDestroy( owner, args[1] );
		return qtrue;
	case PHYS_BODY_ADD_SHAPE:
		CHECK( 2, sizeof( oaxPhysShapeDef_t ), "PHYSSHAPE" );
		if ( args[4] < 0 || args[4] > 65536 || args[6] < 0 || args[6] > 3 * 65536 ) {
			return qtrue;
		}
		if ( args[3] ) CHECK( 3, sizeof( float ) * 3 * args[4], "PHYSPOINTS" );
		if ( args[5] ) CHECK( 5, sizeof( int ) * args[6], "PHYSINDICES" );
		*ret = Phys_BodyAddShape( owner, args[1], VMA( 2 ), OPT( 3 ), args[4], OPT( 5 ), args[6] );
		return qtrue;
	case PHYS_BODY_SET_TRANSFORM:
		CHECK( 2, sizeof( vec3_t ), "PHYSORG" );
		if ( args[3] ) CHECK( 3, 4 * sizeof( float ), "PHYSQUAT" );
		if ( ( b = Phys_Body( owner, args[1] ) ) ) {
			b3Body_SetTransform( b->id, Phys_Vec( VMA( 2 ) ), Phys_Quat( OPT( 3 ) ) );
		}
		return qtrue;
	case PHYS_BODY_SET_VELOCITY:
		if ( args[2] ) CHECK( 2, sizeof( vec3_t ), "PHYSVEL" );
		if ( args[3] ) CHECK( 3, sizeof( vec3_t ), "PHYSAVEL" );
		if ( ( b = Phys_Body( owner, args[1] ) ) ) {
			if ( args[2] ) b3Body_SetLinearVelocity( b->id, Phys_Vec( VMA( 2 ) ) );
			if ( args[3] ) b3Body_SetAngularVelocity( b->id, Phys_Vec( VMA( 3 ) ) );
		}
		return qtrue;
	case PHYS_BODY_APPLY:
		CHECK( 3, sizeof( vec3_t ), "PHYSAPPLY" );
		if ( args[4] ) CHECK( 4, sizeof( vec3_t ), "PHYSAPPLYPT" );
		if ( ( b = Phys_Body( owner, args[1] ) ) ) {
			b3Vec3 v = Phys_Vec( VMA( 3 ) );
			switch ( args[2] ) {
			case PHYS_APPLY_FORCE:
				if ( args[4] ) b3Body_ApplyForce( b->id, v, Phys_Vec( VMA( 4 ) ), true );
				else b3Body_ApplyForceToCenter( b->id, v, true );
				break;
			case PHYS_APPLY_IMPULSE:
				if ( args[4] ) b3Body_ApplyLinearImpulse( b->id, v, Phys_Vec( VMA( 4 ) ), true );
				else b3Body_ApplyLinearImpulseToCenter( b->id, v, true );
				break;
			case PHYS_APPLY_TORQUE: b3Body_ApplyTorque( b->id, v, true ); break;
			case PHYS_APPLY_ANGULAR_IMPULSE: b3Body_ApplyAngularImpulse( b->id, v, true ); break;
			}
		}
		return qtrue;
	case PHYS_BODY_SET_TARGET:
		CHECK( 2, sizeof( vec3_t ), "PHYSTARGET" );
		if ( args[3] ) CHECK( 3, 4 * sizeof( float ), "PHYSTARGETQ" );
		if ( ( b = Phys_Body( owner, args[1] ) ) ) {
			b3WorldTransform t;
			t.p = Phys_Vec( VMA( 2 ) );
			t.q = Phys_Quat( OPT( 3 ) );
			b3Body_SetTargetTransform( b->id, t, VMF( 4 ) > 0.0f ? VMF( 4 ) : 0.016f, true );
		}
		return qtrue;
	case PHYS_BODY_SET_PARAM:
		if ( ( b = Phys_Body( owner, args[1] ) ) ) {
			Phys_SetBodyParam( b, args[2], VMF( 3 ) );
		}
		return qtrue;
	case PHYS_BODY_GET_STATE:
		CHECK( 2, sizeof( oaxPhysBodyState_t ), "PHYSSTATE" );
		*ret = Phys_BodyState( owner, args[1], VMA( 2 ) );
		return qtrue;
	case PHYS_BODY_GET_STATES: {
		const int *list;
		oaxPhysBodyState_t *out;
		int i, n = args[2];
		if ( n <= 0 || n > PHYS_MAX_BODIES ) {
			return qtrue;
		}
		CHECK( 1, sizeof( int ) * n, "PHYSSTATESIN" );
		CHECK( 3, sizeof( oaxPhysBodyState_t ) * n, "PHYSSTATES" );
		list = VMA( 1 );
		out = VMA( 3 );
		for ( i = 0; i < n; i++ ) {
			*ret += Phys_BodyState( owner, list[i], &out[i] );
		}
		return qtrue;
	}
	case PHYS_BODY_FROM_BSP_MODEL:
		if ( args[4] ) CHECK( 4, sizeof( oaxPhysShapeDef_t ), "PHYSBMODEL" );
		*ret = Phys_BodyFromBSPModel( owner, args[1], args[2], args[3], OPT( 4 ) );
		return qtrue;
	case PHYS_BODY_GET_MASS:
		*ret = ( b = Phys_Body( owner, args[1] ) ) ? Phys_FloatBits( b3Body_GetMass( b->id ) ) : 0;
		return qtrue;
	case PHYS_RAGDOLL_CREATE:
		if ( args[4] < 1 || args[4] > 64 ) {
			return qtrue;
		}
		CHECK( 2, sizeof( oaxPhysRagdollDef_t ), "PHYSRAGDOLL" );
		CHECK( 3, sizeof( oaxPhysRagdollBone_t ) * args[4], "PHYSBONES" );
		CHECK( 5, sizeof( int ) * args[4], "PHYSRAGBODIES" );
		*ret = Phys_RagdollCreate( owner, args[1], VMA( 2 ), VMA( 3 ), args[4], VMA( 5 ) );
		return qtrue;

	case PHYS_JOINT_CREATE:
		CHECK( 2, sizeof( oaxPhysJointDef_t ), "PHYSJOINT" );
		*ret = Phys_JointCreate( owner, args[1], VMA( 2 ) );
		return qtrue;
	case PHYS_JOINT_DESTROY:
		Phys_JointDestroy( owner, args[1] );
		return qtrue;
	case PHYS_JOINT_SET_PARAM:
		if ( ( j = Phys_Joint( owner, args[1] ) ) ) {
			Phys_SetJointParam( j, args[2], VMF( 3 ) );
		}
		return qtrue;
	case PHYS_JOINT_GET_PARAM:
		*ret = ( j = Phys_Joint( owner, args[1] ) ) ? Phys_FloatBits( Phys_GetJointParam( j, args[2] ) ) : 0;
		return qtrue;

	case PHYS_RAYCAST:
		CHECK( 2, sizeof( oaxPhysRay_t ), "PHYSRAY" );
		CHECK( 3, sizeof( oaxPhysHit_t ), "PHYSHIT" );
		if ( ( w = Phys_World( owner, args[1] ) ) ) {
			*ret = Phys_Raycast( owner, w, VMA( 2 ), VMA( 3 ) );
		}
		return qtrue;
	case PHYS_RAYCAST_BATCH: {
		const oaxPhysRay_t *rays;
		oaxPhysHit_t *hits;
		int i, n = args[3];
		if ( n <= 0 || n > 4096 ) {
			return qtrue;
		}
		CHECK( 2, sizeof( oaxPhysRay_t ) * n, "PHYSRAYS" );
		CHECK( 4, sizeof( oaxPhysHit_t ) * n, "PHYSHITS" );
		if ( !( w = Phys_World( owner, args[1] ) ) ) {
			return qtrue;
		}
		rays = VMA( 2 );
		hits = VMA( 4 );
		for ( i = 0; i < n; i++ ) {
			*ret += Phys_Raycast( owner, w, &rays[i], &hits[i] );
		}
		return qtrue;
	}
	case PHYS_SHAPECAST: {
		const oaxPhysShapeDef_t *def;
		const oaxPhysRay_t *ray;
		oaxPhysHit_t *out;
		b3Vec3 buf[B3_MAX_SHAPE_CAST_POINTS];
		b3ShapeProxy proxy;
		physCastCtx_t ctx;
		b3Vec3 start, delta;
		CHECK( 2, sizeof( oaxPhysShapeDef_t ), "PHYSCASTSHAPE" );
		if ( args[4] < 0 || args[4] > B3_MAX_SHAPE_CAST_POINTS ) {
			return qtrue;
		}
		if ( args[3] ) CHECK( 3, sizeof( float ) * 3 * args[4], "PHYSCASTPTS" );
		CHECK( 5, sizeof( oaxPhysRay_t ), "PHYSCASTRAY" );
		if ( args[6] ) CHECK( 6, 4 * sizeof( float ), "PHYSCASTQ" );
		CHECK( 7, sizeof( oaxPhysHit_t ), "PHYSCASTHIT" );
		def = VMA( 2 );
		ray = VMA( 5 );
		out = VMA( 7 );
		Com_Memset( out, 0, sizeof( *out ) );
		out->fraction = 1.0f;
		VectorCopy( ray->end, out->point );
		if ( !( w = Phys_World( owner, args[1] ) ) || !Phys_Proxy( def, OPT( 3 ), args[4], Phys_Quat( OPT( 6 ) ), buf, &proxy ) ) {
			return qtrue;
		}
		start = Phys_Vec( ray->start );
		delta = b3Sub( Phys_Vec( ray->end ), start );
		ctx.owner = owner;
		ctx.ignoreBody = ray->ignoreBody;
		ctx.hit = out;
		b3World_CastShape( w->id, start, &proxy, delta, Phys_QueryFilter( ray->maskBits ), Phys_CastHit, &ctx );
		*ret = out->hit;
		return qtrue;
	}
	case PHYS_OVERLAP: {
		const oaxPhysShapeDef_t *def;
		b3Vec3 buf[B3_MAX_SHAPE_CAST_POINTS];
		b3ShapeProxy proxy;
		physOverlapCtx_t ctx;
		CHECK( 2, sizeof( oaxPhysShapeDef_t ), "PHYSOVSHAPE" );
		if ( args[4] < 0 || args[4] > B3_MAX_SHAPE_CAST_POINTS || args[9] <= 0 || args[9] > PHYS_MAX_BODIES ) {
			return qtrue;
		}
		if ( args[3] ) CHECK( 3, sizeof( float ) * 3 * args[4], "PHYSOVPTS" );
		CHECK( 5, sizeof( vec3_t ), "PHYSOVORG" );
		if ( args[6] ) CHECK( 6, 4 * sizeof( float ), "PHYSOVQ" );
		CHECK( 8, sizeof( int ) * args[9], "PHYSOVOUT" );
		def = VMA( 2 );
		if ( !( w = Phys_World( owner, args[1] ) ) || !Phys_Proxy( def, OPT( 3 ), args[4], Phys_Quat( OPT( 6 ) ), buf, &proxy ) ) {
			return qtrue;
		}
		ctx.owner = owner;
		ctx.bodies = VMA( 8 );
		ctx.count = 0;
		ctx.max = args[9];
		b3World_OverlapShape( w->id, Phys_Vec( VMA( 5 ) ), &proxy, Phys_QueryFilter( (unsigned)args[7] ), Phys_OverlapHit, &ctx );
		*ret = ctx.count;
		return qtrue;
	}
	}
	return qfalse;
}

/*
==============================================================================
entry points for the server and client (phys_public.h)
==============================================================================
*/

qboolean Phys_GameCalls( intptr_t *args, intptr_t *ret ) {
	return Phys_Syscall( PHYS_OWNER_GAME, args, ret );
}

qboolean Phys_CgameCalls( intptr_t *args, intptr_t *ret ) {
	return Phys_Syscall( PHYS_OWNER_CGAME, args, ret );
}

void Phys_FreeGameWorlds( void ) {
	Phys_FreeOwner( PHYS_OWNER_GAME );
}

void Phys_FreeCgameWorlds( void ) {
	Phys_FreeOwner( PHYS_OWNER_CGAME );
}
