/*
===========================================================================
oax_phys.h: the physics syscall ABI (Box3D worlds for gamecode).

Identical in oa-engine (code/physics/oax_phys.h) and oa-gamecode
(code/game/oax_phys.h): keep the two in sync. Layouts never change once
shipped; new things are appended. The syscall numbers are in the engine's
qcommon/oax.h and the gamecode's oax_public.h, block 1200-1299; the
game (qagame) and the cgame use the SAME numbers and the same semantics.
Feature token: "physics" (and "physics_skel" for the cgame skeleton calls).

WHO OWNS WHAT
  Each VM owns the worlds it creates. Handles are private to the VM: the
  game cannot touch a cgame world or body and the reverse. Everything a VM
  owns is destroyed when that VM restarts (map change, vid_restart for the
  cgame), so gamecode never has to clean up on shutdown.
  - The server (game) runs AUTHORITATIVE worlds: vehicles, gameplay
    objects. They are deterministic: the same calls with the same arguments
    give the same bits on every build (native x86-64, the wasm cart) and for
    any workerCount (the physics-determinism test).
  - The client (cgame) runs COSMETIC worlds: gibs, brass, debris, ragdolls.
    Same API; nothing in them may feed back into gameplay.

UNITS
  Lengths are Quake units (the engine sets Box3D's length scale to 32 units
  per meter), time in seconds, angles in radians, mass in kg. Densities are
  kg per cubic METER (water = 1000; the engine converts). Forces are
  kg*units/s^2, impulses kg*units/s, torques kg*units^2/s^2. Z is up.
  Rotations are quaternions x y z w (all four zero means identity).

HANDLES
  0 is "none" or "failed" everywhere. World handles are 1..8, body and
  joint handles are > 0. bodyA 0 in a joint means the world's static ground
  body. A body handle stays valid until PHYS_BODY_DESTROY or the world goes.

TIME
  PHYS_WORLD_STEP( world, msec ) adds msec to the world's accumulator and
  runs whole fixed ticks of def.tickMsec (default 16) with def.substeps
  (default 4) solver substeps each, so the result depends only on the
  ticks, never on how the frames were sliced. It returns the ticks run.
  PHYS_WORLD_STEP( world, -n ) runs exactly n ticks (scripted tests).
===========================================================================
*/
#ifndef OAX_PHYS_H
#define OAX_PHYS_H

#define OAX_PHYS_VERSION        1
#define OAX_PHYS_MAX_WORLDS     8
#define OAX_PHYS_MAX_WORKERS    8
#define OAX_PHYS_MAX_HULL_POINTS 64     /* points for one PHYS_SHAPE_HULL */

/* ---- world -------------------------------------------------------------- */

#define PHYS_WORLD_NOSLEEP          0x0001  /* bodies never fall asleep */
#define PHYS_WORLD_NOCONTINUOUS     0x0002  /* no continuous collision vs static */

typedef struct {
	float   gravity[3];         /* units/s^2; game gravity is 0 0 -g_gravity */
	int     workerCount;        /* 1..8; results identical for every count */
	int     tickMsec;           /* fixed tick for PHYS_WORLD_STEP, 0 = 16 */
	int     substeps;           /* solver substeps per tick, 0 = 4 */
	int     flags;              /* PHYS_WORLD_* */
	float   contactHertz;       /* 0 = Box3D default (30) */
	float   contactDampingRatio;/* 0 = Box3D default (10) */
	float   contactSpeed;       /* max push-out speed, units/s; 0 = default (96) */
	float   maxLinearSpeed;     /* units/s; 0 = default (12800) */
	float   restitutionThreshold; /* units/s; 0 = default (32) */
	float   hitEventThreshold;  /* units/s; 0 = default (32) */
} oaxPhysWorldDef_t;

/* what PHYS_WORLD_STATS writes (and what the debug values publish) */
typedef struct {
	int     bodies;             /* live bodies, static ones included */
	int     awakeBodies;
	int     shapes;
	int     joints;
	int     contacts;           /* touching contact pairs */
	int     ticks;              /* ticks run since the world was created */
	float   lastStepMs;         /* wall time of the last tick (0 on a cart replay) */
	float   accumMs;            /* time waiting in the accumulator, for interpolation */
	unsigned hash;              /* PHYS_WORLD_HASH */
	int     workerCount;
} oaxPhysStats_t;

