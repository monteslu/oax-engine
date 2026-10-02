/*
===========================================================================
oax.h: the engine's extensions to the Quake III game interfaces.

New gamecode ("oax", our OpenArena gamecode fork) reaches new engine
features through syscalls numbered from 1000, which stock syscall tables
never reach, so stock QVMs and stock syscall numbers are untouched. A QVM
reads the read-only cvar `oax_features` (space-separated tokens) before it
calls any of them, and degrades to stock behavior when a token is missing.

Each feature owns a block of numbers, so features can land independently.
Within a block, numbers are append-only: once a QVM ships with a number,
its meaning never changes.
===========================================================================
*/
#ifndef OAX_H
#define OAX_H

#define OAX_VERSION 1

// ---- game (qagame) imports ------------------------------------------------
typedef enum {
	// 1000-1009 infrastructure
	G_OAX_DEBUG_SET = 1000,      // ( const char *name, const char *value )
	G_OAX_BSPX_READ = 1001,      // ( const char *lump, void *buf, int size ) -> lump length, -1 if absent

	// 1010-1039 map scripting (id Tech 4 script VM; idscript/oax_script.h)
	G_OAX_SCRIPT_BASE = 1010,
	G_OAX_SCRIPT_INIT = 1010,            // ( int randomSeed ) -> 1
	G_OAX_SCRIPT_REGISTER_EVENT = 1011,  // ( const char *name, const char *argfmt, int ret, int flags ) -> event number, -1
	G_OAX_SCRIPT_COMPILE_FILE = 1012,    // ( const char *path ) -> 1 ok, 0 compile error, -1 missing
	G_OAX_SCRIPT_SET_ENTITY = 1013,      // ( const char *name, int handle ) -> 1 if scripts use $name
	G_OAX_SCRIPT_START_THREAD = 1014,    // ( const char *func, int self ) -> thread number, 0 if no such function
	G_OAX_SCRIPT_RUN = 1015,             // ( int levelTime, oaxScriptCall_t *call ) -> 0 done, 1 call pending
	G_OAX_SCRIPT_RETURN = 1016,          // ( const oaxScriptValue_t *value, const char *string )
	G_OAX_SCRIPT_OBJECT_DONE = 1017,     // ( int threadNum, int handle )
	G_OAX_SCRIPT_KILL_THREAD = 1018,     // ( int threadNum )
	G_OAX_SCRIPT_SHUTDOWN = 1019,        // ( void )
	G_OAX_SCRIPT_NUM_THREADS = 1020,     // ( void ) -> live threads

	// 1040-1059 in-world GUIs (server/sv_gui_oax.c)
	G_OAX_GUI_BASE = 1040,
	G_OAX_GUI_LOAD = 1040,           // ( const char *guiFile ) -> handle, 0 on failure
	G_OAX_GUI_FREE = 1041,           // ( int handle )
	G_OAX_GUI_SETSTATE = 1042,       // ( int handle, const char *key, const char *value )
	G_OAX_GUI_GETSTATE = 1043,       // ( int handle, const char *key, char *buf, int size ) -> found
	G_OAX_GUI_HANDLE_EVENT = 1044,   // ( int handle, float x, float y, int buttons, int time, char *cmds, int size ) -> length
	G_OAX_GUI_TRACE = 1045,          // ( int entnum, const vec3_t start, const vec3_t end, float *xyFrac ) -> hit
	G_OAX_GUI_ACTIVATE = 1046,       // ( int handle, int activate, int time, char *cmds, int size ) -> length
	G_OAX_GUI_NAMED_EVENT = 1047,    // ( int handle, const char *name, int time, char *cmds, int size ) -> length
	G_OAX_GUI_STATE_INFO = 1048,     // ( int handle, char *buf, int size ) -> length of "\\key\\value..."

	// 1060-1069 area portals and world queries
	G_OAX_PORTAL_BASE = 1060,

	// 1070-1079 unified lighting (phase 5)
	G_OAX_ULIGHT_BASE = 1070,

	// 1090-1099 navigation: Recast/Detour navmesh for bots where AAS is absent (server/sv_nav_oax.c)
	G_OAX_NAV_BASE = 1090,
	G_OAX_NAV_STATUS = 1090,         // ( void ) -> navmesh polygon count, 0 if none
	G_OAX_NAV_FINDPATH = 1091,       // ( const vec3_t start, const vec3_t goal, float *points, int maxPoints, int *flags )
	                                 //   -> corner points written (start first), 0 if an end is off the mesh;
	                                 //   flags & 1: goal unreachable, the path ends nearest to it
	G_OAX_NAV_NEAREST = 1092,        // ( const vec3_t point, const vec3_t halfExtents, vec3_t out ) -> 1 found
	G_OAX_NAV_RANDOMPOINT = 1093,    // ( int seed, vec3_t out ) -> 1 found (area-weighted, deterministic)

	G_OAX_END = 1300	// 1200-1299: physics (oaxPhysImport_t below)
} gameImportOAX_t;

