/*
===========================================================================
phys_main.c: Box3D worlds, bodies, shapes, joints and their bookkeeping.

The physics module owns every Box3D world the engine runs. Gamecode
reaches it through the syscall block in phys_syscalls.c (ABI in
oax_phys.h); handles are per owner (game, cgame, engine), so a VM only
ever sees its own objects, and Phys_FreeOwner drops a VM's worlds when it
restarts.

Determinism: Box3D steps give the same bits for any worker count and on
every build as long as the calls are the same. This file adds nothing that
could break that: no wall clock feeds the simulation (the step time is
measured for the debug values only), tables are walked in handle order,
and every conversion is plain float math compiled with -ffp-contract=off.
===========================================================================
*/

#include "phys_local.h"

physWorld_t		phys_worlds[OAX_PHYS_MAX_WORLDS];
cvar_t			*phys_workers;

static physBody_t	phys_bodies[PHYS_NUM_OWNERS][PHYS_MAX_BODIES];
static physJoint_t	phys_joints[PHYS_NUM_OWNERS][PHYS_MAX_JOINTS];
static qboolean		phys_initialized;

static const char *phys_ownerNames[PHYS_NUM_OWNERS] = { "game", "cgame", "engine" };

/*
==============================================================================
math helpers
==============================================================================
*/

b3Vec3 Phys_Vec( const float *v ) {
	b3Vec3 r;
	r.x = v[0];
	r.y = v[1];
	r.z = v[2];
	return r;
}

// a quaternion from gamecode: all zero means identity; anything else is
// normalized (gamecode builds them with interpreter float math)
b3Quat Phys_Quat( const float *q ) {
	b3Quat r;
	float len;

	if ( !q || ( q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f && q[3] == 0.0f ) ) {
		return b3Quat_identity;
	}
	len = sqrtf( q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] );
	if ( len < 1e-6f ) {
		return b3Quat_identity;
	}
	r.v.x = q[0] / len;
	r.v.y = q[1] / len;
	r.v.z = q[2] / len;
	r.s = q[3] / len;
	return r;
}

b3Transform Phys_Frame( const float *f7 ) {
	b3Transform t;
	t.p = Phys_Vec( f7 );
	t.q = Phys_Quat( f7 + 3 );
	return t;
}

/*
==============================================================================
allocation: Box3D allocates from its worker threads too (arena overflow), so
on the cart, where the workers are our own threads, every Box3D allocation
goes through one spinlock. Native builds keep Box3D's own allocator.
==============================================================================
*/

#ifdef WASMCART_THREADS
static int phys_allocLock;

static void Phys_Lock( void ) {
	while ( __atomic_exchange_n( &phys_allocLock, 1, __ATOMIC_ACQUIRE ) ) {
		while ( __atomic_load_n( &phys_allocLock, __ATOMIC_RELAXED ) ) {
		}
	}
}

static void Phys_Unlock( void ) {
	__atomic_store_n( &phys_allocLock, 0, __ATOMIC_RELEASE );
}

static void *Phys_Alloc( int32_t size, int32_t alignment ) {
	void *p;
	Phys_Lock();
	p = aligned_alloc( alignment, ( (size_t)size + alignment - 1 ) & ~( (size_t)alignment - 1 ) );
	Phys_Unlock();
	return p;
}

static void Phys_Free( void *mem ) {
	Phys_Lock();
	free( mem );
	Phys_Unlock();
}
#endif

static void Phys_Log( const char *message ) {
	Com_Printf( "box3d: %s\n", message );
}

