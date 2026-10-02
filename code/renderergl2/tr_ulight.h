/*
===========================================================================
tr_ulight.h: unified lighting (phase 5), shared by the frontend
(tr_ulight.c), the backend (tb_ulight.c) and the material extensions
(tr_matexpr.c).

A map selects its lighting model with the worldspawn key `oax_lighting`:
  lightmap  (default) the stock renderer path, untouched;
  unified   every light entity is a realtime per-pixel light with shadows,
            the world has no lightmaps and gets `oax_ambient` as its floor;
  hybrid    lightmapped base, plus `rtlight` entities as realtime lights.

Light definitions follow id Tech 4's renderLight_t: a light is a projection
(four planes S, T, Q and a falloff plane) plus a projection image and a
falloff image from its light shader. Point lights are the special case
whose projection is a box around the origin.
===========================================================================
*/
#ifndef TR_ULIGHT_H
#define TR_ULIGHT_H

#define ULIGHT_LIGHTMAP 0
#define ULIGHT_UNIFIED  1
#define ULIGHT_HYBRID   2

#define ULIGHT_SHADOW_AUTO    0
#define ULIGHT_SHADOW_MAPS    1
#define ULIGHT_SHADOW_STENCIL 2

#define MAX_ULIGHTS           4096
#define MAX_VIEW_ULIGHTS      256
#define MAX_ULIGHT_VIEWS      16
#define MAX_ULIGHT_AREAS      16

// light shader flags (light materials, tr_matexpr.c)
#define ULSF_AMBIENT        0x01
#define ULSF_FOG            0x02
#define ULSF_BLEND          0x04
#define ULSF_NOSHADOWS      0x08
#define ULSF_NOSELFSHADOW   0x10
#define ULSF_FORCESHADOWS   0x20
#define ULSF_LIGHTSHADER    0x40   // the material had a light-only keyword

// ---- material expressions (tr_matexpr.c, port of id Tech 4 Material.cpp) ----

#define MATEXPR_MAX_REGISTERS 128
#define MATEXPR_MAX_OPS       64

enum {
	MEXP_REG_TIME,
	MEXP_REG_PARM0, MEXP_REG_PARM1, MEXP_REG_PARM2, MEXP_REG_PARM3,
	MEXP_REG_PARM4, MEXP_REG_PARM5, MEXP_REG_PARM6, MEXP_REG_PARM7,
	MEXP_REG_PARM8, MEXP_REG_PARM9, MEXP_REG_PARM10, MEXP_REG_PARM11,
	MEXP_REG_GLOBAL0, MEXP_REG_GLOBAL1, MEXP_REG_GLOBAL2, MEXP_REG_GLOBAL3,
	MEXP_REG_GLOBAL4, MEXP_REG_GLOBAL5, MEXP_REG_GLOBAL6, MEXP_REG_GLOBAL7,
	MEXP_REG_NUM_PREDEFINED
};

typedef struct {
	int opType;
	int a, b, c;
} matExprOp_t;

typedef struct matExpr_s {
	int         numRegisters;
	float       constRegisters[MATEXPR_MAX_REGISTERS];
	qboolean    registerIsTemporary[MATEXPR_MAX_REGISTERS];
	int         numOps;
	matExprOp_t ops[MATEXPR_MAX_OPS];
	qboolean    constant;       // no time / parm / table terms
	int         lastFrame;      // frame the per-frame color was evaluated
} matExpr_t;

void        R_MatExprInit( void );
qboolean    R_MatExprStageShorthand( const char *token, char **text, char *out, int outSize );
qboolean    R_MatExprParseShaderKeyword( const char *token, char **text, shader_t *sh );
qboolean    R_MatExprParseStageKeyword( const char *token, char **text, shader_t *sh, shaderStage_t *stage );
void        R_MatExprEvaluate( const matExpr_t *expr, float *registers, const float *parms, float time );
float       R_TableLookup( int table, float index );
int         R_FindTable( const char *name );
void        R_MatExprUpdateFrame( void );
void        R_StageExprColor( shader_t *sh, int stageNum, const float *parms, float time, vec4_t out );

// ---- light definitions ----

typedef struct {
	// parms (renderLight_t)
	vec3_t      origin;
	vec3_t      axis[3];
	vec3_t      lightRadius;
	vec3_t      lightCenter;
	vec3_t      target, right, up, start, end;
	qboolean    pointLight;
	qboolean    parallel;
	qboolean    noShadows;
	qboolean    noSpecular;
	float       shaderParms[12];
	shader_t   *shader;
} uLightParms_t;

typedef struct uLight_s {
	uLightParms_t parms;
	uLightParms_t baseParms;    // as loaded from the map, for game updates

	int         entityNum;      // ordinal in the entity lump (-1 for scene dlights)
	char        targetname[64];
	qboolean    gameControlled; // has a targetname or bind: the game may update it
	qboolean    rtlight;        // classname rtlight
	qboolean    on;
	qboolean    dynamic;        // a scene dlight (rockets, muzzle flashes)
	int         lastUpdateFrame;

	// derived (R_DeriveULightData)
	vec4_t      lightProject[4];    // S, T, Q, falloff planes in world space
	vec4_t      frustum[6];         // planes facing out of the light volume
	vec3_t      frustumVerts[32];
	int         numFrustumVerts;
	vec3_t      bounds[2];
	vec3_t      globalLightOrigin;
	image_t    *projImage;
	image_t    *falloffImage;
	int         lightFlags;         // ULSF_*

	// world interactions (cached at load / when the light moves)
	int        *worldSurfs;         // world surface indexes in the light volume
	int         numWorldSurfs;
	int         maxWorldSurfs;
	int         areas[MAX_ULIGHT_AREAS];
	int         numAreas;           // -1: unknown (outside the world): always visible
	int         derivedCount;       // bumps when the light changes; invalidates caches

	// shadows
	int         shadowSlot;         // static shadow cache slot, -1 none
	int         shadowSlotStamp;    // derivedCount the slot was rendered for
	void       *stencilWorld;       // cached world brush shadow volumes (stencil mode)
	int         stencilWorldStamp;
} uLight_t;