// ---- cgame imports ---------------------------------------------------------
typedef enum {
	// 1000-1009 infrastructure
	CG_OAX_DEBUG_SET = 1000,     // ( const char *name, const char *value )
	CG_OAX_BSPX_READ = 1001,     // ( const char *lump, void *buf, int size ) -> lump length, -1 if absent

	// 1010-1019 world rendering: light styles, view fog, sky portal
	CG_OAX_R_BASE = 1010,
	CG_OAX_R_SETLIGHTSTYLE = 1010,	// ( int style, float r, float g, float b ): 1 = as compiled, 0 = off, up to 2
	// 1011 reserved (sky portal needs no syscall: refdef flags 0x0100 / 0x0200)
	CG_OAX_R_SETVIEWFOG = 1012,		// ( const float *rgb, float density, float start, float end ); density 0 = off.
									// end > start: linear from start to end up to `density` (0..1);
									// else exponential 1 - exp(-density * (dist - start)). Stays set.
	// effects (phase 6; tokens "particles", "decals", "trails"; renderergl2 tr_oax_fx*.c)
	CG_OAX_R_REGISTERFX = 1013,		// ( const char *particleDecl ) -> handle, 0 if there is no such decl
	CG_OAX_R_ADDFX = 1014,			// ( const oaxFx_t *fx ) -> 1 while the system has particles alive or to come, 0 once done
	CG_OAX_R_ADDDECAL = 1015,		// ( const oaxDecal_t *decal ) -> polygons projected, 0 if none (or decals are off)
	CG_OAX_R_ADDTRAIL = 1016,		// ( const oaxTrail_t *trail, const float *points ): numPoints * (x y z ageMs)
	CG_OAX_R_CLEARDECALS = 1017,	// ( void )

	// 1020-1029 sound: zone reverb, occlusion
	CG_OAX_S_BASE = 1020,
	CG_OAX_S_SETREVERB = 1020,  // ( const char *preset, float decay, float wet ); "" + 0 = off (snd_reverb.c)

	// 1030-1049 in-world GUIs (client/cl_gui_oax.c)
	CG_OAX_GUI_BASE = 1030,
	CG_OAX_GUI_LOAD = 1030,          // ( const char *guiFile ) -> handle, 0 on failure
	CG_OAX_GUI_FREE = 1031,          // ( int handle )
	CG_OAX_GUI_SETSTATE = 1032,      // ( int handle, const char *key, const char *value )
	CG_OAX_GUI_ACTIVATE = 1033,      // ( int handle, int activate )
	CG_OAX_R_ADDREFENTITYEXT = 1034, // ( const refEntity_t *re, const refEntityExt_t *ext )
	CG_OAX_GUI_TRACE = 1035,         // ( int inlineModel, origin, angles, start, end, float *xyFrac ) -> hit
	CG_OAX_GUI_CURSOR = 1036,        // ( int handle, float x, float y ): cosmetic hover

	// 1050-1069 unified lighting (phase 5)
	CG_OAX_ULIGHT_BASE = 1050,
	CG_OAX_R_UPDATELIGHTDEF = 1050, // ( int lightOrdinal, const vec3_t origin, const vec3_t axis[3] or 0,
	                                //   const vec3_t rgb, const float parms[12] or 0, int flags: 1 on )

	CG_OAX_END = 1300	// 1200-1299: physics (oaxPhysImport_t below)
} cgameImportOAX_t;

