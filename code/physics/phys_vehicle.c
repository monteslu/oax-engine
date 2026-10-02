/*
===========================================================================
phys_vehicle.c: raycast-wheel and hover vehicles in a physics world, and
heightmap terrain (OAX_TERRAIN) as static collision (phase 8).

A vehicle is one dynamic chassis body (a box) plus up to eight wheels or
thrusters that are not bodies: every fixed tick, before the world steps,
each wheel casts a ray down the chassis' up axis and, where it touches,
pushes the chassis with a spring-damper suspension and the tire's grip
(drive, brake and lateral forces inside a friction circle). A hover vehicle
casts the same rays from its thruster points, holds its hover height with
the springs, and steers and banks with torques. Everything runs inside
Phys_RunTicks, tick by tick, so the forces never depend on how gamecode
slices its frames, and gamecode only sets the controls (throttle, steer,
brakes) and reads the state back.

Determinism: the forces are plain float math (-ffp-contract=off), angles
use Box3D's own b3ComputeCosSin, the only root is sqrtf (correctly rounded
everywhere), ray hits are chosen by fraction with a body-handle tie break,
and vehicles run in handle order. Native and the cart give the same bits
(the vehicle-parity test).
===========================================================================
*/

#include "phys_local.h"
#include "../qcommon/cm_terrain.h"

#define PHYS_MAX_VEHICLES	64	// per owner

typedef struct {
	qboolean		inuse;
	int				world;
	int				body;			// chassis body handle
	oaxPhysVehicleDef_t def;
	oaxPhysVehicleInput_t input;
	// state
	float			steer;			// radians, current
	float			susp[OAX_PHYS_MAX_WHEELS];
	float			spin[OAX_PHYS_MAX_WHEELS];
	int				contacts;
	float			distance;
	float			flipTime;		// seconds spent tipped over
	float			lastPos[3];
	int				ticks;
} physVehicle_t;

static physVehicle_t phys_vehicles[PHYS_NUM_OWNERS][PHYS_MAX_VEHICLES];

static physVehicle_t *Phys_Vehicle( physOwner_t owner, int handle ) {
	physVehicle_t *v;

	if ( handle < 1 || handle > PHYS_MAX_VEHICLES ) {
		return NULL;
	}
	v = &phys_vehicles[owner][handle - 1];
	return v->inuse ? v : NULL;
}

/*
==============================================================================
creation
==============================================================================
*/

static int Phys_VehicleCreate( physOwner_t owner, int world, const oaxPhysVehicleDef_t *def ) {
	physWorld_t *w = Phys_World( owner, world );
	oaxPhysBodyDef_t bd;
	oaxPhysShapeDef_t sd;
	physBody_t *b;
	physVehicle_t *v = NULL;
	b3MassData md;
	float scale;
	int i;

	if ( !w || def->numWheels < 0 || def->numWheels > OAX_PHYS_MAX_WHEELS || !( def->mass > 0.0f )
		|| !( def->halfExtents[0] > 0.0f && def->halfExtents[1] > 0.0f && def->halfExtents[2] > 0.0f ) ) {
		return 0;
	}
	for ( i = 0; i < PHYS_MAX_VEHICLES; i++ ) {
		if ( !phys_vehicles[owner][i].inuse ) {
			v = &phys_vehicles[owner][i];
			break;
		}
	}
	if ( !v ) {
		return 0;
	}

	Com_Memset( &bd, 0, sizeof( bd ) );
	bd.type = PHYS_BODY_DYNAMIC;
	VectorCopy( def->origin, bd.origin );
	Com_Memcpy( bd.quat, def->quat, sizeof( bd.quat ) );
	bd.linearDamping = def->linearDamping;
	bd.angularDamping = def->angularDamping;
	bd.gravityScale = 1.0f;
	bd.flags = 0;	// a parked vehicle falls asleep (see Phys_VehicleTick)
	bd.userData = def->userData;
	Com_Memset( v, 0, sizeof( *v ) );
	v->body = Phys_BodyCreate( owner, world, &bd );
	if ( !v->body ) {
		return 0;
	}
	Com_Memset( &sd, 0, sizeof( sd ) );
	sd.type = PHYS_SHAPE_BOX;
	VectorCopy( def->halfExtents, sd.params );
	sd.density = 1000.0f;
	sd.friction = def->friction;
	sd.restitution = def->restitution;
	sd.categoryBits = def->categoryBits;
	sd.maskBits = def->maskBits;
	sd.flags = PHYS_SHAPE_HITEVENTS;
	sd.userData = def->userData;
	if ( !Phys_BodyAddShape( owner, v->body, &sd, NULL, 0, NULL, 0 ) ) {
		Phys_BodyDestroy( owner, v->body );
		return 0;
	}
	// the mass gamecode asked for, with the center of mass where it said
	b = Phys_Body( owner, v->body );
	md = b3Body_GetMassData( b->id );
	scale = def->mass / md.mass;
	md.mass = def->mass;
	md.center = Phys_Vec( def->centerOfMass );
	md.inertia.cx = b3MulSV( scale, md.inertia.cx );
	md.inertia.cy = b3MulSV( scale, md.inertia.cy );
	md.inertia.cz = b3MulSV( scale, md.inertia.cz );
	b3Body_SetMassData( b->id, md );

	v->inuse = qtrue;
	v->world = world;
	v->def = *def;
	for ( i = 0; i < def->numWheels; i++ ) {
		v->susp[i] = def->restLength;
	}
	VectorCopy( def->origin, v->lastPos );
	return (int)( v - phys_vehicles[owner] ) + 1;
}

