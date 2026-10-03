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
tr_ulight_phys.c: the physical light description (step 7.5 B), written for
this engine. See docs/lights.md for every key.

A light is described by measurable properties instead of a projection and a
falloff image:
  radius     where the light ends (a sphere)
  intensity  the scalar peak: 1.0 lights a texture at 1x where falloff is 1
  color      a multiplier per channel, NOT normalised
  falloff    a curve over x = d / radius: piecewise-linear points, an image,
             inverse square with a clamp distance, or 1 - smoothstep(x)
  cap        a soft ceiling on intensity * falloff (a quadratic knee)
  angular    Lambert (N.L) or none
  mask       light-mask groups: the light lights a surface when they share one
  effect     a time multiplier from an expression table
The interaction shader evaluates all of it per pixel (interaction_fp.glsl,
ULIGHT_PHYSICAL); the effect is evaluated per frame here.

Profiles translate a source engine's own keys:
  ue1    UE1 Light actor properties, `ue1_<Property>`, with the falloff
         and gain measured, in linear space, by baking UE1 test maps
         (misc/tools/ue1-light-calib.mjs; tests/romdev/reference/ue1-light-calib.json)
  q3     q3map2's point light formula (pointscale 7500, linearscale 1/8000)
  doom3  id Tech 4 conventions: light_radius, _color unnormalised, a
         quadratic falloff, specular
  physical  the keys above, nothing translated
Any oax_* key overrides what a profile derived.

Zones: func_oax_zone `ambient "r g b"` replaces the world ambient for world
vertices inside the zone (unified maps), and for entities whose origin is
inside it.
===========================================================================
*/

#include "tr_local.h"

// ---- UE1, measured in linear space (docs/lights.md) ----
//
// UE1 shots with the display chain neutral (Brightness 0.5, GammaOffset
// 0), six lamps from LightRadius 12 to 255 (325 to 6400 units): every lamp
// is light = gain * LightBrightness * (1 - smoothstep(d / R)) * N.L with R
// = WorldLightRadius() and no ceiling (rms 1.3 grey over 433 calibration
// cells, misc/tools/ue1-light-calib.mjs). The line with a ceiling fitted
// before was the renderer's display curve, not UE1's lighting.
//
// light (1.0 = the texture at 1x) per unit of LightBrightness
#define UE1_GAIN        0.0123f

// q3map2 (light.c, q3map2.h)
#define Q3_POINTSCALE   7500.0f
#define Q3_LINEARSCALE  ( 1.0f / 8000.0f )
#define Q3_MINDIST      16.0f

static const char *physKeys[] = {
	"oax_profile", "oax_radius", "oax_intensity", "oax_color", "oax_falloff", "oax_cap",
	"oax_capKnee", "oax_angular", "oax_mask", "oax_effect", "oax_specular", NULL
};

qboolean R_ULightPhysHasKeys( const spawnArgs_t *a ) {
	int i;

	for ( i = 0; i < a->numKeys; i++ ) {
		int k;

		if ( !Q_stricmpn( a->keys[i], "ue1_", 4 ) ) {
			return qtrue;
		}
		for ( k = 0; physKeys[k]; k++ ) {
			if ( !Q_stricmp( a->keys[i], physKeys[k] ) ) {
				return qtrue;
			}
		}
	}
	return qfalse;
}

static float KeyFloat( const spawnArgs_t *a, const char *key, float def ) {
	const char *v = R_ULightArg( a, key );

	return v && v[0] ? atof( v ) : def;
}

static qboolean KeyVec( const spawnArgs_t *a, const char *key, vec3_t out ) {
	const char *v = R_ULightArg( a, key );

	if ( !v || !v[0] ) {
		return qfalse;
	}
	VectorClear( out );
	sscanf( v, "%f %f %f", &out[0], &out[1], &out[2] );
	return qtrue;
}

// UE1 editor booleans: True/False, or numbers
static qboolean KeyBool( const spawnArgs_t *a, const char *key ) {
	const char *v = R_ULightArg( a, key );

	if ( !v || !v[0] ) {
		return qfalse;
	}
	if ( !Q_stricmp( v, "true" ) ) {
		return qtrue;
	}
	return atoi( v ) != 0;
}

// an enum by name (LT_Pulse) or number
static int KeyEnum( const spawnArgs_t *a, const char *key, const char *const *names, int def ) {
	const char *v = R_ULightArg( a, key );
	int i;

	if ( !v || !v[0] ) {
		return def;
	}
	if ( ( v[0] >= '0' && v[0] <= '9' ) || v[0] == '-' ) {
		return atoi( v );
	}
	for ( i = 0; names[i]; i++ ) {
		if ( !Q_stricmp( v, names[i] ) ) {
			return i;
		}
	}
	return def;
}