// ---- physics, game AND cgame (one block, same numbers, same meaning) -------
// Box3D worlds (code/physics, structs in physics/oax_phys.h). Token
// "physics"; the skeleton calls need "physics_skel" (cgame only).
#define OAX_PHYS_BASE 1200
typedef enum {
	PHYS_WORLD_CREATE = 1200,       // ( const oaxPhysWorldDef_t *def ) -> world, 0 on failure
	PHYS_WORLD_DESTROY = 1201,      // ( int world )
	PHYS_WORLD_STEP = 1202,         // ( int world, int msec ) -> ticks run; msec < 0 runs exactly -msec ticks
	PHYS_WORLD_ADD_BSP = 1203,      // ( int world, int contentsMask, int flags, const oaxPhysShapeDef_t *material or 0 ) -> static body
	PHYS_WORLD_ADD_HEIGHTFIELD = 1204, // ( int world, const oaxPhysHeightField_t *hf, const float *heights, const oaxPhysShapeDef_t *material or 0 ) -> static body
	PHYS_WORLD_SET_GRAVITY = 1205,  // ( int world, const vec3_t gravity )
	PHYS_WORLD_STATS = 1206,        // ( int world, oaxPhysStats_t *out ) -> 1
	PHYS_WORLD_HASH = 1207,         // ( int world ) -> hash of every body's state (handle order)
	PHYS_WORLD_EXPLODE = 1208,      // ( int world, const vec3_t origin, float radius, float falloff, float impulsePerArea, int maskBits )
	PHYS_WORLD_CONTACT_EVENTS = 1209, // ( int world, oaxPhysContact_t *out, int max ) -> count (events of the ticks since the last call)
	PHYS_BODY_CREATE = 1210,        // ( int world, const oaxPhysBodyDef_t *def ) -> body
	PHYS_BODY_DESTROY = 1211,       // ( int body )
	PHYS_BODY_ADD_SHAPE = 1212,     // ( int body, const oaxPhysShapeDef_t *def, const float *points, int numPoints, const int *indices, int numIndices ) -> 1-based shape index, 0
	PHYS_BODY_SET_TRANSFORM = 1213, // ( int body, const vec3_t origin, const float *quat ): teleport
	PHYS_BODY_SET_VELOCITY = 1214,  // ( int body, const vec3_t velocity or 0, const vec3_t angularVelocity or 0 )
	PHYS_BODY_APPLY = 1215,         // ( int body, int kind, const vec3_t vec, const vec3_t point or 0 ) kind PHYS_APPLY_*
	PHYS_BODY_SET_TARGET = 1216,    // ( int body, const vec3_t origin, const float *quat, float seconds ): kinematic move
	PHYS_BODY_SET_PARAM = 1217,     // ( int body, int param, float value ) param PHYS_BP_*
	PHYS_BODY_GET_STATE = 1218,     // ( int body, oaxPhysBodyState_t *out ) -> 1 valid, 0
	PHYS_BODY_GET_STATES = 1219,    // ( const int *bodies, int count, oaxPhysBodyState_t *out ) -> valid count
	PHYS_BODY_FROM_BSP_MODEL = 1220, // ( int world, int inlineModel, int type, const oaxPhysShapeDef_t *material or 0 ) -> body of the brush model's hulls
	PHYS_BODY_GET_MASS = 1221,      // ( int body ) -> float kg
	PHYS_RAGDOLL_CREATE = 1222,     // ( int world, const oaxPhysRagdollDef_t *def, const oaxPhysRagdollBone_t *bones, int numBones, int *outBodies ) -> bodies made
	PHYS_JOINT_CREATE = 1230,       // ( int world, const oaxPhysJointDef_t *def ) -> joint
	PHYS_JOINT_DESTROY = 1231,      // ( int joint )
	PHYS_JOINT_SET_PARAM = 1232,    // ( int joint, int param, float value ) param PHYS_JP_*
	PHYS_JOINT_GET_PARAM = 1233,    // ( int joint, int param ) -> float
	PHYS_RAYCAST = 1240,            // ( int world, const oaxPhysRay_t *ray, oaxPhysHit_t *out ) -> 1 hit, 0 miss (closest hit)
	PHYS_RAYCAST_BATCH = 1241,      // ( int world, const oaxPhysRay_t *rays, int count, oaxPhysHit_t *out ) -> hits (raycast wheels)
	PHYS_SHAPECAST = 1242,          // ( int world, const oaxPhysShapeDef_t *shape, const float *points, int numPoints, const oaxPhysRay_t *ray, const float *quat, oaxPhysHit_t *out ) -> 1 hit
	PHYS_OVERLAP = 1243,            // ( int world, const oaxPhysShapeDef_t *shape, const float *points, int numPoints, const vec3_t origin, const float *quat, unsigned maskBits, int *bodies, int max ) -> count
	PHYS_R_MODEL_SKELETON = 1250,   // cgame: ( qhandle_t model, oaxSkelJoint_t *out, int max ) -> joints, 0 if not skeletal
	PHYS_R_LERP_SKELETON = 1251,    // cgame: ( qhandle_t model, int frame, int oldframe, float backlerp, float *mats, int max ) -> joints
	PHYS_R_ADD_SKELETAL_ENTITY = 1252, // cgame: ( const refEntity_t *re, const float *mats, int numJoints ): draw with these joint matrices
	PHYS_R_MODEL_FRAMES = 1253,     // cgame: ( qhandle_t model ) -> frames
	// 1260-1269 vehicles and terrain (phys_vehicle.c, token "physics_vehicle")
	PHYS_VEHICLE_CREATE = 1260,     // ( int world, const oaxPhysVehicleDef_t *def ) -> vehicle, 0 on failure
	PHYS_VEHICLE_DESTROY = 1261,    // ( int vehicle ): the chassis body goes too
	PHYS_VEHICLE_SET_INPUT = 1262,  // ( int vehicle, const oaxPhysVehicleInput_t *in ): held until changed
	PHYS_VEHICLE_GET_STATE = 1263,  // ( int vehicle, oaxPhysVehicleState_t *out ) -> 1 valid, 0
	PHYS_WORLD_ADD_TERRAIN = 1264,  // ( int world, const oaxPhysShapeDef_t *material or 0, int *bodies, int max ) -> static bodies made
	OAX_PHYS_END = 1300
} oaxPhysImport_t;