static void Phys_VehicleDestroy( physOwner_t owner, int handle ) {
	physVehicle_t *v = Phys_Vehicle( owner, handle );

	if ( !v ) {
		return;
	}
	Phys_BodyDestroy( owner, v->body );
	Com_Memset( v, 0, sizeof( *v ) );
}

// a world is going: its vehicles go with it (phys_main.c)
void Phys_VehiclesWorldDestroyed( physOwner_t owner, int world ) {
	int i;

	for ( i = 0; i < PHYS_MAX_VEHICLES; i++ ) {
		if ( phys_vehicles[owner][i].inuse && phys_vehicles[owner][i].world == world ) {
			Com_Memset( &phys_vehicles[owner][i], 0, sizeof( phys_vehicles[owner][i] ) );
		}
	}
}

/*
==============================================================================
the per-tick model
==============================================================================
*/

typedef struct {
	physOwner_t	owner;
	b3BodyId	self;
	int			best;			// body handle of the hit, for the tie break
	int			hit;
	float		fraction;
	b3Vec3		point;
	b3Vec3		normal;
} physWheelCast_t;

static float Phys_WheelHit( b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t userMaterialId,
		int triangleIndex, int childIndex, void *context ) {
	physWheelCast_t *c = context;
	b3BodyId body = b3Shape_GetBody( shapeId );
	int handle;

	(void)userMaterialId; (void)triangleIndex; (void)childIndex;
	if ( B3_ID_EQUALS( body, c->self ) || b3Shape_IsSensor( shapeId ) ) {
		return -1.0f;
	}
	handle = (int)(intptr_t)b3Body_GetUserData( body );
	if ( c->hit && ( fraction > c->fraction || ( fraction == c->fraction && handle >= c->best ) ) ) {
		return c->fraction;
	}
	c->hit = 1;
	c->fraction = fraction;
	c->point = point;
	c->normal = normal;
	c->best = handle;
	return fraction;
}

static float Phys_Clampf( float x, float lo, float hi ) {
	return x < lo ? lo : x > hi ? hi : x;
}

static float Phys_Absf( float x ) {
	return x < 0.0f ? -x : x;
}

#define PHYS_TWO_PI 6.28318531f