/* ---- bodies ------------------------------------------------------------- */

#define PHYS_BODY_STATIC        0
#define PHYS_BODY_KINEMATIC     1
#define PHYS_BODY_DYNAMIC       2

#define PHYS_BODY_BULLET        0x0001  /* continuous vs dynamic bodies too */
#define PHYS_BODY_NOSLEEP       0x0002
#define PHYS_BODY_ASLEEP        0x0004  /* created asleep */
#define PHYS_BODY_LOCKROTATION  0x0008  /* no angular motion */
#define PHYS_BODY_DISABLED      0x0010  /* created disabled (not simulated) */

typedef struct {
	int     type;               /* PHYS_BODY_* */
	float   origin[3];
	float   quat[4];
	float   velocity[3];        /* units/s */
	float   angularVelocity[3]; /* radians/s, world axes */
	float   linearDamping;
	float   angularDamping;
	float   gravityScale;       /* NOTE 0 means no gravity; use 1 for normal */
	int     flags;              /* PHYS_BODY_* flags */
	int     userData;           /* returned in contacts and hits */
} oaxPhysBodyDef_t;

/* PHYS_BODY_APPLY kinds */
#define PHYS_APPLY_FORCE            0   /* vec at point (point 0 = center) */
#define PHYS_APPLY_IMPULSE          1   /* vec at point (point 0 = center) */
#define PHYS_APPLY_TORQUE           2
#define PHYS_APPLY_ANGULAR_IMPULSE  3

/* PHYS_BODY_SET_PARAM */
#define PHYS_BP_LINEAR_DAMPING      0
#define PHYS_BP_ANGULAR_DAMPING     1
#define PHYS_BP_GRAVITY_SCALE       2
#define PHYS_BP_AWAKE               3   /* 1 wake, 0 sleep */
#define PHYS_BP_ENABLED             4   /* 1 enable, 0 disable */
#define PHYS_BP_TYPE                5   /* PHYS_BODY_STATIC / KINEMATIC / DYNAMIC */
#define PHYS_BP_BULLET              6
#define PHYS_BP_USERDATA            7   /* value is cast to int */
#define PHYS_BP_SLEEP_ENABLED       8

#define PHYS_STATE_AWAKE    0x0001
#define PHYS_STATE_VALID    0x0002

/* PHYS_BODY_GET_STATE(S) */
typedef struct {
	float   origin[3];
	float   quat[4];
	float   velocity[3];
	float   angularVelocity[3];
	int     flags;              /* PHYS_STATE_* */
	int     userData;
} oaxPhysBodyState_t;

/* ---- shapes ------------------------------------------------------------- */

#define PHYS_SHAPE_SPHERE   0   /* params: center[3], radius */
#define PHYS_SHAPE_CAPSULE  1   /* params: center1[3], center2[3], radius */
#define PHYS_SHAPE_BOX      2   /* params: halfExtents[3]; offset/quat place it */
#define PHYS_SHAPE_HULL     3   /* points[numPoints] (<= 64); offset/quat place it */
#define PHYS_SHAPE_MESH     4   /* points + indices (3 per triangle); static or
                                   kinematic bodies only; offset/quat place it */

#define PHYS_SHAPE_SENSOR           0x0001
#define PHYS_SHAPE_NOCONTACTEVENTS  0x0002  /* no begin/end touch events */
#define PHYS_SHAPE_HITEVENTS        0x0004  /* hit events above the threshold */

typedef struct {
	int     type;               /* PHYS_SHAPE_* */
	float   params[8];
	float   offset[3];          /* box/hull/mesh placement in the body */
	float   quat[4];
	float   density;            /* kg per cubic meter; 0 = massless */
	float   friction;
	float   restitution;
	float   rollingResistance;
	unsigned categoryBits;      /* 0 = 1 */
	unsigned maskBits;          /* 0 = all */
	int     groupIndex;         /* Box3D group: < 0 never collide with the same group */
	int     flags;              /* PHYS_SHAPE_* flags */
	int     userData;           /* returned in contacts and hits */
} oaxPhysShapeDef_t;