// surfaceparm gui (tests/maps custinfoparms.txt): a face that shows the
// GUI of its entity. Above every stock Q3 surface flag.
#define SURF_OAX_GUI	0x01000000
// refdef flags of the sky portal feature (renderergl2 tr_oax.h has the same)
#define OAX_RDF_SKYPORTAL	0x0100	// the sky room seen through a sky portal
#define OAX_RDF_UNDERSKY	0x0200	// a scene drawn over a sky portal scene

// one handler per feature block; returns qtrue if it took the call
typedef qboolean ( *oaxSyscallHandler_t )( intptr_t *args, intptr_t *ret );

void OAX_Init( void );
void OAX_AddFeature( const char *token );

// named debug values: published by engine and QVM code, read by tests
// (wasmcart debug field "debug_values", `debugvalues <file>` on any build)
void Com_DebugSet( const char *name, const char *value );
void Com_DebugSetInt( const char *name, int value );
void Com_DebugSetFloat( const char *name, float value );
int Com_DebugValuesText( char *buf, int size );

// BSPX extension lumps (bspx.c)
const void *BSPX_Find( const void *bsp, int bspLen, const char *name, int *outLen );
int BSPX_ReadCurrentMap( const char *name, void *buf, int size );
void BSPX_SetCurrentMap( const void *bsp, int bspLen );

#endif