static void Phys_VehicleTick( physOwner_t owner, physWorld_t *w, physVehicle_t *v, float dt ) {
	const oaxPhysVehicleDef_t *d = &v->def;
	physBody_t *b = Phys_Body( owner, v->body );
	b3BodyId id;
	b3WorldTransform xf;
	b3Vec3 up, fwd, left, lv, av, com;
	b3QueryFilter filter;
	float share, omega, k, c, fwdSpeed, steerTarget, steerStep, steerScale, rayLen, g;
	float throttle, brake, handbrake;
	int i, driven = 0, grounded = 0;

	if ( !b ) {
		return;
	}
	id = b->id;
	// A parked vehicle (no throttle, no steering) that Box3D put to sleep
	// stays asleep: its forces would only hold it where it is. Its state
	// then no longer depends on how long it was parked, and the first
	// throttle or steering wakes it.
	if ( !b3Body_IsAwake( id ) ) {
		if ( v->input.throttle == 0.0f && v->input.steer == 0.0f ) {
			return;
		}
		b3Body_SetAwake( id, true );
	}
	xf = b3Body_GetTransform( id );
	up = b3RotateVector( xf.q, b3Vec3_axisZ );
	fwd = b3RotateVector( xf.q, b3Vec3_axisX );
	left = b3RotateVector( xf.q, b3Vec3_axisY );
	lv = b3Body_GetLinearVelocity( id );
	av = b3Body_GetAngularVelocity( id );
	com = b3Body_GetWorldCenterOfMass( id );
	fwdSpeed = b3Dot( lv, fwd );
	g = -b3World_GetGravity( w->id ).z;

	throttle = Phys_Clampf( v->input.throttle, -1.0f, 1.0f );
	brake = Phys_Clampf( v->input.brake, 0.0f, 1.0f );
	handbrake = Phys_Clampf( v->input.handbrake, 0.0f, 1.0f );

	// steering: less lock at speed, turned at a limited rate
	steerScale = 1.0f - d->steerSpeedFactor * Phys_Clampf( Phys_Absf( fwdSpeed ) / ( d->maxSpeed > 0.0f ? d->maxSpeed : 1.0f ), 0.0f, 1.0f );
	steerTarget = Phys_Clampf( v->input.steer, -1.0f, 1.0f ) * d->maxSteer * steerScale;
	steerStep = d->steerRate * dt;
	if ( steerStep <= 0.0f ) {
		v->steer = steerTarget;
	} else if ( steerTarget > v->steer + steerStep ) {
		v->steer += steerStep;
	} else if ( steerTarget < v->steer - steerStep ) {
		v->steer -= steerStep;
	} else {
		v->steer = steerTarget;
	}

	// the suspension (or hover) spring of one wheel's share of the mass
	share = d->numWheels > 0 ? d->mass / d->numWheels : d->mass;
	omega = PHYS_TWO_PI * d->springHertz;
	k = share * omega * omega;
	c = 2.0f * share * d->dampingRatio * omega;
	for ( i = 0; i < d->numWheels; i++ ) {
		if ( d->driveMask & ( 1 << i ) ) {
			driven++;
		}
	}

	filter = b3DefaultQueryFilter();
	filter.categoryBits = B3_DEFAULT_CATEGORY_BITS;
	filter.maskBits = d->rayMask ? d->rayMask : B3_DEFAULT_MASK_BITS;
	rayLen = d->type == PHYS_VEHICLE_HOVER ? d->restLength * 2.0f : d->restLength + d->wheelRadius;

	v->contacts = 0;
	for ( i = 0; i < d->numWheels; i++ ) {
		physWheelCast_t cast;
		b3Vec3 top, pv, heading, side, force, at;
		float dist, compression, vUp, load, vLong, vLat, fLong, fLat, maxF, total, ang;
		b3CosSin cs;

		top = b3Add( xf.p, b3RotateVector( xf.q, Phys_Vec( d->wheels[i] ) ) );
		Com_Memset( &cast, 0, sizeof( cast ) );
		cast.owner = owner;
		cast.self = id;
		b3World_CastRay( w->id, top, b3MulSV( -rayLen, up ), filter, Phys_WheelHit, &cast );
		if ( !cast.hit ) {
			v->susp[i] = d->type == PHYS_VEHICLE_HOVER ? rayLen : d->restLength;
			if ( d->type != PHYS_VEHICLE_HOVER && ( d->driveMask & ( 1 << i ) ) ) {
				// a free wheel spins up with the throttle
				v->spin[i] += throttle * 20.0f * dt;
			}
			continue;
		}
		v->contacts |= 1 << i;
		grounded++;
		dist = cast.fraction * rayLen;
		if ( d->type == PHYS_VEHICLE_HOVER ) {
			v->susp[i] = dist;
			compression = d->restLength - dist;
		} else {
			v->susp[i] = dist - d->wheelRadius;
			if ( v->susp[i] < 0.0f ) {
				v->susp[i] = 0.0f;
			}
			compression = d->restLength - v->susp[i];
		}
		pv = b3Body_GetWorldPointVelocity( id, cast.point );
		vUp = b3Dot( pv, up );
		// above the hover height a thruster only cushions a fall
		load = ( compression > 0.0f ? k * compression : 0.0f ) - c * vUp;
		if ( load < 0.0f ) {
			load = 0.0f;
		}
		// a hard landing never pushes back harder than eight weights
		if ( load > 8.0f * share * g ) {
			load = 8.0f * share * g;
		}
		b3Body_ApplyForce( id, b3MulSV( load, up ), top, false );

		if ( d->type == PHYS_VEHICLE_HOVER ) {
			continue;
		}

		// the tire: heading turned by the steering, flattened onto the ground
		ang = 0.0f;
		if ( d->steerMask & ( 1 << i ) ) {
			ang = d->wheels[i][0] >= 0.0f ? v->steer : -v->steer;
		}
		cs = b3ComputeCosSin( ang );
		heading = b3Add( b3MulSV( cs.cosine, fwd ), b3MulSV( cs.sine, left ) );
		heading = b3Sub( heading, b3MulSV( b3Dot( heading, cast.normal ), cast.normal ) );
		heading = b3Normalize( heading );
		side = b3Cross( cast.normal, heading );
		vLong = b3Dot( pv, heading );
		vLat = b3Dot( pv, side );

		fLong = 0.0f;
		if ( d->driveMask & ( 1 << i ) ) {
			float drive = throttle * d->engineForce / ( driven ? driven : 1 );
			if ( ( throttle > 0.0f && vLong >= d->maxSpeed ) || ( throttle < 0.0f && vLong <= -d->maxReverse ) ) {
				drive = 0.0f;
			}
			// pushing against the motion brakes instead
			if ( ( throttle > 0.0f && vLong < -32.0f ) || ( throttle < 0.0f && vLong > 32.0f ) ) {
				drive = 0.0f;
				brake = brake > Phys_Absf( throttle ) ? brake : Phys_Absf( throttle );
			}
			fLong += drive;
		}
		{
			float stop = share * Phys_Absf( vLong ) / dt;	// the force that stops this wheel's share in one tick
			float brk = brake * d->brakeForce / ( d->numWheels ? d->numWheels : 1 );
			if ( d->wheels[i][0] < 0.0f && handbrake > 0.0f ) {
				brk += handbrake * d->brakeForce / ( d->numWheels ? d->numWheels : 1 );
			}
			// rolling resistance: a coasting car slows down
			brk += share * g * 0.02f;
			if ( brk > stop ) {
				brk = stop;
			}
			fLong += vLong > 0.0f ? -brk : brk;
		}
		fLat = -vLat * share * d->lateralStiffness / dt;
		maxF = d->grip * load;
		if ( d->wheels[i][0] < 0.0f && handbrake > 0.0f ) {
			maxF *= 1.0f - handbrake * ( 1.0f - d->handbrakeGrip );
		}
		total = sqrtf( fLong * fLong + fLat * fLat );
		if ( total > maxF && total > 0.0f ) {
			fLong *= maxF / total;
			fLat *= maxF / total;
		}
		force = b3Add( b3MulSV( fLong, heading ), b3MulSV( fLat, side ) );
		// tire forces act partway up toward the center of mass: less body roll
		at = b3MulAdd( cast.point, d->rollInfluence * b3Dot( b3Sub( com, cast.point ), up ), up );
		b3Body_ApplyForce( id, force, at, false );
		v->spin[i] += vLong / ( d->wheelRadius > 0.0f ? d->wheelRadius : 1.0f ) * dt;
	}

	if ( d->type == PHYS_VEHICLE_HOVER ) {
		b3Vec3 flatFwd = { fwd.x, fwd.y, 0.0f }, flatLeft;
		float yawRate, ixx, roll, rollRate, targetRoll, alpha, vLat;
		flatFwd = b3Normalize( flatFwd );
		flatLeft.x = -flatFwd.y;
		flatLeft.y = flatFwd.x;
		flatLeft.z = 0.0f;
		if ( grounded ) {
			float drive = throttle * d->engineForce;
			float speedHere = b3Dot( lv, flatFwd );
			if ( ( throttle > 0.0f && speedHere >= d->maxSpeed ) || ( throttle < 0.0f && speedHere <= -d->maxReverse ) ) {
				drive = 0.0f;
			}
			b3Body_ApplyForceToCenter( id, b3MulSV( drive, flatFwd ), false );
			// slide: only a little of the sideways speed is taken each tick
			vLat = b3Dot( lv, flatLeft );
			b3Body_ApplyForceToCenter( id, b3MulSV( -vLat * d->mass * d->lateralStiffness / dt, flatLeft ), false );
			if ( brake > 0.0f ) {
				b3Vec3 flatV = { lv.x, lv.y, 0.0f };
				b3Body_ApplyForceToCenter( id, b3MulSV( -brake * d->brakeForce / ( d->maxSpeed > 0.0f ? d->maxSpeed : 1.0f ), flatV ), false );
			}
		}
		// yaw toward the steering's turn rate, with the inertia about up
		ixx = d->mass * ( d->halfExtents[0] * d->halfExtents[0] + d->halfExtents[1] * d->halfExtents[1] ) / 3.0f;
		yawRate = b3Dot( av, b3Vec3_axisZ );
		alpha = ( Phys_Clampf( v->input.steer, -1.0f, 1.0f ) * d->maxSteer - yawRate ) * d->turnTorque;
		b3Body_ApplyTorque( id, b3MulSV( ixx * alpha, b3Vec3_axisZ ), false );
		// bank into the turn
		ixx = d->mass * ( d->halfExtents[1] * d->halfExtents[1] + d->halfExtents[2] * d->halfExtents[2] ) / 3.0f;
		roll = -left.z;
		rollRate = b3Dot( av, fwd );
		targetRoll = Phys_Clampf( v->input.steer, -1.0f, 1.0f ) * d->bankAngle *
			Phys_Clampf( Phys_Absf( fwdSpeed ) / ( d->maxSpeed > 0.0f ? d->maxSpeed : 1.0f ), 0.0f, 1.0f );
		alpha = ( targetRoll - roll ) * 40.0f - rollRate * 8.0f;
		b3Body_ApplyTorque( id, b3MulSV( -ixx * alpha, fwd ), false );
	}

	// on its side or roof and nearly still: roll back onto the wheels
	if ( ( d->flags & PHYS_VF_AUTOFLIP ) && up.z < 0.4f && b3Length( lv ) < 240.0f && grounded < 2 ) {
		v->flipTime += dt;
		if ( v->flipTime > 1.0f ) {
			b3Vec3 axis = b3Cross( up, b3Vec3_axisZ );
			float ixx = d->mass * ( d->halfExtents[1] * d->halfExtents[1] + d->halfExtents[2] * d->halfExtents[2] ) / 3.0f;
			float len = b3Length( axis );
			if ( len < 0.01f ) {
				axis = fwd;		// upside down: either way round
			} else {
				axis = b3MulSV( 1.0f / len, axis );
			}
			b3Body_ApplyTorque( id, b3MulSV( ixx * 30.0f, b3Sub( axis, b3MulSV( 0.2f, av ) ) ), false );
			if ( up.z < -0.2f || b3Length( lv ) < 64.0f ) {
				b3Body_ApplyForceToCenter( id, b3MulSV( d->mass * g * 0.6f, b3Vec3_axisZ ), false );
			}
		}
	} else {
		v->flipTime = 0.0f;
	}

	for ( i = 0; i < d->numWheels; i++ ) {
		// keep spins small so float precision lasts
		while ( v->spin[i] > PHYS_TWO_PI ) {
			v->spin[i] -= PHYS_TWO_PI;
		}
		while ( v->spin[i] < -PHYS_TWO_PI ) {
			v->spin[i] += PHYS_TWO_PI;
		}
	}
	v->ticks++;
}