// one visible light in one view
typedef struct {
	uLight_t   *light;
	int         index;              // light index (into the map's lights)
	int         scissor[4];         // x, y, w, h in window coordinates
	vec3_t      color;              // light color after the shader expressions
	int         firstLit, numLit;   // into the frame's surface list
	int         firstCaster, numCaster;
	qboolean    shadows;
	qboolean    dynamicCasters;     // entities inside the volume: overlay them
	int         shadowSize;
} uViewLight_t;

typedef struct {
	int         numLights;
	uViewLight_t lights[MAX_VIEW_ULIGHTS];
	int         firstAmbient, numAmbient;   // opaque surfaces for the ambient pass
	vec3_t      ambient;
	int         lightingModel;
	int         shadowMode;
} uView_t;

typedef struct {
	int         lightingModel;
	int         shadowMode;
	vec3_t      ambient;
	int         numLights;
	uLight_t   *lights;
	int         numMapLights;       // lights from the map (scene dlights follow)
	qboolean    loaded;
	// stats published as debug values
	int         statVisible;
	int         statDraws;
	int         statShadowPasses;
	int         statShadowCacheHits;
	int         statStencilTris;
	char        statVisibleIds[256];
} uLightWorld_t;

extern uLightWorld_t ulw;

// backend modes for RB_StageIteratorGeneric
#define ULB_NONE        0
#define ULB_AMBIENT     1
#define ULB_INTERACTION 2
#define ULB_DEPTH       3

typedef struct {
	int             mode;
	uViewLight_t   *vl;
	uView_t        *view;
	int             shadowType;     // 0 none, 1 cube, 2 2D, 3 stencil
	float           shadowMatrix[16];
	vec4_t          shadowParams;   // near, far, bias, texel
	qboolean        skipLitStages;  // main pass in unified mode
} uLightBackend_t;

extern uLightBackend_t ulb;

// cvars
extern cvar_t *r_ulight;
extern cvar_t *r_ulightShadows;
extern cvar_t *r_ulightShadowMode;
extern cvar_t *r_ulightScissor;
extern cvar_t *r_ulightAreaCull;
extern cvar_t *r_ulightSpecular;
extern cvar_t *r_ulightShadowBias;
extern cvar_t *r_ulightDebug;
extern cvar_t *r_dlightShadows;
extern cvar_t *r_shadowMapSizeU;

// the main depth buffer format: depth + stencil when r_ulightStencil is set
int     R_ULightDepthFormat( void );
qboolean R_ULightHasStencil( void );

// frontend (tr_ulight.c)
void    R_ULightInit( void );
void    R_ULightShutdown( void );
void    R_ULightLoadWorld( const void *header );
void    R_ULightBeginScene( void );
int     R_ULightAddView( int firstDrawSurf, int numDrawSurfs );
uView_t *R_ULightGetView( int index );
drawSurf_t *R_ULightSurfList( int first );
void    R_ULightUpdateDef( int index, const vec3_t origin, const vec3_t axis[3], const vec3_t rgb, const float *parms, int flags );
int     R_ULightLightingModel( void );
qboolean R_ULightStageIsInteraction( shader_t *sh, shaderStage_t *stage );
int     R_ULightInteractionStage( shader_t *sh );
void    R_ULightPublishStats( int frontEndMsec, int backEndMsec );
void    RE_OAXUpdateLight( int index, const float *origin, const float *axis, const float *rgb, const float *parms, int flags );
void    R_ULightFrameFinished( void );

// backend (tb_ulight.c)
void    RB_ULightInit( void );
void    RB_ULightShutdown( void );
void    RB_DrawULights( void );
void    RB_ULightDrawViewSurfs( drawSurf_t *drawSurfs, int numDrawSurfs );
qboolean RB_ULightStageIterator( shaderCommands_t *input );
qboolean RB_ULightSkipStage( shaderCommands_t *input, int stage );

// stencil shadow volumes (tb_ulight_stencil.c)
void    R_ULightStencilLoadWorld( const void *header );
void    RB_ULightStencilShadows( uViewLight_t *vl );
void    R_ULightStencilFreeWorld( void );
void    R_ULightStencilBuildStatic( void );

// generated light images (tr_ulight.c, port of id Tech 4 Image_init.cpp)
extern image_t *ulightImages_quadratic;
extern image_t *ulightImages_noFalloff;
extern image_t *ulightImages_pointLight;
extern image_t *ulightImages_spotLight;
extern image_t *ulightImages_flat;
extern image_t *ulightImages_white;
extern image_t *ulightImages_black;

// helpers shared with tr_shade.c
void ComputeTexMods( shaderStage_t *pStage, int bundleNum, vec4_t outMatrix[8] );
void ComputeDeformValues( int *deformGen, vec5_t deformParams );
void R_BindAnimatedImageToTMU( textureBundle_t *bundle, int tmu );
void GLSL_DeleteGPUShader( shaderProgram_t *program );
void RB_RenderDrawSurfList( drawSurf_t *drawSurfs, int numDrawSurfs );
struct FBO_s *FBO_Create( const char *name, int width, int height );
void FBO_CreateBuffer( struct FBO_s *fbo, int format, int index, int multisample );
qboolean R_CheckFBO( const struct FBO_s *fbo );
int  GLSL_InitOAXShader( shaderProgram_t *program, const char *name, int attribs, const char *extra, const char *vp, const char *fp );

#endif