static const char *ue1LightTypes[] = {
	"LT_None", "LT_Steady", "LT_Pulse", "LT_Blink", "LT_Flicker", "LT_Strobe", "LT_BackdropLight",
	"LT_SubtlePulse", "LT_TexturePaletteOnce", "LT_TexturePaletteLoop", NULL
};

static const char *ue1LightEffects[] = {
	"LE_None", "LE_TorchWaver", "LE_FireWaver", "LE_WateryShimmer", "LE_Searchlight", "LE_SlowWave",
	"LE_FastWave", "LE_CloudCast", "LE_StaticSpot", "LE_Shock", "LE_Disco", "LE_Warp", "LE_Spotlight",
	"LE_NonIncidence", "LE_Shell", "LE_OmniBumpMap", "LE_Interference", "LE_Cylinder", "LE_Rotor",
	"LE_Unused", NULL
};

/*
=================
R_UE1Color

FGetHSV's hue and saturation, before normalising: the hue wheel is linear
(red 0, green 85, blue 170) and saturation blends toward white (255 is
white). Hue 0 is RED, so a lamp that only sets LightSaturation=0 is
saturated red. The magnitude is kept: a pure hue peaks at 1.0, a
common lamp yellow (hue 32, saturation 96) at 0.77.
=================
*/
static void R_UE1Color( float h, float saturation, vec3_t out ) {
	float s = saturation / 255.0f;
	int i;

	if ( h < 86 ) {
		VectorSet( out, ( 85 - h ) / 85.0f, h / 85.0f, 0 );
	} else if ( h < 171 ) {
		VectorSet( out, 0, ( 170 - h ) / 85.0f, ( h - 85 ) / 85.0f );
	} else {
		VectorSet( out, ( h - 170 ) / 85.0f, 0, ( 255 - h ) / 84.0f );
	}
	for ( i = 0; i < 3; i++ ) {
		out[i] = out[i] + s * ( 1.0f - out[i] );
	}
}

static void SetLine( uLightPhys_t *ph, float zero ) {
	ph->falloffMode = ULF_TABLE;
	ph->numPoints = 2;
	ph->points[0][0] = 0;
	ph->points[0][1] = 1;
	ph->points[1][0] = zero;
	ph->points[1][1] = 0;
}

/*
=================
ProfileUE1

The ue1_* keys are the Light actor's properties as the UE1 editor stores them.
=================
*/
static void ProfileUE1( const spawnArgs_t *a, uLightParms_t *p ) {
	uLightPhys_t *ph = &p->phys;
	float brightness = KeyFloat( a, "ue1_LightBrightness", 64 );
	float radius = KeyFloat( a, "ue1_LightRadius", 64 );
	float period = KeyFloat( a, "ue1_LightPeriod", 32 );
	float phase = KeyFloat( a, "ue1_LightPhase", 0 );
	int type = KeyEnum( a, "ue1_LightType", ue1LightTypes, 1 );
	int effect = KeyEnum( a, "ue1_LightEffect", ue1LightEffects, 0 );

	// Actor::WorldLightRadius() = 25 * (LightRadius + 1) (469 SDK AActor.h)
	ph->radius = 25.0f * ( (int)radius + 1 );
	ph->falloffMode = ULF_SMOOTH;
	ph->numPoints = 0;
	// LevelInfo.Brightness (worldspawn ue1_LevelBrightness): a plain gain in
	// linear space (1.50x and 2.00x measured at 1.5 and 2)
	ph->intensity = UE1_GAIN * brightness * ulw.ue1LevelBrightness;
	ph->cap = 0;
	ph->capKnee = 0;
	R_UE1Color( KeyFloat( a, "ue1_LightHue", 0 ), KeyFloat( a, "ue1_LightSaturation", 255 ), ph->color );
	ph->lambert = qtrue;
	ph->mask = KeyBool( a, "ue1_bSpecialLit" ) ? ULIGHT_MASK_SPECIALLIT : ULIGHT_MASK_DEFAULT;
	p->noSpecular = qtrue;       // lightmaps: no specular term
	if ( brightness <= 0 || type == 0 ) {
		ph->startOff = qtrue;    // LT_None, or brightness 0: switched off in the original
	}

	// LightType: the time functions (UE1's light manager, 35 ticks a second;
	// recalled, not measured: see docs/lights.md)
	switch ( type ) {
	case 2:     // LT_Pulse
	case 7:     // LT_SubtlePulse
		ph->effectTable = R_FindTable( "oax_sin" );
		ph->effectRate = 35.0f / MAX( period, 1.0f );
		ph->effectPhase = phase / 256.0f;
		ph->effectBase = type == 2 ? 0.6f : 0.9f;
		ph->effectAmp = type == 2 ? 0.39f : 0.09f;
		break;
	case 3:     // LT_Blink: dark for the second half of each period
		ph->effectTable = R_FindTable( "oax_blink" );
		ph->effectRate = 35.0f / ( period + 1.0f );
		ph->effectPhase = phase / 256.0f;
		break;
	case 4:     // LT_Flicker: a new random level each tick
		ph->effectTable = R_FindTable( "oax_flicker" );
		ph->effectRate = 35.0f / 64.0f;
		break;
	case 5:     // LT_Strobe
		ph->effectTable = R_FindTable( "oax_strobe" );
		ph->effectRate = 35.0f / 64.0f;
		break;
	case 0: case 1:
		break;
	default:    // LT_BackdropLight, LT_TexturePalette*: lit steady
		ph->unsupported++;
		break;
	}

	// LightEffect: only the angular one has a translation; the rest are
	// spatial patterns of UE1's software renderer
	if ( effect == 13 ) {           // LE_NonIncidence
		ph->lambert = qfalse;
	} else if ( effect != 0 ) {
		ph->unsupported++;
	}
}