// path length, measured after the step
static void Phys_VehicleMeasure( physOwner_t owner, physVehicle_t *v ) {
	physBody_t *b = Phys_Body( owner, v->body );
	b3WorldTransform xf;
	float dx, dy, dz;

	if ( !b ) {
		return;
	}
	xf = b3Body_GetTransform( b->id );
	dx = xf.p.x - v->lastPos[0];
	dy = xf.p.y - v->lastPos[1];
	dz = xf.p.z - v->lastPos[2];
	v->distance += sqrtf( dx * dx + dy * dy + dz * dz );
	v->lastPos[0] = xf.p.x;
	v->lastPos[1] = xf.p.y;
	v->lastPos[2] = xf.p.z;
}

// before every tick of a world (phys_main.c Phys_RunTicks)
void Phys_VehiclesBeforeTick( physOwner_t owner, int world, physWorld_t *w, float dt ) {
	int i;

	for ( i = 0; i < PHYS_MAX_VEHICLES; i++ ) {
		physVehicle_t *v = &phys_vehicles[owner][i];
		if ( v->inuse && v->world == world ) {
			Phys_VehicleTick( owner, w, v, dt );
		}
	}
}

void Phys_VehiclesAfterTick( physOwner_t owner, int world ) {
	int i;

	for ( i = 0; i < PHYS_MAX_VEHICLES; i++ ) {
		physVehicle_t *v = &phys_vehicles[owner][i];
		if ( v->inuse && v->world == world ) {
			Phys_VehicleMeasure( owner, v );
		}
	}
}

