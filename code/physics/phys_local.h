/*
===========================================================================
phys_local.h: the engine physics module's internals (code/physics).

Owns Box3D worlds for gamecode. The syscall ABI (structs, constants) is
oax_phys.h; the numbers are in qcommon/oax.h. Engine code that wants a
world of its own uses the Phys_* functions below with PHYS_OWNER_ENGINE.
===========================================================================
*/
#ifndef PHYS_LOCAL_H
#define PHYS_LOCAL_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../qcommon/oax.h"
#include "oax_phys.h"

#include "box3d/box3d.h"

// who created a world; handles never cross owners
typedef enum {
	PHYS_OWNER_GAME,
	PHYS_OWNER_CGAME,
	PHYS_OWNER_ENGINE,
	PHYS_NUM_OWNERS
} physOwner_t;

// 32 Quake units to the meter: the player is 56 units, 1.75 m
#define PHYS_UNITS_PER_METER	32.0f

#define PHYS_MAX_BODIES		8192	// per owner
#define PHYS_MAX_JOINTS		4096	// per owner
#define PHYS_MAX_EVENTS		1024	// per world, between two CONTACT_EVENTS calls

typedef struct {
	qboolean		inuse;
	physOwner_t		owner;
	b3WorldId		id;
	b3BodyId		ground;			// static body joints use for bodyA 0
	int				tickMsec;
	int				substeps;
	int				accumMsec;
	int				ticks;
	int				workerCount;
	float			lastStepMs;
	// geometry a shape references (meshes, height fields): freed with the world
	void			**owned;
	byte			*ownedKind;		// 0 mesh, 1 height field
	int				numOwned, maxOwned;
	// contact events gathered tick by tick until gamecode reads them
	oaxPhysContact_t *events;
	int				numEvents;
	int				droppedEvents;
} physWorld_t;

typedef struct {
	b3BodyId		id;				// index1 0 = free slot
	int				world;			// world handle
	int				userData;
} physBody_t;

typedef struct {
	b3JointId		id;
	int				world;
	int				userData;
} physJoint_t;

extern physWorld_t	phys_worlds[OAX_PHYS_MAX_WORLDS];
extern cvar_t		*phys_workers;

void		Phys_Init( void );
void		Phys_FreeOwner( physOwner_t owner );

// worlds
int			Phys_WorldCreate( physOwner_t owner, const oaxPhysWorldDef_t *def );
void		Phys_WorldDestroy( physOwner_t owner, int world );
physWorld_t	*Phys_World( physOwner_t owner, int world );
int			Phys_WorldStep( physOwner_t owner, int world, int msec );
void		Phys_WorldStats( physOwner_t owner, int world, oaxPhysStats_t *out );
unsigned	Phys_WorldHash( physOwner_t owner, int world );
void		Phys_WorldOwn( physWorld_t *w, void *blob, int kind );
int			Phys_WorldContactEvents( physOwner_t owner, int world, oaxPhysContact_t *out, int max );

// bodies
int			Phys_BodyCreate( physOwner_t owner, int world, const oaxPhysBodyDef_t *def );
int			Phys_BodyRegister( physOwner_t owner, int world, b3BodyId id, int userData );
void		Phys_BodyDestroy( physOwner_t owner, int body );
physBody_t	*Phys_Body( physOwner_t owner, int body );
int			Phys_BodyAddShape( physOwner_t owner, int body, const oaxPhysShapeDef_t *def,
				const float *points, int numPoints, const int *indices, int numIndices );
qboolean	Phys_BodyState( physOwner_t owner, int body, oaxPhysBodyState_t *out );
void		Phys_ShapeDefFrom( const oaxPhysShapeDef_t *def, b3ShapeDef *sd );

// joints
int			Phys_JointCreate( physOwner_t owner, int world, const oaxPhysJointDef_t *def );
void		Phys_JointDestroy( physOwner_t owner, int joint );
physJoint_t	*Phys_Joint( physOwner_t owner, int joint );

// static collision (phys_bsp.c)
int			Phys_AddBSP( physOwner_t owner, int world, int contentsMask, int flags, const oaxPhysShapeDef_t *material );
int			Phys_BodyFromBSPModel( physOwner_t owner, int world, int inlineModel, int type, const oaxPhysShapeDef_t *material );
int			Phys_AddHeightField( physOwner_t owner, int world, const oaxPhysHeightField_t *hf,
				const float *heights, const oaxPhysShapeDef_t *material );

// ragdolls (phys_ragdoll.c)
int			Phys_RagdollCreate( physOwner_t owner, int world, const oaxPhysRagdollDef_t *def,
				const oaxPhysRagdollBone_t *bones, int numBones, int *outBodies );

// vehicles and terrain (phys_vehicle.c, phase 8)
void		Phys_VehiclesWorldDestroyed( physOwner_t owner, int world );
void		Phys_VehiclesBeforeTick( physOwner_t owner, int world, physWorld_t *w, float dt );
void		Phys_VehiclesAfterTick( physOwner_t owner, int world );
qboolean	Phys_VehicleSyscall( physOwner_t owner, intptr_t *args, intptr_t *ret );

// the syscall block, shared by the game and cgame dispatchers (phys_syscalls.c)
qboolean	Phys_Syscall( physOwner_t owner, intptr_t *args, intptr_t *ret );

// task system for worker threads where Box3D's own scheduler cannot run
// (the cart); phys_tasks.c
qboolean	Phys_TasksAvailable( void );
void		Phys_TasksSetup( b3WorldDef *def, int workerCount );
void		Phys_TasksBeginStep( void );
void		Phys_TasksPublish( void );

// math helpers
b3Quat		Phys_Quat( const float *q );
b3Vec3		Phys_Vec( const float *v );
b3Transform	Phys_Frame( const float *f7 );

#endif