/*
=================
ProfileQ3

q3map2's point light (light.c): photons = light * _scale * 7500;
inverse square: add = photons * N.L / max(d, 16)^2, ending where it falls to
1 (the envelope, sqrt(photons)); linear (spawnflags 1): add = photons / 8000
- max(d, 16) * fade, with no angle term in Q3 mode. Lightmap units: 255 is
the texture at 1x. Colors are normalised (ColorNormalize).
=================
*/
static void ProfileQ3( const spawnArgs_t *a, uLightParms_t *p ) {
	uLightPhys_t *ph = &p->phys;
	float intensity = KeyFloat( a, "_light", 0 );
	float scale = KeyFloat( a, "_scale", 1 );
	int spawnflags = (int)KeyFloat( a, "spawnflags", 0 );
	float photons, m;

	if ( intensity == 0 ) {
		intensity = KeyFloat( a, "light", 0 );
	}
	if ( intensity == 0 ) {
		intensity = 300;
	}
	if ( scale == 0 ) {
		scale = 1;
	}
	photons = intensity * scale * Q3_POINTSCALE;
	ph->lambert = !( spawnflags & 2 );
	if ( spawnflags & 1 ) {
		float fade = KeyFloat( a, "fade", 1 );

		if ( fade == 0 ) {
			fade = 1;
		}
		ph->radius = MAX( photons * Q3_LINEARSCALE / fade, 1.0f );
		ph->intensity = photons * Q3_LINEARSCALE / 255.0f;
		m = MIN( Q3_MINDIST / ph->radius, 1.0f );
		ph->falloffMode = ULF_TABLE;
		ph->numPoints = 3;
		ph->points[0][0] = 0;
		ph->points[0][1] = 1 - m;
		ph->points[1][0] = m;
		ph->points[1][1] = 1 - m;
		ph->points[2][0] = 1;
		ph->points[2][1] = 0;
		ph->lambert = qfalse;
	} else {
		ph->radius = MAX( sqrt( photons ), Q3_MINDIST * 2 );
		ph->falloffMode = ULF_INVSQ;
		ph->invsqMin = Q3_MINDIST / ph->radius;
		ph->intensity = photons / ( Q3_MINDIST * Q3_MINDIST ) / 255.0f;
	}
	// ColorNormalize: the brightest channel becomes 1
	{
		float mx = MAX( p->shaderParms[0], MAX( p->shaderParms[1], p->shaderParms[2] ) );

		VectorSet( ph->color, 1, 1, 1 );
		if ( mx > 0 ) {
			VectorScale( ph->color, 1.0f / mx, ph->color );
		}
	}
	p->noSpecular = qtrue;
}

/*
=================
ProfileDoom3

light_radius (the largest axis) or `light`; _color as given; the radial
counterpart of this engine's default point light, (1 - d/R)^2; Lambert and
specular as id Tech 4's interaction.
=================
*/
static void ProfileDoom3( const spawnArgs_t *a, uLightParms_t *p ) {
	uLightPhys_t *ph = &p->phys;

	ph->radius = MAX( p->lightRadius[0], MAX( p->lightRadius[1], p->lightRadius[2] ) );
	ph->falloffMode = ULF_TABLE;
	{
		int i;

		// (1 - x)^2 as 16 points
		ph->numPoints = ULIGHT_MAX_FALLOFF_POINTS;
		for ( i = 0; i < ULIGHT_MAX_FALLOFF_POINTS; i++ ) {
			float x = i / (float)( ULIGHT_MAX_FALLOFF_POINTS - 1 );

			ph->points[i][0] = x;
			ph->points[i][1] = ( 1 - x ) * ( 1 - x );
		}
	}
	ph->intensity = 1;
	VectorSet( ph->color, 1, 1, 1 );
	ph->lambert = qtrue;
}