/*
==================
Phys_Init

Once per process, before any world: the length scale is global in Box3D
and every default (slop, margins, thresholds) is computed from it.
==================
*/
void Phys_Init( void ) {
	if ( phys_initialized ) {
		return;
	}
	phys_initialized = qtrue;
	b3SetLengthUnitsPerMeter( PHYS_UNITS_PER_METER );
	b3SetLogFcn( Phys_Log );
#ifdef WASMCART_THREADS
	b3SetAllocator( Phys_Alloc, Phys_Free );
#endif
	// override for every world's workerCount (0 = what gamecode asked for);
	// results never depend on it, only the time a step takes
	phys_workers = Cvar_Get( "phys_workers", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( phys_workers, "Box3D worker threads for every physics world, 0 = as gamecode asks (1-8)." );
}

/*
==============================================================================
worlds
==============================================================================
*/

physWorld_t *Phys_World( physOwner_t owner, int world ) {
	physWorld_t *w;

	if ( world < 1 || world > OAX_PHYS_MAX_WORLDS ) {
		return NULL;
	}
	w = &phys_worlds[world - 1];
	if ( !w->inuse || w->owner != owner ) {
		return NULL;
	}
	return w;
}

void Phys_WorldOwn( physWorld_t *w, void *blob, int kind ) {
	if ( w->numOwned == w->maxOwned ) {
		int n = w->maxOwned ? w->maxOwned * 2 : 64;
		void **o = Z_Malloc( n * sizeof( *o ) );
		byte *k = Z_Malloc( n );
		if ( w->owned ) {
			Com_Memcpy( o, w->owned, w->numOwned * sizeof( *o ) );
			Com_Memcpy( k, w->ownedKind, w->numOwned );
			Z_Free( w->owned );
			Z_Free( w->ownedKind );
		}
		w->owned = o;
		w->ownedKind = k;
		w->maxOwned = n;
	}
	w->ownedKind[w->numOwned] = (byte)kind;
	w->owned[w->numOwned++] = blob;
}

static int Phys_ClampWorkers( int n ) {
	if ( phys_workers && phys_workers->integer > 0 ) {
		n = phys_workers->integer;
	}
	if ( n < 1 ) {
		n = 1;
	}
	if ( n > OAX_PHYS_MAX_WORKERS ) {
		n = OAX_PHYS_MAX_WORKERS;
	}
#ifdef WASMCART
	if ( !Phys_TasksAvailable() ) {
		n = 1;	// a cart built without threads runs one worker
	}
#endif
	return n;
}

int Phys_WorldCreate( physOwner_t owner, const oaxPhysWorldDef_t *def ) {
	b3WorldDef wd;
	b3BodyDef bd;
	physWorld_t *w;
	int i;

	Phys_Init();
	for ( i = 0; i < OAX_PHYS_MAX_WORLDS; i++ ) {
		if ( !phys_worlds[i].inuse ) {
			break;
		}
	}
	if ( i == OAX_PHYS_MAX_WORLDS ) {
		Com_Printf( S_COLOR_YELLOW "physics: no free world (max %d)\n", OAX_PHYS_MAX_WORLDS );
		return 0;
	}
	w = &phys_worlds[i];
	Com_Memset( w, 0, sizeof( *w ) );

	wd = b3DefaultWorldDef();
	wd.gravity = Phys_Vec( def->gravity );
	if ( def->contactHertz > 0.0f ) wd.contactHertz = def->contactHertz;
	if ( def->contactDampingRatio > 0.0f ) wd.contactDampingRatio = def->contactDampingRatio;
	if ( def->contactSpeed > 0.0f ) wd.contactSpeed = def->contactSpeed;
	if ( def->maxLinearSpeed > 0.0f ) wd.maximumLinearSpeed = def->maxLinearSpeed;
	if ( def->restitutionThreshold > 0.0f ) wd.restitutionThreshold = def->restitutionThreshold;
	if ( def->hitEventThreshold > 0.0f ) wd.hitEventThreshold = def->hitEventThreshold;
	wd.enableSleep = ( def->flags & PHYS_WORLD_NOSLEEP ) ? false : true;
	wd.enableContinuous = ( def->flags & PHYS_WORLD_NOCONTINUOUS ) ? false : true;
	w->workerCount = Phys_ClampWorkers( def->workerCount );
	wd.workerCount = w->workerCount;
	if ( w->workerCount > 1 ) {
		Phys_TasksSetup( &wd, w->workerCount );
	}

	w->id = b3CreateWorld( &wd );
	if ( B3_IS_NULL( w->id ) ) {
		return 0;
	}
	w->inuse = qtrue;
	w->owner = owner;
	w->tickMsec = def->tickMsec > 0 ? def->tickMsec : 16;
	w->substeps = def->substeps > 0 ? def->substeps : 4;
	w->events = Z_Malloc( PHYS_MAX_EVENTS * sizeof( oaxPhysContact_t ) );

	bd = b3DefaultBodyDef();
	bd.type = b3_staticBody;
	w->ground = b3CreateBody( w->id, &bd );
	return i + 1;
}

void Phys_WorldDestroy( physOwner_t owner, int world ) {
	physWorld_t *w = Phys_World( owner, world );
	int i;

	if ( !w ) {
		return;
	}
	Phys_VehiclesWorldDestroyed( owner, world );
	for ( i = 0; i < PHYS_MAX_BODIES; i++ ) {
		if ( phys_bodies[owner][i].world == world ) {
			Com_Memset( &phys_bodies[owner][i], 0, sizeof( physBody_t ) );
		}
	}
	for ( i = 0; i < PHYS_MAX_JOINTS; i++ ) {
		if ( phys_joints[owner][i].world == world ) {
			Com_Memset( &phys_joints[owner][i], 0, sizeof( physJoint_t ) );
		}
	}
	b3DestroyWorld( w->id );
	// meshes and height fields a shape referenced outlive the world's shapes
	for ( i = 0; i < w->numOwned; i++ ) {
		if ( w->ownedKind[i] == 1 ) {
			b3DestroyHeightField( w->owned[i] );
		} else {
			b3DestroyMesh( w->owned[i] );
		}
	}
	if ( w->owned ) {
		Z_Free( w->owned );
		Z_Free( w->ownedKind );
	}
	if ( w->events ) {
		Z_Free( w->events );
	}
	Com_Memset( w, 0, sizeof( *w ) );
}

void Phys_FreeOwner( physOwner_t owner ) {
	int i;

	for ( i = 0; i < OAX_PHYS_MAX_WORLDS; i++ ) {
		if ( phys_worlds[i].inuse && phys_worlds[i].owner == owner ) {
			Phys_WorldDestroy( owner, i + 1 );
		}
	}
	Com_Memset( phys_bodies[owner], 0, sizeof( phys_bodies[owner] ) );
	Com_Memset( phys_joints[owner], 0, sizeof( phys_joints[owner] ) );
}

static int Phys_BodyHandleOf( b3BodyId id ) {
	return (int)(intptr_t)b3Body_GetUserData( id );
}

static int Phys_ShapeBody( b3ShapeId shape, int *user, int *shapeUser ) {
	b3BodyId body;
	int handle;

	*user = 0;
	*shapeUser = 0;
	if ( !b3Shape_IsValid( shape ) ) {
		return 0;
	}
	*shapeUser = (int)(intptr_t)b3Shape_GetUserData( shape );
	body = b3Shape_GetBody( shape );
	handle = Phys_BodyHandleOf( body );
	return handle;
}

// gather this tick's contact events into the world's queue
static void Phys_GatherEvents( physOwner_t owner, physWorld_t *w ) {
	b3ContactEvents ev = b3World_GetContactEvents( w->id );
	int i;

#define PHYS_PUSH_EVENT( TYPE, SA, SB ) \
	if ( w->numEvents >= PHYS_MAX_EVENTS ) { w->droppedEvents++; } else { \
		oaxPhysContact_t *c = &w->events[w->numEvents++]; \
		physBody_t *ba, *bb; \
		Com_Memset( c, 0, sizeof( *c ) ); \
		c->type = TYPE; \
		c->bodyA = Phys_ShapeBody( SA, &c->userA, &c->shapeUserA ); \
		c->bodyB = Phys_ShapeBody( SB, &c->userB, &c->shapeUserB ); \
		ba = Phys_Body( owner, c->bodyA ); bb = Phys_Body( owner, c->bodyB ); \
		c->userA = ba ? ba->userData : 0; c->userB = bb ? bb->userData : 0;

	for ( i = 0; i < ev.beginCount; i++ ) {
		PHYS_PUSH_EVENT( PHYS_CONTACT_BEGIN, ev.beginEvents[i].shapeIdA, ev.beginEvents[i].shapeIdB )
		}
	}
	for ( i = 0; i < ev.endCount; i++ ) {
		PHYS_PUSH_EVENT( PHYS_CONTACT_END, ev.endEvents[i].shapeIdA, ev.endEvents[i].shapeIdB )
		}
	}
	for ( i = 0; i < ev.hitCount; i++ ) {
		const b3ContactHitEvent *h = &ev.hitEvents[i];
		PHYS_PUSH_EVENT( PHYS_CONTACT_HIT, h->shapeIdA, h->shapeIdB )
			c->point[0] = h->point.x;
			c->point[1] = h->point.y;
			c->point[2] = h->point.z;
			c->normal[0] = h->normal.x;
			c->normal[1] = h->normal.y;
			c->normal[2] = h->normal.z;
			c->speed = h->approachSpeed;
		}
	}
