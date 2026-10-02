/*
===========================================================================
tr_oax_water.c: water surfaces (phase 6), opt-in by shader keyword.

A shader with the keyword `oaxWater` draws its surfaces with the water
program instead of its stages (the stages stay as the fallback: other
renderers, r_oaxWater 0, reflection views):

    textures/oax_fx/pool
    {
        surfaceparm trans
        surfaceparm water
        oaxWater
        oaxWaterParm tint 0.05 0.22 0.25      // deep water colour
        oaxWaterParm density 0.012            // tint per unit of depth
        oaxWaterParm scale 0.0078             // normal map repeats per unit
        oaxWaterParm speed 0.04 0.03          // the two wave layers, repeats per second
        oaxWaterParm distortion 0.025         // screen-space offset of refraction and reflection
        oaxWaterParm reflectivity 0.8         // reflection at grazing angles
        oaxWaterParm fresnel 0.04             // reflection looking straight down
        oaxWaterParm waves 0.4                // wave normal strength
        oaxWaterParm normalMap textures/x.tga // default *oaxwaves, built in
        { map textures/liquids/clear_calm1 ... }  // fallback stages
    }

- Reflection: when a view sees a world surface of a water shader, the
  scene is rendered first from the camera mirrored in that surface's
  plane, clipped at the plane (an oblique near plane, as Q3 portals),
  into a half-size HDR target (R_OAXWaterReflection). One plane per view.
- Refraction: the opaque scene's colour (the scene copy) seen through the
  surface, offset by the wave normal where what is behind is underwater.
- Depth tint: the water thickness along the view ray (scene depth copy
  minus the surface's depth) absorbs toward the tint colour.
- Waves: two scrolling layers of a normal map, by shader time (so
  r_fixedShaderTime freezes them).
===========================================================================
*/

#include "tr_local.h"

typedef struct oaxWater_s {
	image_t	*normalMap;
	float	scale;
	float	speed[2];
	float	distortion;
	vec3_t	tint;
	float	density;
	float	reflectivity;
	float	fresnel;
	float	waves;			// wave normal strength (0 = flat)
} oaxWater_t;

static FBO_t	*reflectFbo;
static image_t	*reflectImage;
static image_t	*wavesImage;

/*
=================
WavesImage

A tileable normal map from a sum of integer-frequency waves, built once.
=================
*/
#define WAVES_SIZE 128

static image_t *WavesImage( void ) {
	static const float waves[6][4] = {
		// fx, fy, amplitude, phase
		{ 1, 2, 1.0f, 0.3f }, { 3, -1, 0.6f, 1.7f }, { -2, 5, 0.35f, 4.1f },
		{ 7, 3, 0.2f, 2.2f }, { -5, -8, 0.12f, 5.3f }, { 11, -6, 0.08f, 0.9f },
	};
	byte *data;
	int x, y, i;

	if ( wavesImage ) {
		return wavesImage;
	}
	data = ri.Hunk_AllocateTempMemory( WAVES_SIZE * WAVES_SIZE * 4 );
	for ( y = 0; y < WAVES_SIZE; y++ ) {
		for ( x = 0; x < WAVES_SIZE; x++ ) {
			float dx = 0, dy = 0, len;
			vec3_t n;
			byte *p = data + ( y * WAVES_SIZE + x ) * 4;

			for ( i = 0; i < 6; i++ ) {
				float a = 2.0f * M_PI * ( waves[i][0] * x + waves[i][1] * y ) / WAVES_SIZE + waves[i][3];
				float c = waves[i][2] * cos( a ) * 2.0f * M_PI / WAVES_SIZE * 6.0f;

				dx += c * waves[i][0];
				dy += c * waves[i][1];
			}
			VectorSet( n, -dx, -dy, 1.0f );
			len = VectorLength( n );
			for ( i = 0; i < 3; i++ ) {
				p[i] = (byte)( ( n[i] / len * 0.5f + 0.5f ) * 255.0f + 0.5f );
			}
			p[3] = 255;
		}
	}
	wavesImage = R_CreateImage( "*oaxwaves", data, WAVES_SIZE, WAVES_SIZE, IMGTYPE_COLORALPHA,
		IMGFLAG_MIPMAP | IMGFLAG_NO_COMPRESSION | IMGFLAG_NOLIGHTSCALE, GL_RGBA8 );
	ri.Hunk_FreeTempMemory( data );
	return wavesImage;
}