static void Phys_VehicleGetState( physOwner_t owner, physVehicle_t *v, oaxPhysVehicleState_t *out ) {
	oaxPhysBodyState_t bs;
	int i;

	Com_Memset( out, 0, sizeof( *out ) );
	if ( !Phys_BodyState( owner, v->body, &bs ) ) {
		return;
	}
	out->body = v->body;
	VectorCopy( bs.origin, out->origin );
	Com_Memcpy( out->quat, bs.quat, sizeof( out->quat ) );
	VectorCopy( bs.velocity, out->velocity );
	VectorCopy( bs.angularVelocity, out->angularVelocity );
	{
		b3Vec3 fwd = b3RotateVector( Phys_Quat( bs.quat ), b3Vec3_axisX );
		out->speed = bs.velocity[0] * fwd.x + bs.velocity[1] * fwd.y + bs.velocity[2] * fwd.z;
	}
	out->steer = v->steer;
	out->numWheels = v->def.numWheels;
	out->contacts = v->contacts;
	for ( i = 0; i < v->def.numWheels; i++ ) {
		out->suspension[i] = v->susp[i];
		out->spin[i] = v->spin[i];
	}
	out->distance = v->distance;
	out->ticks = v->ticks;
}

/*
==============================================================================
terrain: the collision model's heightmaps as height fields, its trunks as
boxes (cm_terrain.c: the exact heights and boxes players collide with)
==============================================================================
*/