#undef PHYS_PUSH_EVENT
}

static void Phys_PublishOwner( physOwner_t owner ) {
	int i, worlds = 0, bodies = 0, awake = 0, contacts = 0, joints = 0, ticks = 0;
	float stepMs = 0.0f;
	char name[64];

	for ( i = 0; i < OAX_PHYS_MAX_WORLDS; i++ ) {
		physWorld_t *w = &phys_worlds[i];
		b3Counters c;
		if ( !w->inuse || w->owner != owner ) {
			continue;
		}
		c = b3World_GetCounters( w->id );
		worlds++;
		bodies += c.bodyCount;
		joints += c.jointCount;
		contacts += c.contactCount;
		awake += b3World_GetAwakeBodyCount( w->id );
		ticks += w->ticks;
		stepMs += w->lastStepMs;
	}
#define PUB( FIELD, V ) Com_sprintf( name, sizeof( name ), "phys_%s_%s", phys_ownerNames[owner], FIELD ); Com_DebugSetInt( name, V );
	PUB( "worlds", worlds );
	PUB( "bodies", bodies );
	PUB( "awake", awake );
	PUB( "contacts", contacts );
	PUB( "joints", joints );
	PUB( "ticks", ticks );
#undef PUB
	Com_sprintf( name, sizeof( name ), "phys_%s_step_ms", phys_ownerNames[owner] );
	Com_DebugSetFloat( name, stepMs );
	Phys_TasksPublish();
}