static void ProfilePhysical( const spawnArgs_t *a, uLightParms_t *p ) {
	uLightPhys_t *ph = &p->phys;

	ph->radius = MAX( p->lightRadius[0], MAX( p->lightRadius[1], p->lightRadius[2] ) );
	SetLine( ph, 1 );
	ph->intensity = 1;
	VectorSet( ph->color, 1, 1, 1 );
	ph->lambert = qtrue;
}

/*
=================
ParseFalloff

oax_falloff: linear | quadratic | smooth | invsq <clamp units> | table x y x y ... |
image <path>
=================
*/
static void ParseFalloff( const char *v, uLightPhys_t *ph, const char *where ) {
	char buf[256];
	char *s = buf, *t;

	Q_strncpyz( buf, v, sizeof( buf ) );
	t = COM_ParseExt( &s, qfalse );
	if ( !Q_stricmp( t, "linear" ) ) {
		SetLine( ph, 1 );
	} else if ( !Q_stricmp( t, "quadratic" ) ) {
		int i;

		ph->falloffMode = ULF_TABLE;
		ph->numPoints = ULIGHT_MAX_FALLOFF_POINTS;
		for ( i = 0; i < ULIGHT_MAX_FALLOFF_POINTS; i++ ) {
			float x = i / (float)( ULIGHT_MAX_FALLOFF_POINTS - 1 );

			ph->points[i][0] = x;
			ph->points[i][1] = ( 1 - x ) * ( 1 - x );
		}
	} else if ( !Q_stricmp( t, "smooth" ) ) {
		ph->falloffMode = ULF_SMOOTH;
	} else if ( !Q_stricmp( t, "invsq" ) ) {
		float m = atof( COM_ParseExt( &s, qfalse ) );

		ph->falloffMode = ULF_INVSQ;
		ph->invsqMin = m > 0 && ph->radius > 0 ? m / ph->radius : 1.0f / 64;
	} else if ( !Q_stricmp( t, "table" ) ) {
		int n = 0;

		while ( n < ULIGHT_MAX_FALLOFF_POINTS ) {
			char *x = COM_ParseExt( &s, qfalse ), *y;

			if ( !x[0] ) {
				break;
			}
			ph->points[n][0] = atof( x );
			y = COM_ParseExt( &s, qfalse );
			if ( !y[0] ) {
				break;
			}
			ph->points[n][1] = atof( y );
			if ( n && ph->points[n][0] < ph->points[n - 1][0] ) {
				ri.Printf( PRINT_WARNING, "%s: oax_falloff table x values must not decrease\n", where );
				ph->points[n][0] = ph->points[n - 1][0];
			}
			n++;
		}
		if ( n ) {
			ph->falloffMode = ULF_TABLE;
			ph->numPoints = n;
		} else {
			ri.Printf( PRINT_WARNING, "%s: empty oax_falloff table, keeping the profile's\n", where );
		}
	} else if ( !Q_stricmp( t, "image" ) ) {
		t = COM_ParseExt( &s, qfalse );
		ph->falloffImage = R_FindImageFile( t, IMGTYPE_COLORALPHA, IMGFLAG_CLAMPTOEDGE | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE );
		if ( ph->falloffImage ) {
			ph->falloffMode = ULF_IMAGE;
		} else {
			ri.Printf( PRINT_WARNING, "%s: oax_falloff image '%s' not found\n", where, t );
		}
	} else {
		ri.Printf( PRINT_WARNING, "%s: unknown oax_falloff '%s'\n", where, t );
	}
}