/*
=================
R_OAXParseWaterKeyword

From ParseShader, for shader-level keywords.
=================
*/
qboolean R_OAXParseWaterKeyword( const char *token, char **text, oaxWater_t **water, const char *shaderName ) {
	oaxWater_t *w;
	char *name;
	int i;

	if ( Q_stricmp( token, "oaxWater" ) && Q_stricmp( token, "oaxWaterParm" ) ) {
		return qfalse;
	}
	if ( !*water ) {
		w = *water = ri.Hunk_Alloc( sizeof( oaxWater_t ), h_low );
		w->normalMap = NULL;
		w->scale = 1.0f / 128.0f;
		w->speed[0] = 0.04f;
		w->speed[1] = 0.03f;
		w->distortion = 0.025f;
		VectorSet( w->tint, 0.05f, 0.2f, 0.22f );
		w->density = 0.01f;
		w->reflectivity = 0.8f;
		w->fresnel = 0.04f;
		w->waves = 0.4f;
	}
	w = *water;
	if ( !Q_stricmp( token, "oaxWater" ) ) {
		SkipRestOfLine( text );
		return qtrue;
	}

	name = COM_ParseExt( text, qfalse );
	if ( !Q_stricmp( name, "tint" ) ) {
		for ( i = 0; i < 3; i++ ) {
			w->tint[i] = atof( COM_ParseExt( text, qfalse ) );
		}
	} else if ( !Q_stricmp( name, "density" ) ) {
		w->density = atof( COM_ParseExt( text, qfalse ) );
	} else if ( !Q_stricmp( name, "scale" ) ) {
		w->scale = atof( COM_ParseExt( text, qfalse ) );
	} else if ( !Q_stricmp( name, "speed" ) ) {
		w->speed[0] = atof( COM_ParseExt( text, qfalse ) );
		w->speed[1] = atof( COM_ParseExt( text, qfalse ) );
	} else if ( !Q_stricmp( name, "distortion" ) ) {
		w->distortion = atof( COM_ParseExt( text, qfalse ) );
	} else if ( !Q_stricmp( name, "reflectivity" ) ) {
		w->reflectivity = atof( COM_ParseExt( text, qfalse ) );
	} else if ( !Q_stricmp( name, "waves" ) ) {
		w->waves = atof( COM_ParseExt( text, qfalse ) );
	} else if ( !Q_stricmp( name, "fresnel" ) ) {
		w->fresnel = atof( COM_ParseExt( text, qfalse ) );
	} else if ( !Q_stricmp( name, "normalMap" ) ) {
		char *img = COM_ParseExt( text, qfalse );

		w->normalMap = R_FindImageFile( img, IMGTYPE_COLORALPHA, IMGFLAG_MIPMAP | IMGFLAG_NOLIGHTSCALE | IMGFLAG_NO_COMPRESSION );
		if ( !w->normalMap ) {
			ri.Printf( PRINT_WARNING, "WARNING: shader %s: oaxWaterParm normalMap %s not found\n", shaderName, img );
		}
	} else {
		ri.Printf( PRINT_WARNING, "WARNING: shader %s: unknown oaxWaterParm %s\n", shaderName, name );
	}
	SkipRestOfLine( text );
	return qtrue;
}

/*
=================
R_OAXWaterFinishShader

The water program reads positions and normals.
=================
*/
void R_OAXWaterFinishShader( shader_t *sh ) {
	if ( !sh->oaxWater ) {
		return;
	}
	sh->vertexAttribs |= ATTR_POSITION | ATTR_NORMAL;
	if ( !sh->oaxWater->normalMap ) {
		sh->oaxWater->normalMap = WavesImage();
	}
}