// runs `count` fixed ticks
static void Phys_RunTicks( physOwner_t owner, physWorld_t *w, int count ) {
	float dt = w->tickMsec * 0.001f;
	int i;

	for ( i = 0; i < count; i++ ) {
		uint64_t t0 = b3GetTicks();
		if ( w->workerCount > 1 ) {
			Phys_TasksBeginStep();
		}
		Phys_VehiclesBeforeTick( owner, (int)( w - phys_worlds ) + 1, w, dt );
		b3World_Step( w->id, dt, w->substeps );
		Phys_VehiclesAfterTick( owner, (int)( w - phys_worlds ) + 1 );
		w->lastStepMs = b3GetMilliseconds( t0 );
		w->ticks++;
		Phys_GatherEvents( owner, w );
	}
}

int Phys_WorldStep( physOwner_t owner, int world, int msec ) {
	physWorld_t *w = Phys_World( owner, world );
	int ticks;

	if ( !w ) {
		return 0;
	}
	if ( msec < 0 ) {
		ticks = -msec;
		if ( ticks > 1000 ) {
			ticks = 1000;
		}
	} else {
		// a hitch never asks for more than a quarter second of catch-up
		w->accumMsec += msec;
		ticks = w->accumMsec / w->tickMsec;
		w->accumMsec -= ticks * w->tickMsec;
		if ( ticks > 250 / w->tickMsec + 1 ) {
			ticks = 250 / w->tickMsec + 1;
		}
	}
	Phys_RunTicks( owner, w, ticks );
	Phys_PublishOwner( owner );
	return ticks;
}

void Phys_WorldStats( physOwner_t owner, int world, oaxPhysStats_t *out ) {
	physWorld_t *w = Phys_World( owner, world );
	b3Counters c;

	Com_Memset( out, 0, sizeof( *out ) );
	if ( !w ) {
		return;
	}
	c = b3World_GetCounters( w->id );
	out->bodies = c.bodyCount;
	out->shapes = c.shapeCount;
	out->joints = c.jointCount;
	out->contacts = c.contactCount;
	out->awakeBodies = b3World_GetAwakeBodyCount( w->id );
	out->ticks = w->ticks;
	out->lastStepMs = w->lastStepMs;
	out->accumMs = (float)w->accumMsec;
	out->hash = Phys_WorldHash( owner, world );
	out->workerCount = w->workerCount;
}

// FNV-1a over every live body of the world in handle order: position,
// rotation, linear and angular velocity, awake flag
unsigned Phys_WorldHash( physOwner_t owner, int world ) {
	unsigned h = 2166136261u;
	int i, j;

	if ( !Phys_World( owner, world ) ) {
		return 0;
	}
	for ( i = 0; i < PHYS_MAX_BODIES; i++ ) {
		physBody_t *b = &phys_bodies[owner][i];
		float v[13];
		const byte *p;
		b3WorldTransform xf;
		b3Vec3 lv, av;

		if ( b->world != world || B3_IS_NULL( b->id ) || !b3Body_IsValid( b->id ) ) {
			continue;
		}
		xf = b3Body_GetTransform( b->id );
		lv = b3Body_GetLinearVelocity( b->id );
		av = b3Body_GetAngularVelocity( b->id );
		v[0] = xf.p.x; v[1] = xf.p.y; v[2] = xf.p.z;
		v[3] = xf.q.v.x; v[4] = xf.q.v.y; v[5] = xf.q.v.z; v[6] = xf.q.s;
		v[7] = lv.x; v[8] = lv.y; v[9] = lv.z;
		v[10] = av.x; v[11] = av.y; v[12] = av.z;
		p = (const byte *)v;
		for ( j = 0; j < (int)sizeof( v ); j++ ) {
			h = ( h ^ p[j] ) * 16777619u;
		}
		h = ( h ^ ( b3Body_IsAwake( b->id ) ? 1u : 0u ) ) * 16777619u;
	}
	return h;
}

int Phys_WorldContactEvents( physOwner_t owner, int world, oaxPhysContact_t *out, int max ) {
	physWorld_t *w = Phys_World( owner, world );
	int n;

	if ( !w ) {
		return 0;
	}
	n = w->numEvents < max ? w->numEvents : max;
	if ( n > 0 ) {
		Com_Memcpy( out, w->events, n * sizeof( oaxPhysContact_t ) );
	}
	w->numEvents = 0;
	w->droppedEvents = 0;
	return n;
}