/*
=================
R_ULightPhysParse

Called at the end of ParseLight for a light with physical keys: p already
holds the origin, axis, _color (shaderParms 0-2) and light_radius / light.
=================
*/
void R_ULightPhysParse( const spawnArgs_t *a, uLightParms_t *p ) {
	uLightPhys_t *ph = &p->phys;
	const char *v;
	char where[96];
	vec3_t c;

	Com_sprintf( where, sizeof( where ), "light at (%d %d %d)", (int)p->origin[0], (int)p->origin[1], (int)p->origin[2] );
	Com_Memset( ph, 0, sizeof( *ph ) );
	ph->physical = qtrue;
	ph->effectTable = -1;
	ph->effectBase = 0;
	ph->effectAmp = 1;
	ph->mask = ULIGHT_MASK_DEFAULT;

	v = R_ULightArg( a, "oax_profile" );
	if ( v && !Q_stricmp( v, "ue1" ) ) {
		ph->profile = ULP_UE1;
	} else if ( v && !Q_stricmp( v, "q3" ) ) {
		ph->profile = ULP_Q3;
	} else if ( v && !Q_stricmp( v, "doom3" ) ) {
		ph->profile = ULP_DOOM3;
	} else if ( v && Q_stricmp( v, "physical" ) ) {
		ri.Printf( PRINT_WARNING, "%s: unknown oax_profile '%s', using physical\n", where, v );
		ph->profile = ULP_PHYSICAL;
	} else if ( !v ) {
		int i;

		ph->profile = ULP_PHYSICAL;
		for ( i = 0; i < a->numKeys; i++ ) {
			if ( !Q_stricmpn( a->keys[i], "ue1_", 4 ) ) {
				ph->profile = ULP_UE1;
			}
		}
	}

	switch ( ph->profile ) {
	case ULP_UE1:   ProfileUE1( a, p ); break;
	case ULP_Q3:    ProfileQ3( a, p ); break;
	case ULP_DOOM3: ProfileDoom3( a, p ); break;
	default:        ProfilePhysical( a, p ); break;
	}

	// overrides, in this order (a falloff relative to the radius needs it)
	ph->radius = KeyFloat( a, "oax_radius", ph->radius );
	if ( ph->radius < 1 ) {
		ph->radius = 1;
	}
	if ( ( v = R_ULightArg( a, "oax_falloff" ) ) != NULL && v[0] ) {
		ParseFalloff( v, ph, where );
	}
	ph->intensity = KeyFloat( a, "oax_intensity", ph->intensity );
	if ( KeyVec( a, "oax_color", c ) ) {
		VectorCopy( c, ph->color );
	}
	ph->cap = KeyFloat( a, "oax_cap", ph->cap );
	ph->capKnee = KeyFloat( a, "oax_capKnee", ph->capKnee );
	if ( ( v = R_ULightArg( a, "oax_angular" ) ) != NULL && v[0] ) {
		ph->lambert = Q_stricmp( v, "none" ) != 0;
	}
	if ( ( v = R_ULightArg( a, "oax_mask" ) ) != NULL && v[0] ) {
		ph->mask = (int)strtol( v, NULL, 0 ) & ULIGHT_MASK_ALL;
	}
	if ( ( v = R_ULightArg( a, "oax_specular" ) ) != NULL && v[0] ) {
		p->noSpecular = atoi( v ) == 0;
	}
	if ( ( v = R_ULightArg( a, "oax_effect" ) ) != NULL && v[0] ) {
		char buf[256], *s = buf, *t;

		Q_strncpyz( buf, v, sizeof( buf ) );
		t = COM_ParseExt( &s, qfalse );
		if ( !Q_stricmp( t, "none" ) ) {
			ph->effectTable = -1;
		} else {
			ph->effectTable = R_FindTable( t );
			if ( ph->effectTable < 0 ) {
				ri.Printf( PRINT_WARNING, "%s: oax_effect table '%s' not found\n", where, t );
			}
			t = COM_ParseExt( &s, qfalse );
			ph->effectRate = t[0] ? atof( t ) : 1;
			t = COM_ParseExt( &s, qfalse );
			ph->effectPhase = t[0] ? atof( t ) : 0;
			t = COM_ParseExt( &s, qfalse );
			ph->effectBase = t[0] ? atof( t ) : 0;
			t = COM_ParseExt( &s, qfalse );
			ph->effectAmp = t[0] ? atof( t ) : 1;
		}
	}
	if ( ph->cap < 0 ) {
		ph->cap = 0;
	}
	if ( ph->capKnee < 0 ) {
		ph->capKnee = 0;
	}
	if ( ph->falloffMode == ULF_INVSQ && ph->invsqMin <= 0 ) {
		ph->invsqMin = 1.0f / 64;
	}

	// a sphere: the light volume is the box around it
	p->pointLight = qtrue;
	p->parallel = qfalse;
	VectorClear( p->lightCenter );
	VectorSet( p->lightRadius, ph->radius, ph->radius, ph->radius );
}

/*
=================
R_ULightPhysEffect

The effect's multiplier at a time (seconds); 1 without an effect.
=================
*/
float R_ULightPhysEffect( const uLightPhys_t *ph, float time ) {
	if ( !ph->physical || ph->effectTable < 0 ) {
		return 1.0f;
	}
	return ph->effectBase + ph->effectAmp * R_TableLookup( ph->effectTable, time * ph->effectRate + ph->effectPhase );
}