/*
=================
R_OAXWaterReflection

From R_SortDrawSurfs: if the view sees a water surface, render the view
mirrored in its plane into the reflection target first.
=================
*/
void R_OAXWaterReflection( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	viewParms_t	oldParms, newParms;
	cplane_t	plane;
	float		d;
	int			i, k;

	if ( r_oaxWater->integer != 1 || tr.viewParms.isPortal || tr.viewParms.oaxReflection || tr.viewParms.targetFbo
		|| !glRefConfig.framebufferObject || !tr.renderFbo || ( tr.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_OAX_SKYPORTAL ) ) ) {
		return;
	}

	for ( i = 0; i < numDrawSurfs; i++ ) {
		shader_t *sh;
		int entityNum, fogNum, dlighted, pshadowed;

		R_DecomposeSort( drawSurfs[i].sort, &entityNum, &sh, &fogNum, &dlighted, &pshadowed );
		if ( !sh->oaxWater || entityNum != REFENTITYNUM_WORLD ) {
			continue;
		}
		R_PlaneForSurface( drawSurfs[i].surface, &plane );
		if ( *drawSurfs[i].surface == SF_TRIANGLES ) {
			// a triangle soup's winding may face either way: its normals decide
			srfBspSurface_t *tri = (srfBspSurface_t *)drawSurfs[i].surface;
			vec3_t n;

			R_VaoUnpackNormal( n, tri->verts[tri->indexes[0]].normal );
			if ( DotProduct( n, plane.normal ) < 0 ) {
				VectorScale( plane.normal, -1.0f, plane.normal );
				plane.dist = -plane.dist;
			}
		}
		d = DotProduct( tr.viewParms.or.origin, plane.normal ) - plane.dist;
		if ( d > 0.5f ) {
			break;
		}
	}
	if ( i == numDrawSurfs ) {
		return;
	}

	if ( !reflectFbo ) {
		int hdrFormat = ( r_hdr->integer && glRefConfig.textureFloat ) ? GL_RGBA16F_ARB : GL_RGBA8;
		int w = MAX( glConfig.vidWidth / 2, 1 ), h = MAX( glConfig.vidHeight / 2, 1 );

		R_IssuePendingRenderCommands();
		reflectImage = R_CreateImage( "*oaxReflection", NULL, w, h, IMGTYPE_COLORALPHA,
			IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, hdrFormat );
		reflectFbo = FBO_Create( "_oaxReflection", w, h );
		FBO_AttachImage( reflectFbo, reflectImage, GL_COLOR_ATTACHMENT0, 0 );
		FBO_CreateBuffer( reflectFbo, R_ULightDepthFormat(), 0, 0 );
		R_CheckFBO( reflectFbo );
		FBO_Bind( NULL );
	}

	oldParms = tr.viewParms;
	newParms = tr.viewParms;
	VectorMA( oldParms.or.origin, -2.0f * d, plane.normal, newParms.or.origin );
	for ( k = 0; k < 3; k++ ) {
		VectorMA( oldParms.or.axis[k], -2.0f * DotProduct( oldParms.or.axis[k], plane.normal ), plane.normal, newParms.or.axis[k] );
	}
	newParms.isPortal = qtrue;
	newParms.isMirror = qtrue;
	VectorCopy( plane.normal, newParms.portalPlane.normal );
	newParms.portalPlane.dist = plane.dist - 1.0f;	// a little under the surface, so shorelines meet
	newParms.zFar = 0.0f;
	newParms.flags &= ~( VPF_FARPLANEFRUSTUM | VPF_USESUNLIGHT );
	newParms.flags |= VPF_NOVIEWMODEL;
	newParms.viewportX = 0;
	newParms.viewportY = 0;
	newParms.viewportWidth = reflectFbo->width;
	newParms.viewportHeight = reflectFbo->height;
	newParms.targetFbo = reflectFbo;
	newParms.oaxReflection = qtrue;
	newParms.oaxHasReflection = qfalse;

	R_RenderView( &newParms );

	tr.viewParms = oldParms;
	tr.viewParms.oaxHasReflection = qtrue;
	oaxFxStats.waterReflections++;
}