/*
==============================================================================
bodies
==============================================================================
*/

physBody_t *Phys_Body( physOwner_t owner, int body ) {
	physBody_t *b;

	if ( body < 1 || body > PHYS_MAX_BODIES ) {
		return NULL;
	}
	b = &phys_bodies[owner][body - 1];
	if ( B3_IS_NULL( b->id ) || !b3Body_IsValid( b->id ) ) {
		return NULL;
	}
	return b;
}

int Phys_BodyRegister( physOwner_t owner, int world, b3BodyId id, int userData ) {
	int i;

	// lowest free slot: handles are reused in a fixed order, so the same
	// calls give the same handles on every build
	for ( i = 0; i < PHYS_MAX_BODIES; i++ ) {
		if ( B3_IS_NULL( phys_bodies[owner][i].id ) ) {
			break;
		}
	}
	if ( i == PHYS_MAX_BODIES ) {
		b3DestroyBody( id );
		Com_Printf( S_COLOR_YELLOW "physics: %s body table full\n", phys_ownerNames[owner] );
		return 0;
	}
	phys_bodies[owner][i].id = id;
	phys_bodies[owner][i].world = world;
	phys_bodies[owner][i].userData = userData;
	b3Body_SetUserData( id, (void *)(intptr_t)( i + 1 ) );
	return i + 1;
}

int Phys_BodyCreate( physOwner_t owner, int world, const oaxPhysBodyDef_t *def ) {
	physWorld_t *w = Phys_World( owner, world );
	b3BodyDef bd;

	if ( !w ) {
		return 0;
	}
	bd = b3DefaultBodyDef();
	switch ( def->type ) {
	case PHYS_BODY_KINEMATIC: bd.type = b3_kinematicBody; break;
	case PHYS_BODY_DYNAMIC: bd.type = b3_dynamicBody; break;
	default: bd.type = b3_staticBody; break;
	}
	bd.position = Phys_Vec( def->origin );
	bd.rotation = Phys_Quat( def->quat );
	bd.linearVelocity = Phys_Vec( def->velocity );
	bd.angularVelocity = Phys_Vec( def->angularVelocity );
	bd.linearDamping = def->linearDamping;
	bd.angularDamping = def->angularDamping;
	bd.gravityScale = def->gravityScale;
	bd.isBullet = ( def->flags & PHYS_BODY_BULLET ) ? true : false;
	bd.enableSleep = ( def->flags & PHYS_BODY_NOSLEEP ) ? false : true;
	bd.isAwake = ( def->flags & PHYS_BODY_ASLEEP ) ? false : true;
	bd.isEnabled = ( def->flags & PHYS_BODY_DISABLED ) ? false : true;
	if ( def->flags & PHYS_BODY_LOCKROTATION ) {
		bd.motionLocks.angularX = bd.motionLocks.angularY = bd.motionLocks.angularZ = true;
	}
	return Phys_BodyRegister( owner, world, b3CreateBody( w->id, &bd ), def->userData );
}

void Phys_BodyDestroy( physOwner_t owner, int body ) {
	physBody_t *b = Phys_Body( owner, body );
	int i;

	if ( !b ) {
		return;
	}
	// joints attached to it go with it
	for ( i = 0; i < PHYS_MAX_JOINTS; i++ ) {
		physJoint_t *j = &phys_joints[owner][i];
		if ( !B3_IS_NULL( j->id ) && !b3Joint_IsValid( j->id ) ) {
			Com_Memset( j, 0, sizeof( *j ) );
		}
	}
	b3DestroyBody( b->id );
	for ( i = 0; i < PHYS_MAX_JOINTS; i++ ) {
		physJoint_t *j = &phys_joints[owner][i];
		if ( !B3_IS_NULL( j->id ) && !b3Joint_IsValid( j->id ) ) {
			Com_Memset( j, 0, sizeof( *j ) );
		}
	}
	Com_Memset( b, 0, sizeof( *b ) );
}