/*
=================
R_ULightPhysWorldspawn

oax_overbright: the per-light ceiling of physical lights, 1 (off, a light
reaches the texture at 1x) to 2 (UE1-style lightmaps at 2x).
=================
*/
void R_ULightPhysWorldspawn( const spawnArgs_t *world ) {
	ulw.ue1LevelBrightness = KeyFloat( world, "ue1_LevelBrightness", 1 );
	if ( ulw.ue1LevelBrightness <= 0 ) {
		ri.Printf( PRINT_WARNING, "ue1_LevelBrightness %g: must be above 0, using 1\n", ulw.ue1LevelBrightness );
		ulw.ue1LevelBrightness = 1;
	}
	ulw.overbright = KeyFloat( world, "oax_overbright", 1 );
	if ( ulw.overbright < 1 ) {
		ulw.overbright = 1;
	} else if ( ulw.overbright > 2 ) {
		ulw.overbright = 2;
	}
}

/*
=====================================================================

ZONE AMBIENT (func_oax_zone `ambient`)

=====================================================================
*/

// no fixed limits: the arrays are sized from the BSP at load (a large
// imported map's zones have over 1100 brushes); anything that cannot be read is a warning and
// counts in the debug value r_ulight_zones, never silently dropped

typedef struct {
	int     firstBrush, numBrushes;     // into zoneBrushes
	vec3_t  ambient;
	int     priority;
	int     entityNum;
	vec3_t  mins, maxs;
} zoneAmbient_t;

typedef struct {
	int     firstPlane, numPlanes;      // into zonePlanes
} zoneBrush_t;

static zoneAmbient_t *zones;
static int           numZones;
static zoneBrush_t  *zoneBrushes;
static int           numZoneBrushes;
static vec4_t       *zonePlanes;
static int           numZonePlanes;
static int           zoneErrors;        // zones, brushes or sides that could not be read

void R_ULightZonesFree( void ) {
	numZones = numZoneBrushes = numZonePlanes = zoneErrors = 0;
	if ( zones ) {
		ri.Free( zones );
		zones = NULL;
	}
	if ( zoneBrushes ) {
		ri.Free( zoneBrushes );
		zoneBrushes = NULL;
	}
	if ( zonePlanes ) {
		ri.Free( zonePlanes );
		zonePlanes = NULL;
	}
	if ( ulw.surfMask ) {
		ri.Free( ulw.surfMask );
		ulw.surfMask = NULL;
	}
}

// the zone whose brushes contain the point; highest priority, then the
// lowest entity number (bg_oax_zone.c's rule); -1 none
static int ZoneAt( const vec3_t p ) {
	int best = -1, i, b, k;

	for ( i = 0; i < numZones; i++ ) {
		const zoneAmbient_t *z = &zones[i];
		qboolean inside = qfalse;

		if ( p[0] < z->mins[0] || p[1] < z->mins[1] || p[2] < z->mins[2]
			|| p[0] > z->maxs[0] || p[1] > z->maxs[1] || p[2] > z->maxs[2] ) {
			continue;
		}
		for ( b = 0; b < z->numBrushes && !inside; b++ ) {
			const zoneBrush_t *br = &zoneBrushes[z->firstBrush + b];

			inside = qtrue;
			for ( k = 0; k < br->numPlanes; k++ ) {
				const float *pl = zonePlanes[br->firstPlane + k];

				if ( DotProduct( p, pl ) - pl[3] > 0.01f ) {
					inside = qfalse;
					break;
				}
			}
		}
		if ( !inside ) {
			continue;
		}
		if ( best < 0 || z->priority > zones[best].priority
			|| ( z->priority == zones[best].priority && z->entityNum < zones[best].entityNum ) ) {
			best = i;
		}
	}
	return best;
}

qboolean R_ULightZoneAmbientAt( const vec3_t point, vec3_t out ) {
	int z;

	if ( !numZones ) {
		return qfalse;
	}
	z = ZoneAt( point );
	if ( z < 0 ) {
		return qfalse;
	}
	VectorCopy( zones[z].ambient, out );
	return qtrue;
}

/*
=================
R_ULightZonesLoad

The func_oax_zone entities with an `ambient` key: their inline model's
brushes from the BSP, so the renderer tests points against the same solids
the game does. Zones that start off (spawnflag 1) are skipped.
=================
*/
static qboolean ZoneEntity( const spawnArgs_t *e, int numModels, int ordinal, int *modelIndex ) {
	const char *cn = R_ULightArg( e, "classname" );
	const char *model = R_ULightArg( e, "model" );
	int mi;

	if ( !cn || Q_stricmp( cn, "func_oax_zone" ) || !R_ULightArg( e, "ambient" ) ) {
		return qfalse;
	}
	if ( (int)KeyFloat( e, "spawnflags", 0 ) & 1 ) {
		return qfalse;
	}
	if ( !model || model[0] != '*' || ( mi = atoi( model + 1 ) ) <= 0 || mi >= numModels ) {
		ri.Printf( PRINT_WARNING, "func_oax_zone %d: no brush model, ambient ignored\n", ordinal );
		zoneErrors++;
		return qfalse;
	}
	*modelIndex = mi;
	return qtrue;
}