/* PHYS_WORLD_ADD_BSP flags */
#define PHYS_BSP_PATCHES    0x0001  /* curved surfaces as triangle meshes */
#define PHYS_BSP_TRISOUPS   0x0002  /* triangle soups (models) as triangle meshes */

/* PHYS_WORLD_ADD_HEIGHTFIELD: a grid in the XY plane, heights along Z.
   Sample (i, j) sits at origin + (i * cellSize[0], j * cellSize[1]),
   heights[j * countX + i] above origin[2]. */
typedef struct {
	float   origin[3];
	float   cellSize[2];
	int     countX;             /* grid lines along x, >= 2 */
	int     countY;             /* grid lines along y, >= 2 */
	float   minHeight;          /* quantization range; heights are clamped */
	float   maxHeight;
} oaxPhysHeightField_t;

/* ---- joints ------------------------------------------------------------- */

#define PHYS_JOINT_REVOLUTE     0   /* hinge about frame z */
#define PHYS_JOINT_SPHERICAL    1   /* ball; cone about frameA z, twist about frameB z */
#define PHYS_JOINT_DISTANCE     2   /* rope / spring between the frame origins */
#define PHYS_JOINT_WELD         3
#define PHYS_JOINT_WHEEL        4   /* A = chassis, B = wheel: suspension along frameA x,
                                       spin about frameB z, optional steering */
#define PHYS_JOINT_PRISMATIC    5   /* slide along frameA x */

#define PHYS_JF_COLLIDE_CONNECTED   0x0001
#define PHYS_JF_LIMIT               0x0002  /* revolute angles, prismatic/wheel translation,
                                               distance length range, spherical cone */
#define PHYS_JF_SPRING              0x0004
#define PHYS_JF_MOTOR               0x0008
#define PHYS_JF_TWIST_LIMIT         0x0010  /* spherical */
#define PHYS_JF_STEERING            0x0020  /* wheel */
#define PHYS_JF_STEERING_LIMIT      0x0040  /* wheel */

typedef struct {
	int     type;               /* PHYS_JOINT_* */
	int     bodyA;              /* 0 = the world's ground */
	int     bodyB;
	float   frameA[7];          /* origin[3] quat[4] in body A space (world space if bodyA 0) */
	float   frameB[7];          /* origin[3] quat[4] in body B space */
	int     flags;              /* PHYS_JF_* */
	float   lower;              /* limits: radians (revolute, spherical twist), units
	                               (prismatic, wheel suspension, distance min length) */
	float   upper;
	float   coneAngle;          /* spherical cone limit, radians */
	float   hertz;              /* spring (all types; weld: linear) */
	float   dampingRatio;
	float   angularHertz;       /* weld angular spring */
	float   angularDampingRatio;
	float   motorSpeed;         /* rad/s or units/s (wheel: spin speed) */
	float   maxMotorForce;      /* torque for revolute/spherical/wheel spin, force otherwise */
	float   length;             /* distance rest length; prismatic/revolute spring target */
	float   steeringHertz;      /* wheel */
	float   steeringDampingRatio;
	float   maxSteeringTorque;
	float   steeringLower;
	float   steeringUpper;
	int     userData;
} oaxPhysJointDef_t;

/* PHYS_JOINT_SET_PARAM / GET_PARAM */
#define PHYS_JP_MOTOR_SPEED         0   /* set/get */
#define PHYS_JP_MAX_MOTOR_FORCE     1   /* set/get */
#define PHYS_JP_SPRING_HERTZ        2   /* set */
#define PHYS_JP_SPRING_DAMPING      3   /* set */
#define PHYS_JP_TARGET              4   /* set: revolute angle, prismatic translation,
                                           wheel steering angle */