void Phys_ShapeDefFrom( const oaxPhysShapeDef_t *def, b3ShapeDef *sd ) {
	const float metersCubed = PHYS_UNITS_PER_METER * PHYS_UNITS_PER_METER * PHYS_UNITS_PER_METER;

	*sd = b3DefaultShapeDef();
	if ( !def ) {
		return;
	}
	sd->density = def->density / metersCubed;
	sd->baseMaterial.friction = def->friction;
	sd->baseMaterial.restitution = def->restitution;
	sd->baseMaterial.rollingResistance = def->rollingResistance;
	sd->baseMaterial.userMaterialId = (uint64_t)(uint32_t)def->userData;
	sd->filter.categoryBits = def->categoryBits ? def->categoryBits : 1u;
	sd->filter.maskBits = def->maskBits ? def->maskBits : 0xffffffffu;
	sd->filter.groupIndex = def->groupIndex;
	sd->isSensor = ( def->flags & PHYS_SHAPE_SENSOR ) ? true : false;
	sd->enableSensorEvents = sd->isSensor;
	sd->enableContactEvents = ( def->flags & PHYS_SHAPE_NOCONTACTEVENTS ) ? false : true;
	sd->enableHitEvents = ( def->flags & PHYS_SHAPE_HITEVENTS ) ? true : false;
	sd->userData = (void *)(intptr_t)def->userData;
}

int Phys_BodyAddShape( physOwner_t owner, int body, const oaxPhysShapeDef_t *def,
		const float *points, int numPoints, const int *indices, int numIndices ) {
	physBody_t *b = Phys_Body( owner, body );
	physWorld_t *w;
	b3ShapeDef sd;
	b3ShapeId shape = b3_nullShapeId;
	b3Transform xf;

	if ( !b || !( w = Phys_World( owner, b->world ) ) ) {
		return 0;
	}
	Phys_ShapeDefFrom( def, &sd );
	xf.p = Phys_Vec( def->offset );
	xf.q = Phys_Quat( def->quat );

	switch ( def->type ) {
	case PHYS_SHAPE_SPHERE: {
		b3Sphere s;
		s.center = Phys_Vec( def->params );
		s.radius = def->params[3];
		if ( s.radius <= 0.0f ) {
			return 0;
		}
		shape = b3CreateSphereShape( b->id, &sd, &s );
		break;
	}
	case PHYS_SHAPE_CAPSULE: {
		b3Capsule c;
		c.center1 = Phys_Vec( def->params );
		c.center2 = Phys_Vec( def->params + 3 );
		c.radius = def->params[6];
		if ( c.radius <= 0.0f ) {
			return 0;
		}
		shape = b3CreateCapsuleShape( b->id, &sd, &c );
		break;
	}
	case PHYS_SHAPE_BOX: {
		b3BoxHull box;
		if ( def->params[0] <= 0.0f || def->params[1] <= 0.0f || def->params[2] <= 0.0f ) {
			return 0;
		}
		box = b3MakeTransformedBoxHull( def->params[0], def->params[1], def->params[2], xf );
		shape = b3CreateHullShape( b->id, &sd, &box.base );
		break;
	}
	case PHYS_SHAPE_HULL: {
		b3Vec3 pts[OAX_PHYS_MAX_HULL_POINTS];
		b3HullData *hull;
		int i;
		if ( !points || numPoints < 4 || numPoints > OAX_PHYS_MAX_HULL_POINTS ) {
			return 0;
		}
		for ( i = 0; i < numPoints; i++ ) {
			pts[i] = b3TransformPoint( xf, Phys_Vec( points + i * 3 ) );
		}
		hull = b3CreateHull( pts, numPoints, numPoints );
		if ( !hull ) {
			return 0;
		}
		shape = b3CreateHullShape( b->id, &sd, hull );
		b3DestroyHull( hull );		// the world keeps its own copy
		break;
	}
	case PHYS_SHAPE_MESH: {
		b3MeshDef md;
		b3MeshData *mesh;
		b3Vec3 *verts;
		int i;
		if ( !points || !indices || numPoints < 3 || numIndices < 3 || numIndices % 3 ) {
			return 0;
		}
		if ( b3Body_GetType( b->id ) == b3_dynamicBody ) {
			return 0;	// Box3D meshes are for static and kinematic bodies
		}
		for ( i = 0; i < numIndices; i++ ) {
			if ( indices[i] < 0 || indices[i] >= numPoints ) {
				return 0;
			}
		}
		verts = Z_Malloc( numPoints * sizeof( b3Vec3 ) );
		for ( i = 0; i < numPoints; i++ ) {
			verts[i] = b3TransformPoint( xf, Phys_Vec( points + i * 3 ) );
		}
		Com_Memset( &md, 0, sizeof( md ) );
		md.vertices = verts;
		md.indices = (int32_t *)indices;
		md.vertexCount = numPoints;
		md.triangleCount = numIndices / 3;
		md.identifyEdges = true;
		mesh = b3CreateMesh( &md, NULL, 0 );
		Z_Free( verts );
		if ( !mesh ) {
			return 0;
		}
		Phys_WorldOwn( w, mesh, 0 );
		shape = b3CreateMeshShape( b->id, &sd, mesh, b3Vec3_one );
		break;
	}
	default:
		return 0;
	}
	if ( B3_IS_NULL( shape ) ) {
		return 0;
	}
	return b3Body_GetShapeCount( b->id );
}