void R_ULightZonesLoad( const void *header, const spawnArgs_t *ents, int numEnts ) {
	const dheader_t *h = header;
	const byte *base = header;
	const dmodel_t *models;
	const dbrush_t *brushes;
	const dbrushside_t *sides;
	const dplane_t *planes;
	int numModels, numBrushes, numSides, numPlanes, i, mi, b;
	int wantZones = 0, wantBrushes = 0, wantPlanes = 0;

	// this map's zones only (the surface masks are not ours to free here)
	{
		int *keep = ulw.surfMask;

		ulw.surfMask = NULL;
		R_ULightZonesFree();
		ulw.surfMask = keep;
	}
	ulw.zoneAmbient = qfalse;
	ulw.numZoneAmbient = 0;
	if ( !header ) {
		return;
	}
	models = (const dmodel_t *)( base + LittleLong( h->lumps[LUMP_MODELS].fileofs ) );
	numModels = LittleLong( h->lumps[LUMP_MODELS].filelen ) / sizeof( dmodel_t );
	brushes = (const dbrush_t *)( base + LittleLong( h->lumps[LUMP_BRUSHES].fileofs ) );
	numBrushes = LittleLong( h->lumps[LUMP_BRUSHES].filelen ) / sizeof( dbrush_t );
	sides = (const dbrushside_t *)( base + LittleLong( h->lumps[LUMP_BRUSHSIDES].fileofs ) );
	numSides = LittleLong( h->lumps[LUMP_BRUSHSIDES].filelen ) / sizeof( dbrushside_t );
	planes = (const dplane_t *)( base + LittleLong( h->lumps[LUMP_PLANES].fileofs ) );
	numPlanes = LittleLong( h->lumps[LUMP_PLANES].filelen ) / sizeof( dplane_t );

	// pass 1: sizes
	for ( i = 1; i < numEnts; i++ ) {
		const dmodel_t *m;

		if ( !ZoneEntity( &ents[i], numModels, i, &mi ) ) {
			continue;
		}
		m = &models[mi];
		wantZones++;
		for ( b = 0; b < LittleLong( m->numBrushes ); b++ ) {
			int bi = LittleLong( m->firstBrush ) + b;

			if ( bi >= 0 && bi < numBrushes ) {
				wantBrushes++;
				wantPlanes += MAX( LittleLong( brushes[bi].numSides ), 0 );
			}
		}
	}
	zoneErrors = 0;     // pass 2 counts each problem once
	if ( !wantZones ) {
		return;
	}
	zones = ri.Malloc( sizeof( *zones ) * wantZones );
	zoneBrushes = ri.Malloc( sizeof( *zoneBrushes ) * MAX( wantBrushes, 1 ) );
	zonePlanes = ri.Malloc( sizeof( *zonePlanes ) * MAX( wantPlanes, 1 ) );

	// pass 2: fill
	for ( i = 1; i < numEnts; i++ ) {
		zoneAmbient_t *z;
		const dmodel_t *m;

		if ( !ZoneEntity( &ents[i], numModels, i, &mi ) ) {
			continue;
		}
		m = &models[mi];
		z = &zones[numZones];
		Com_Memset( z, 0, sizeof( *z ) );
		KeyVec( &ents[i], "ambient", z->ambient );
		z->priority = (int)KeyFloat( &ents[i], "priority", 0 );
		z->entityNum = i;
		z->firstBrush = numZoneBrushes;
		for ( b = 0; b < LittleLong( m->numBrushes ); b++ ) {
			int bi = LittleLong( m->firstBrush ) + b, s;
			zoneBrush_t *zb;

			if ( bi < 0 || bi >= numBrushes ) {
				ri.Printf( PRINT_WARNING, "func_oax_zone %d: brush %d out of range\n", i, bi );
				zoneErrors++;
				continue;
			}
			zb = &zoneBrushes[numZoneBrushes];
			zb->firstPlane = numZonePlanes;
			zb->numPlanes = 0;
			for ( s = 0; s < LittleLong( brushes[bi].numSides ); s++ ) {
				int si = LittleLong( brushes[bi].firstSide ) + s, pi = -1;
				float *out;

				if ( si >= 0 && si < numSides ) {
					pi = LittleLong( sides[si].planeNum );
				}
				if ( pi < 0 || pi >= numPlanes ) {
					// a missing side would make the brush bigger than it is
					ri.Printf( PRINT_WARNING, "func_oax_zone %d: brush %d side %d unreadable\n", i, bi, s );
					zoneErrors++;
					continue;
				}
				out = zonePlanes[numZonePlanes++];
				out[0] = LittleFloat( planes[pi].normal[0] );
				out[1] = LittleFloat( planes[pi].normal[1] );
				out[2] = LittleFloat( planes[pi].normal[2] );
				out[3] = LittleFloat( planes[pi].dist );
				zb->numPlanes++;
			}
			numZoneBrushes++;
			z->numBrushes++;
		}
		for ( b = 0; b < 3; b++ ) {
			z->mins[b] = LittleFloat( m->mins[b] ) - 1;
			z->maxs[b] = LittleFloat( m->maxs[b] ) + 1;
		}
		if ( z->numBrushes ) {
			numZones++;
		} else {
			ri.Printf( PRINT_WARNING, "func_oax_zone %d: no brushes, ambient ignored\n", i );
			zoneErrors++;
		}
	}
	ulw.numZoneAmbient = numZones;
	ulw.zoneAmbient = numZones > 0;
	if ( ri.DebugSet ) {
		ri.DebugSet( "r_ulight_zones", va( "%d %d %d %d", numZones, numZoneBrushes, numZonePlanes, zoneErrors ) );
	}
}