#define PHYS_JP_LOWER               5   /* set: limit (keeps upper) */
#define PHYS_JP_UPPER               6   /* set */
#define PHYS_JP_ENABLE_MOTOR        7   /* set 0/1 */
#define PHYS_JP_ENABLE_LIMIT        8   /* set 0/1 */
#define PHYS_JP_ENABLE_SPRING       9   /* set 0/1 */
#define PHYS_JP_ANGLE               10  /* get: revolute angle, spherical cone angle */
#define PHYS_JP_TRANSLATION         11  /* get: prismatic translation */
#define PHYS_JP_SPEED               12  /* get: revolute/prismatic speed, wheel spin speed */
#define PHYS_JP_MOTOR_OUTPUT        13  /* get: motor torque/force this tick */
#define PHYS_JP_LENGTH              14  /* get: distance current length; set: rest length */
#define PHYS_JP_CONSTRAINT_FORCE    15  /* get: magnitude of the constraint force */

/* ---- queries ------------------------------------------------------------ */

typedef struct {
	float   fraction;           /* 0..1 along start->end */
	float   point[3];
	float   normal[3];
	int     body;               /* body handle */
	int     bodyUserData;
	int     shapeUserData;
	int     hit;                /* 1 hit, 0 miss */
} oaxPhysHit_t;

typedef struct {
	float   start[3];
	float   end[3];
	unsigned maskBits;          /* shapes whose categoryBits & maskBits != 0; 0 = all */
	int     ignoreBody;         /* body handle to skip (the vehicle itself), 0 = none */
} oaxPhysRay_t;

/* ---- events ------------------------------------------------------------- */

#define PHYS_CONTACT_BEGIN  1
#define PHYS_CONTACT_END    2
#define PHYS_CONTACT_HIT    3   /* point, normal, speed set; shapes need PHYS_SHAPE_HITEVENTS */

typedef struct {
	int     type;               /* PHYS_CONTACT_* */
	int     bodyA, bodyB;       /* handles (0 if that body is gone) */
	int     userA, userB;       /* body userData */
	int     shapeUserA, shapeUserB;
	float   point[3];
	float   normal[3];          /* from A to B */
	float   speed;              /* approach speed, units/s */
} oaxPhysContact_t;

/* ---- ragdolls ----------------------------------------------------------- */

/* One capsule body per bone, from head to tail (world space at creation),
   joined to its parent at its head. */
#define PHYS_RAGDOLL_BALL   0   /* spherical: cone about the bone, twist limits */
#define PHYS_RAGDOLL_HINGE  1   /* revolute about `axis` (knees, elbows) */

typedef struct {
	int     parent;             /* bone index, -1 for the root */
	float   head[3];
	float   tail[3];
	float   radius;
	float   density;            /* kg/m^3, 0 = 1000 */
	int     jointType;          /* PHYS_RAGDOLL_* */
	float   axis[3];            /* hinge axis (world) */
	float   coneAngle;          /* ball */
	float   lower, upper;       /* twist (ball) or hinge limits, radians */
	float   friction;           /* joint friction torque, 0 = none */
} oaxPhysRagdollBone_t;

typedef struct {
	float   velocity[3];        /* given to every bone body */
	int     groupIndex;         /* < 0: bones of this ragdoll never collide with each other */
	unsigned categoryBits;      /* 0 = 1 */
	unsigned maskBits;          /* 0 = all */
	float   friction;           /* shapes; 0 = 0.6 */
	float   linearDamping;
	float   angularDamping;
	float   jointHertz;         /* soft pose spring toward the start pose, 0 = none */
	float   jointDampingRatio;
	int     userData;
} oaxPhysRagdollDef_t;

/* ---- skeletons (cgame only, renderer side; token "physics_skel") -------- */

typedef struct {
	char    name[32];
	int     parent;             /* joint index, -1 root */
	float   origin[3];          /* bind pose, model space */
	float   quat[4];
} oaxSkelJoint_t;

/* joint matrices for PHYS_R_LERP_SKELETON / PHYS_R_ADD_SKELETAL_ENTITY:
   12 floats per joint, model space, row major 3x4: [ axis0 origin0 ... ],
   i.e. m[r*4+c], column 3 is the translation (the IQM convention). */
#define PHYS_SKEL_MAT_FLOATS 12

#endif