qboolean Phys_BodyState( physOwner_t owner, int body, oaxPhysBodyState_t *out ) {
	physBody_t *b = Phys_Body( owner, body );
	b3WorldTransform xf;
	b3Vec3 v;

	Com_Memset( out, 0, sizeof( *out ) );
	if ( !b ) {
		out->quat[3] = 1.0f;
		return qfalse;
	}
	xf = b3Body_GetTransform( b->id );
	out->origin[0] = xf.p.x;
	out->origin[1] = xf.p.y;
	out->origin[2] = xf.p.z;
	out->quat[0] = xf.q.v.x;
	out->quat[1] = xf.q.v.y;
	out->quat[2] = xf.q.v.z;
	out->quat[3] = xf.q.s;
	v = b3Body_GetLinearVelocity( b->id );
	out->velocity[0] = v.x;
	out->velocity[1] = v.y;
	out->velocity[2] = v.z;
	v = b3Body_GetAngularVelocity( b->id );
	out->angularVelocity[0] = v.x;
	out->angularVelocity[1] = v.y;
	out->angularVelocity[2] = v.z;
	out->flags = PHYS_STATE_VALID | ( b3Body_IsAwake( b->id ) ? PHYS_STATE_AWAKE : 0 );
	out->userData = b->userData;
	return qtrue;
}

/*
==============================================================================
joints
==============================================================================
*/

physJoint_t *Phys_Joint( physOwner_t owner, int joint ) {
	physJoint_t *j;

	if ( joint < 1 || joint > PHYS_MAX_JOINTS ) {
		return NULL;
	}
	j = &phys_joints[owner][joint - 1];
	if ( B3_IS_NULL( j->id ) || !b3Joint_IsValid( j->id ) ) {
		return NULL;
	}
	return j;
}

