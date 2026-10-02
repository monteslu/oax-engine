/*
===========================================================================
tr_oax_fx.h: renderergl2 effects (phase 6): stateless GPU particles, soft
particles, projected decals, water, ribbon trails and bloom.

Types and constants only; the prototypes are at the end of tr_local.h,
after the types they use.
===========================================================================
*/
#ifndef TR_OAX_FX_H
#define TR_OAX_FX_H

// ---- particle decls (tr_oax_particle.c, after DOOM-3's idDeclParticle) ----

#define OAX_PRT_MAX_DECLS		256
#define OAX_PRT_MAX_STAGES		8
#define OAX_PRT_MAX_PARTICLES	4096	// per stage; the static index buffer holds this many quads

typedef enum {
	OAX_PDIST_RECT,			// ( sizeX sizeY sizeZ )
	OAX_PDIST_CYLINDER,		// ( sizeX sizeY sizeZ ringFraction )
	OAX_PDIST_SPHERE		// ( sizeX sizeY sizeZ ringFraction )
} oaxPrtDistribution_t;

typedef enum {
	OAX_PDIR_CONE,			// parm0 is the solid cone angle
	OAX_PDIR_OUTWARD		// direction is the offset from the origin, parm0 is an upward bias
} oaxPrtDirection_t;

typedef enum {
	OAX_PPATH_STANDARD,
	OAX_PPATH_HELIX,		// ( sizeX sizeY sizeZ radialSpeed climbSpeed )
	OAX_PPATH_FLIES,		// ( radialSpeed axialSpeed size )
	OAX_PPATH_ORBIT,		// ( radius speed )
	OAX_PPATH_DRIP			// ( speed )
} oaxPrtPath_t;

typedef enum {
	OAX_POR_VIEW,
	OAX_POR_AIMED,			// a quad from where the particle was trailTime ago to where it is
	OAX_POR_X,
	OAX_POR_Y,
	OAX_POR_Z
} oaxPrtOrientation_t;

typedef struct {
	float	from, to;
} oaxPrtParm_t;

typedef struct {
	struct shader_s	*material;
	int		totalParticles;
	float	cycles;				// 0 = forever
	int		cycleMsec;			// ( particleLife + deadTime ) in msec
	float	spawnBunching;		// 0 = all at once, 1 = evenly over the cycle
	float	particleLife;		// seconds
	float	timeOffset;			// seconds after the system starts
	float	deadTime;			// seconds after particleLife before respawning

	int		distributionType;
	float	distributionParms[4];
	int		directionType;
	float	directionParms[4];
	oaxPrtParm_t speed;
	float	gravity;
	qboolean worldGravity;
	qboolean randomDistribution;
	qboolean entityColor;

	int		customPathType;
	float	customPathParms[8];

	vec3_t	offset;
	int		animationFrames;
	float	animationRate;
	float	initialAngle;
	oaxPrtParm_t rotationSpeed;
	int		orientation;
	float	orientationParms[4];
	oaxPrtParm_t size;
	oaxPrtParm_t aspect;
	vec4_t	color;
	vec4_t	fadeColor;
	float	fadeInFraction;
	float	fadeOutFraction;
	float	fadeIndexFraction;
	float	boundsExpansion;

	// oax additions
	float	softDistance;		// soft particles: fade over this many units in front of the scene, < 0 = off, 0 = r_oaxSoftParticles default

	float	radius;				// derived: bounding radius around the system origin
} oaxPrtStage_t;

typedef struct {
	char	name[MAX_QPATH];
	int		numStages;
	oaxPrtStage_t stages[OAX_PRT_MAX_STAGES];
	float	radius;				// derived: largest stage radius
	int		durationMsec;		// derived: when a system with finite cycles has drawn its last particle, 0 = never ends
} oaxPrtDecl_t;

// ---- per-frame scene lists (tr_oax_fx.c) ----

#define OAX_MAX_SCENE_FX		512
#define OAX_MAX_SCENE_TRAILS	128
#define OAX_MAX_TRAIL_POINTS	8192	// for all trails in a frame
#define OAX_MAX_PRT_SURFS		(OAX_MAX_SCENE_FX * 2)

typedef struct {
	surfaceType_t	surfaceType;	// SF_OAX_PARTICLES
	int				fx;				// index into the frame's fx list
	int				stage;
} srfOaxParticles_t;

typedef struct {
	oaxFx_t			fx;
	oaxPrtDecl_t	*decl;
	int				firstSurf, numSurfs;
	int				timeMs;			// the system's clock at this frame (shader or refdef clock)
	vec3_t			bounds[2];
} oaxSceneFx_t;

typedef struct {
	surfaceType_t	surfaceType;	// SF_OAX_TRAIL
	oaxTrail_t		t;
	int				firstPoint;		// into the frame's point list
	vec3_t			bounds[2];
} srfOaxTrail_t;

// ---- decals (tr_oax_decal.c) ----

#define OAX_MAX_DECALS			256
#define OAX_DECAL_MAX_VERTS		192
#define OAX_DECAL_MAX_POLYS		48

typedef struct {
	surfaceType_t	surfaceType;	// SF_OAX_DECAL
	int				index;
} srfOaxDecal_t;

#endif