static int Phys_AddTerrain( physOwner_t owner, int world, const oaxPhysShapeDef_t *material, int *outBodies, int maxBodies ) {
	physWorld_t *w = Phys_World( owner, world );
	int n, k, i, made = 0, trunkBody = 0;

	if ( !w ) {
		return 0;
	}
	n = CM_OAXNumTerrains();
	for ( k = 0; k < n; k++ ) {
		const oaxTerrainInfo_t *t = CM_OAXTerrainInfo( k );
		const float *z = CM_OAXTerrainHeights( k );
		oaxPhysHeightField_t hf;
		vec3_t mins, maxs;
		int body;

		if ( !t || !z ) {
			continue;
		}
		CM_OAXTerrainBounds( k, mins, maxs );
		Com_Memset( &hf, 0, sizeof( hf ) );
		hf.origin[0] = t->origin[0];
		hf.origin[1] = t->origin[1];
		hf.origin[2] = 0.0f;
		hf.cellSize[0] = t->cellSize;
		hf.cellSize[1] = t->cellSize;
		hf.countX = t->samplesX;
		hf.countY = t->samplesY;
		hf.minHeight = mins[2] - 1.0f;
		hf.maxHeight = maxs[2] + 1.0f;
		body = Phys_AddHeightField( owner, world, &hf, z, material );
		if ( !body ) {
			Com_Printf( S_COLOR_YELLOW "physics: terrain %d: no height field (%dx%d, %g..%g)\n", k, hf.countX, hf.countY, hf.minHeight, hf.maxHeight );
			continue;
		}
		if ( outBodies && made < maxBodies ) {
			outBodies[made] = body;
		}
		made++;
	}
	for ( i = 0; i < CM_OAXNumTrunks(); i++ ) {
		oaxPhysShapeDef_t sd;
		vec3_t mins, maxs;

		if ( !trunkBody ) {
			oaxPhysBodyDef_t bd;
			Com_Memset( &bd, 0, sizeof( bd ) );
			bd.type = PHYS_BODY_STATIC;
			bd.userData = material ? material->userData : 0;
			if ( !( trunkBody = Phys_BodyCreate( owner, world, &bd ) ) ) {
				break;
			}
			if ( outBodies && made < maxBodies ) {
				outBodies[made] = trunkBody;
			}
			made++;
		}
		CM_OAXTrunkBounds( i, mins, maxs );
		if ( material ) {
			sd = *material;
		} else {
			Com_Memset( &sd, 0, sizeof( sd ) );
			sd.friction = 0.6f;
		}
		sd.type = PHYS_SHAPE_BOX;
		sd.params[0] = ( maxs[0] - mins[0] ) * 0.5f;
		sd.params[1] = ( maxs[1] - mins[1] ) * 0.5f;
		sd.params[2] = ( maxs[2] - mins[2] ) * 0.5f;
		sd.offset[0] = ( maxs[0] + mins[0] ) * 0.5f;
		sd.offset[1] = ( maxs[1] + mins[1] ) * 0.5f;
		sd.offset[2] = ( maxs[2] + mins[2] ) * 0.5f;
		sd.quat[0] = sd.quat[1] = sd.quat[2] = sd.quat[3] = 0.0f;
		Phys_BodyAddShape( owner, trunkBody, &sd, NULL, 0, NULL, 0 );
	}
	Com_DPrintf( "physics: %d terrain(s), %d trunks, %d static bodies for world %d\n", n, CM_OAXNumTrunks(), made, world );
	return made;
}