int Phys_JointCreate( physOwner_t owner, int world, const oaxPhysJointDef_t *def ) {
	physWorld_t *w = Phys_World( owner, world );
	physBody_t *a, *b;
	b3JointDef base;
	b3JointId id = b3_nullJointId;
	int i;

	if ( !w ) {
		return 0;
	}
	b = Phys_Body( owner, def->bodyB );
	a = def->bodyA ? Phys_Body( owner, def->bodyA ) : NULL;
	if ( !b || ( def->bodyA && !a ) || ( a && a->world != world ) || b->world != world ) {
		return 0;
	}
	for ( i = 0; i < PHYS_MAX_JOINTS; i++ ) {
		if ( B3_IS_NULL( phys_joints[owner][i].id ) ) {
			break;
		}
	}
	if ( i == PHYS_MAX_JOINTS ) {
		return 0;
	}

#define PHYS_JOINT_BASE( JD ) \
	JD.base.bodyIdA = a ? a->id : w->ground; \
	JD.base.bodyIdB = b->id; \
	JD.base.localFrameA = Phys_Frame( def->frameA ); \
	JD.base.localFrameB = Phys_Frame( def->frameB ); \
	JD.base.collideConnected = ( def->flags & PHYS_JF_COLLIDE_CONNECTED ) ? true : false; \
	JD.base.userData = (void *)(intptr_t)( i + 1 );

	(void)base;
	switch ( def->type ) {
	case PHYS_JOINT_REVOLUTE: {
		b3RevoluteJointDef jd = b3DefaultRevoluteJointDef();
		PHYS_JOINT_BASE( jd )
		jd.enableLimit = ( def->flags & PHYS_JF_LIMIT ) ? true : false;
		jd.lowerAngle = def->lower;
		jd.upperAngle = def->upper;
		jd.enableSpring = ( def->flags & PHYS_JF_SPRING ) ? true : false;
		jd.hertz = def->hertz;
		jd.dampingRatio = def->dampingRatio;
		jd.targetAngle = def->length;
		jd.enableMotor = ( def->flags & PHYS_JF_MOTOR ) ? true : false;
		jd.motorSpeed = def->motorSpeed;
		jd.maxMotorTorque = def->maxMotorForce;
		id = b3CreateRevoluteJoint( w->id, &jd );
		break;
	}
	case PHYS_JOINT_SPHERICAL: {
		b3SphericalJointDef jd = b3DefaultSphericalJointDef();
		PHYS_JOINT_BASE( jd )
		jd.enableConeLimit = ( def->flags & PHYS_JF_LIMIT ) ? true : false;
		jd.coneAngle = def->coneAngle;
		jd.enableTwistLimit = ( def->flags & PHYS_JF_TWIST_LIMIT ) ? true : false;
		jd.lowerTwistAngle = def->lower;
		jd.upperTwistAngle = def->upper;
		jd.enableSpring = ( def->flags & PHYS_JF_SPRING ) ? true : false;
		jd.hertz = def->hertz;
		jd.dampingRatio = def->dampingRatio;
		jd.enableMotor = ( def->flags & PHYS_JF_MOTOR ) ? true : false;
		jd.maxMotorTorque = def->maxMotorForce;
		id = b3CreateSphericalJoint( w->id, &jd );
		break;
	}
	case PHYS_JOINT_DISTANCE: {
		b3DistanceJointDef jd = b3DefaultDistanceJointDef();
		PHYS_JOINT_BASE( jd )
		jd.length = def->length;
		jd.enableLimit = ( def->flags & PHYS_JF_LIMIT ) ? true : false;
		jd.minLength = def->lower;
		jd.maxLength = def->upper;
		jd.enableSpring = ( def->flags & PHYS_JF_SPRING ) ? true : false;
		jd.hertz = def->hertz;
		jd.dampingRatio = def->dampingRatio;
		jd.enableMotor = ( def->flags & PHYS_JF_MOTOR ) ? true : false;
		jd.motorSpeed = def->motorSpeed;
		jd.maxMotorForce = def->maxMotorForce;
		id = b3CreateDistanceJoint( w->id, &jd );
		break;
	}
	case PHYS_JOINT_WELD: {
		b3WeldJointDef jd = b3DefaultWeldJointDef();
		PHYS_JOINT_BASE( jd )
		jd.linearHertz = def->hertz;
		jd.linearDampingRatio = def->dampingRatio;
		jd.angularHertz = def->angularHertz;
		jd.angularDampingRatio = def->angularDampingRatio;
		id = b3CreateWeldJoint( w->id, &jd );
		break;
	}
	case PHYS_JOINT_WHEEL: {
		b3WheelJointDef jd = b3DefaultWheelJointDef();
		PHYS_JOINT_BASE( jd )
		jd.enableSuspensionSpring = ( def->flags & PHYS_JF_SPRING ) ? true : false;
		jd.suspensionHertz = def->hertz;
		jd.suspensionDampingRatio = def->dampingRatio;
		jd.enableSuspensionLimit = ( def->flags & PHYS_JF_LIMIT ) ? true : false;
		jd.lowerSuspensionLimit = def->lower;
		jd.upperSuspensionLimit = def->upper;
		jd.enableSpinMotor = ( def->flags & PHYS_JF_MOTOR ) ? true : false;
		jd.spinSpeed = def->motorSpeed;
		jd.maxSpinTorque = def->maxMotorForce;
		jd.enableSteering = ( def->flags & PHYS_JF_STEERING ) ? true : false;
		jd.steeringHertz = def->steeringHertz;
		jd.steeringDampingRatio = def->steeringDampingRatio;
		jd.maxSteeringTorque = def->maxSteeringTorque;
		jd.enableSteeringLimit = ( def->flags & PHYS_JF_STEERING_LIMIT ) ? true : false;
		jd.lowerSteeringLimit = def->steeringLower;
		jd.upperSteeringLimit = def->steeringUpper;
		id = b3CreateWheelJoint( w->id, &jd );
		break;
	}
	case PHYS_JOINT_PRISMATIC: {
		b3PrismaticJointDef jd = b3DefaultPrismaticJointDef();
		PHYS_JOINT_BASE( jd )
		jd.enableLimit = ( def->flags & PHYS_JF_LIMIT ) ? true : false;
		jd.lowerTranslation = def->lower;
		jd.upperTranslation = def->upper;
		jd.enableSpring = ( def->flags & PHYS_JF_SPRING ) ? true : false;
		jd.hertz = def->hertz;
		jd.dampingRatio = def->dampingRatio;
		jd.targetTranslation = def->length;
		jd.enableMotor = ( def->flags & PHYS_JF_MOTOR ) ? true : false;
		jd.motorSpeed = def->motorSpeed;
		jd.maxMotorForce = def->maxMotorForce;
		id = b3CreatePrismaticJoint( w->id, &jd );
		break;
	}
	default:
		return 0;
	}
#undef PHYS_JOINT_BASE
	if ( B3_IS_NULL( id ) ) {
		return 0;
	}
	phys_joints[owner][i].id = id;
	phys_joints[owner][i].world = world;
	phys_joints[owner][i].userData = def->userData;
	return i + 1;
}

void Phys_JointDestroy( physOwner_t owner, int joint ) {
	physJoint_t *j = Phys_Joint( owner, joint );

	if ( !j ) {
		return;
	}
	b3DestroyJoint( j->id, true );
	Com_Memset( j, 0, sizeof( *j ) );
}