/*
=================
RB_OAXWaterStageIterator

From RB_StageIteratorGeneric: draws a water batch. qfalse lets the
shader's own stages draw it.
=================
*/
qboolean RB_OAXWaterStageIterator( shaderCommands_t *input ) {
	oaxWater_t		*w = input->shader->oaxWater;
	shaderProgram_t	*sp = &tr.oaxWaterShader;
	vec4_t			v;
	vec2_t			invRes;
	image_t			*color, *depth;
	qboolean		reflect;

	if ( !w || !r_oaxWater->integer || !sp->program ) {
		return qfalse;
	}
	if ( backEnd.viewParms.oaxReflection ) {
		return qtrue;	// not in its own reflection
	}
	if ( backEnd.viewParms.isPortal || backEnd.viewParms.targetFbo ) {
		return qfalse;
	}
	if ( !RB_OAXSceneCopy() ) {
		return qfalse;
	}
	color = RB_OAXSceneColor();
	depth = RB_OAXSceneDepth();
	reflect = backEnd.viewParms.oaxHasReflection && reflectImage && r_oaxWater->integer == 1;

	GLSL_BindProgram( sp );
	GLSL_SetUniformMat4( sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection );
	// the world's orientation has no transform matrix: identity
	if ( backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity ) {
		GLSL_SetUniformMat4( sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix );
	} else {
		mat4_t identity;

		Mat4Identity( identity );
		GLSL_SetUniformMat4( sp, UNIFORM_MODELMATRIX, identity );
	}
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin );
	GLSL_SetUniformVec3( sp, UNIFORM_VIEWFORWARD, backEnd.viewParms.or.axis[0] );
	GLSL_SetUniformFloat( sp, UNIFORM_TIME, tess.shaderTime );

	VectorSet4( v, w->scale, w->speed[0], w->speed[1], w->distortion );
	GLSL_SetUniformVec4( sp, UNIFORM_NORMALSCALE, v );
	VectorSet4( v, w->tint[0], w->tint[1], w->tint[2], w->density );
	GLSL_SetUniformVec4( sp, UNIFORM_SPECULARSCALE, v );
	GLSL_SetUniformFloat( sp, UNIFORM_VERTEXLERP, w->waves );
	VectorSet4( v, r_znear->value, backEnd.viewParms.zFar, reflect ? w->reflectivity : 0.0f, w->fresnel );
	GLSL_SetUniformVec4( sp, UNIFORM_VIEWINFO, v );
	VectorSet4( v, backEnd.viewParms.viewportX, backEnd.viewParms.viewportY,
		1.0f / backEnd.viewParms.viewportWidth, 1.0f / backEnd.viewParms.viewportHeight );
	GLSL_SetUniformVec4( sp, UNIFORM_CUBEMAPINFO, v );
	invRes[0] = 1.0f / color->width;
	invRes[1] = 1.0f / color->height;
	GLSL_SetUniformVec2( sp, UNIFORM_INVTEXRES, invRes );

	GL_BindToTMU( reflect ? reflectImage : color, TB_COLORMAP );
	GL_BindToTMU( color, TB_LIGHTMAP );
	GL_BindToTMU( w->normalMap ? w->normalMap : tr.whiteImage, TB_NORMALMAP );
	GL_BindToTMU( depth, TB_SHADOWMAP );

	GL_State( GLS_DEPTHMASK_TRUE );
	R_DrawElements( input->numIndexes, input->firstIndex );

	// later translucent passes see the water in the scene copy
	RB_OAXSceneCopyInvalidate();
	oaxFxStats.waterSurfsDrawn++;
	return qtrue;
}

/*
=================
R_OAXWaterReset

At renderer start (the images and targets of the last one are gone).
=================
*/
void R_OAXWaterReset( void ) {
	reflectFbo = NULL;
	reflectImage = NULL;
	wavesImage = NULL;
}