/*
==============================================================================
syscalls (1260-1269, token "physics_vehicle")
==============================================================================
*/

#define CHECK( n, size, what ) VM_CheckBlock( args[n], (int)( size ), what )

qboolean Phys_VehicleSyscall( physOwner_t owner, intptr_t *args, intptr_t *ret ) {
	physVehicle_t *v;

	switch ( args[0] ) {
	case PHYS_VEHICLE_CREATE:
		CHECK( 2, sizeof( oaxPhysVehicleDef_t ), "PHYSVEHDEF" );
		*ret = Phys_VehicleCreate( owner, args[1], VMA( 2 ) );
		return qtrue;
	case PHYS_VEHICLE_DESTROY:
		Phys_VehicleDestroy( owner, args[1] );
		return qtrue;
	case PHYS_VEHICLE_SET_INPUT:
		CHECK( 2, sizeof( oaxPhysVehicleInput_t ), "PHYSVEHIN" );
		if ( ( v = Phys_Vehicle( owner, args[1] ) ) ) {
			v->input = *(const oaxPhysVehicleInput_t *)VMA( 2 );
		}
		return qtrue;
	case PHYS_VEHICLE_GET_STATE:
		CHECK( 2, sizeof( oaxPhysVehicleState_t ), "PHYSVEHSTATE" );
		if ( ( v = Phys_Vehicle( owner, args[1] ) ) ) {
			Phys_VehicleGetState( owner, v, VMA( 2 ) );
			*ret = 1;
		} else {
			Com_Memset( VMA( 2 ), 0, sizeof( oaxPhysVehicleState_t ) );
		}
		return qtrue;
	case PHYS_WORLD_ADD_TERRAIN:
		if ( args[2] ) {
			CHECK( 2, sizeof( oaxPhysShapeDef_t ), "PHYSTERRMAT" );
		}
		if ( args[4] < 0 || args[4] > 64 ) {
			return qtrue;
		}
		if ( args[4] ) {
			CHECK( 3, sizeof( int ) * args[4], "PHYSTERRBODIES" );
		}
		*ret = Phys_AddTerrain( owner, args[1], args[2] ? VMA( 2 ) : NULL, args[4] ? VMA( 3 ) : NULL, args[4] );
		return qtrue;
	}
	return qfalse;
}