/*
=====================================================================

SURFACE LIGHT MASKS

=====================================================================
*/

void R_ULightSetSurfaceMask( int worldSurface, int mask ) {
	if ( !tr.world || worldSurface < 0 || worldSurface >= tr.world->numWorldSurfaces ) {
		return;
	}
	if ( !ulw.surfMask ) {
		ulw.surfMask = ri.Malloc( sizeof( int ) * tr.world->numWorldSurfaces );
		Com_Memset( ulw.surfMask, 0, sizeof( int ) * tr.world->numWorldSurfaces );
	}
	ulw.surfMask[worldSurface] = mask ? mask & ULIGHT_MASK_ALL : -1;
}

// a surface's light-mask groups: set by R_ULightSetSurfaceMask, else the
// surface world's per-surface mask (OAX_SURFACES lightMask), else its
// shader's oaxLightMask, else the default group
int R_ULightSurfaceMask( shader_t *sh, int worldSurface ) {
	int m = 0;
	qboolean inWorld = worldSurface >= 0 && tr.world && worldSurface < tr.world->numWorldSurfaces;

	if ( ulw.surfMask && inWorld ) {
		m = ulw.surfMask[worldSurface];
	}
	if ( !m && inWorld && tr.world->surfaces[worldSurface].oaxLightMask ) {
		m = (int)( tr.world->surfaces[worldSurface].oaxLightMask & ULIGHT_MASK_ALL );
	}
	if ( !m && sh ) {
		m = sh->oaxLightMask;
	}
	if ( !m ) {
		return ULIGHT_MASK_DEFAULT;
	}
	return m < 0 ? 0 : m;
}

/*
=================
R_ULightPhysPublish

Debug values for tests: `r_ulight_phys` "<count> <overbright> <zones>" and
`r_ulight_phys_lights`, one "ordinal:profile radius intensity cap knee
lambert mask r g b effectTable unsupported" record per physical light (the
first 12).
=================
*/
void R_ULightPhysPublish( void ) {
	char buf[1000];
	int i, n = 0;

	if ( !ri.DebugSet ) {
		return;
	}
	buf[0] = 0;
	for ( i = 0; i < ulw.numMapLights && n < 12; i++ ) {
		const uLight_t *l = &ulw.lights[i];
		const uLightPhys_t *ph = &l->parms.phys;

		if ( !ph->physical ) {
			continue;
		}
		Q_strcat( buf, sizeof( buf ), va( "%s%d:%d %.2f %.5f %.5f %.4f %d %d %.4f %.4f %.4f %d %d", n ? ";" : "", l->entityNum,
			ph->profile, ph->radius, ph->intensity, ph->cap, ph->capKnee, ph->lambert, ph->mask,
			ph->color[0] * l->parms.shaderParms[0], ph->color[1] * l->parms.shaderParms[1], ph->color[2] * l->parms.shaderParms[2],
			ph->effectTable, ph->unsupported ) );
		n++;
	}
	ri.DebugSet( "r_ulight_phys", va( "%d %.3f %d %.3f", ulw.numPhysical, ulw.overbright, ulw.numZoneAmbient, ulw.ue1LevelBrightness ) );
	ri.DebugSet( "r_ulight_phys_lights", buf[0] ? buf : "-" );
}
