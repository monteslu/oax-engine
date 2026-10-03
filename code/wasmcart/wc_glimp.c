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
wasmcart platform backend: GL context and function loading.

The host already owns an OpenGL ES 3.0 / WebGL2 context; a cart just
imports GL entry points from the "gl" wasm module. The imports are generated
from qgl.h's own GLE() lists, so every signature is exactly the one the
renderer calls through. SDL_GL_GetProcAddress / SDL_GL_ExtensionSupported,
which the renderer calls directly, are answered from the same table.
===========================================================================
*/

#include "../renderercommon/tr_common.h"

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL_video.h"
#else
#	include <SDL_video.h>
#endif
#include "wc_local.h"

int qglMajorVersion, qglMinorVersion;
int qglesMajorVersion, qglesMinorVersion;

void (APIENTRYP qglActiveTextureARB) (GLenum texture);
void (APIENTRYP qglClientActiveTextureARB) (GLenum texture);
void (APIENTRYP qglMultiTexCoord2fARB) (GLenum target, GLfloat s, GLfloat t);

void (APIENTRYP qglLockArraysEXT) (GLint first, GLsizei count);
void (APIENTRYP qglUnlockArraysEXT) (void);

#define GLE(ret, name, ...) name##proc * qgl##name = NULL;
QGL_1_1_PROCS;
QGL_1_1_FIXED_FUNCTION_PROCS;
QGL_DESKTOP_1_1_PROCS;
QGL_DESKTOP_1_1_FIXED_FUNCTION_PROCS;
QGL_ES_1_1_PROCS;
QGL_ES_1_1_FIXED_FUNCTION_PROCS;
QGL_1_3_PROCS;
QGL_1_5_PROCS;
QGL_2_0_PROCS;
QGL_3_0_PROCS;
QGL_ARB_occlusion_query_PROCS;
QGL_ARB_framebuffer_object_PROCS;
QGL_ARB_vertex_array_object_PROCS;
QGL_EXT_direct_state_access_PROCS;
QGL_OAX_INSTANCING_PROCS;
#undef GLE

/*
===============
Imports: one per GLE() entry in the lists an ES 3.0 context provides.
===============
*/
#define WC_GL_PROC_LISTS \
	QGL_1_1_PROCS \
	QGL_ES_1_1_PROCS \
	QGL_1_3_PROCS \
	QGL_1_5_PROCS \
	QGL_2_0_PROCS \
	QGL_3_0_PROCS \
	QGL_ARB_occlusion_query_PROCS \
	QGL_ARB_framebuffer_object_PROCS \
	QGL_ARB_vertex_array_object_PROCS \
	QGL_OAX_INSTANCING_PROCS

#define GLE( ret, name, ... ) \
	__attribute__((import_module("gl"), import_name("gl" #name))) extern ret wcgl_##name( __VA_ARGS__ );
WC_GL_PROC_LISTS
#undef GLE

typedef struct {
	const char *name;
	void       *proc;
} wcGLProc_t;

#define GLE( ret, name, ... ) { "gl" #name, (void *)wcgl_##name },
static const wcGLProc_t wcGLProcs[] = {
	WC_GL_PROC_LISTS
	{ NULL, NULL }
};
#undef GLE

void *SDL_GL_GetProcAddress( const char *proc ) {
	const wcGLProc_t *p;

	for ( p = wcGLProcs; p->name; p++ ) {
		if ( !strcmp( p->name, proc ) ) {
			return p->proc;
		}
	}
	return NULL;
}

SDL_bool SDL_GL_ExtensionSupported( const char *extension ) {
	GLint i, n = 0;

	if ( !qglGetStringi ) {
		return SDL_FALSE;
	}
	qglGetIntegerv( GL_NUM_EXTENSIONS, &n );
	for ( i = 0; i < n; i++ ) {
		const char *ext = (const char *)qglGetStringi( GL_EXTENSIONS, i );
		if ( ext && !strcmp( ext, extension ) ) {
			return SDL_TRUE;
		}
	}
	return SDL_FALSE;
}

/*
===============
GLES shims for the desktop-only entry points the renderer calls
===============
*/
static void GLimp_GLES_ClearDepth( GLclampd depth ) { qglClearDepthf( depth ); }
static void GLimp_GLES_DepthRange( GLclampd near_val, GLclampd far_val ) { qglDepthRangef( near_val, far_val ); }
static void GLimp_GLES_DrawBuffer( GLenum mode ) { (void)mode; }
static void GLimp_GLES_PolygonMode( GLenum face, GLenum mode ) { (void)face; (void)mode; }

static void GLimp_GetProcAddresses( void ) {
	const char *version;

#define GLE( ret, name, ... ) qgl##name = (name##proc *)wcgl_##name;
	QGL_1_1_PROCS;
	QGL_ES_1_1_PROCS;
	QGL_1_3_PROCS;
	QGL_1_5_PROCS;
	QGL_2_0_PROCS;
	QGL_3_0_PROCS;
#undef GLE

	qglClearDepth = GLimp_GLES_ClearDepth;
	qglDepthRange = GLimp_GLES_DepthRange;
	qglDrawBuffer = GLimp_GLES_DrawBuffer;
	qglPolygonMode = GLimp_GLES_PolygonMode;

	version = (const char *)qglGetString( GL_VERSION );
	qglMajorVersion = qglMinorVersion = 0;
	qglesMajorVersion = 3;
	qglesMinorVersion = 0;
	if ( version && !Q_stricmpn( version, "OpenGL ES", 9 ) ) {
		char profile[6];
		sscanf( version, "OpenGL %5s %d.%d", profile, &qglesMajorVersion, &qglesMinorVersion );
	}
}

/*
===============
GLimp_Init
===============
*/
void GLimp_Init( qboolean fixedFunction ) {
	if ( fixedFunction ) {
		ri.Error( ERR_FATAL, "GLimp_Init: the wasmcart build has no fixed-function GL; use the GLES 3.0 renderer" );
	}

	glConfig.vidWidth = wc_width;
	glConfig.vidHeight = wc_height;
	glConfig.windowAspect = (float)wc_width / (float)wc_height;
	glConfig.isFullscreen = qtrue;
	glConfig.displayFrequency = 60;
	glConfig.colorBits = 24;
	glConfig.depthBits = 24;
	glConfig.stencilBits = 8;
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;
	glConfig.deviceSupportsGamma = qfalse;

	GLimp_GetProcAddresses();

	Q_strncpyz( glConfig.vendor_string, (const char *)qglGetString( GL_VENDOR ), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (const char *)qglGetString( GL_RENDERER ), sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.version_string, (const char *)qglGetString( GL_VERSION ), sizeof( glConfig.version_string ) );

	{
		GLint i, n = 0, len = 0;
		qglGetIntegerv( GL_NUM_EXTENSIONS, &n );
		glConfig.extensions_string[0] = '\0';
		for ( i = 0; i < n; i++ ) {
			const char *ext = (const char *)qglGetStringi( GL_EXTENSIONS, i );
			int         elen = ext ? (int)strlen( ext ) : 0;
			if ( !elen || len + elen + 1 >= (int)sizeof( glConfig.extensions_string ) ) {
				continue;
			}
			if ( len ) {
				Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), " " );
				len++;
			}
			Q_strcat( glConfig.extensions_string, sizeof( glConfig.extensions_string ), ext );
			len += elen;
		}
	}

	glConfig.textureCompression = TC_NONE;
	glConfig.textureEnvAddAvailable = qfalse;

	ri.Cvar_Set( "r_mode", "-1" );
	ri.Cvar_Set( "r_customwidth", va( "%d", wc_width ) );
	ri.Cvar_Set( "r_customheight", va( "%d", wc_height ) );
	ri.Cvar_Get( "r_availableModes", "", CVAR_ROM );

	ri.Printf( PRINT_ALL, "wasmcart GL: %s (%dx%d)\n", glConfig.version_string, wc_width, wc_height );

	ri.IN_Init( NULL );
}

void GLimp_Shutdown( void ) {
	ri.IN_Shutdown();
}

// The host presents the frame when wc_render() returns.
void GLimp_EndFrame( void ) { }
void GLimp_Minimize( void ) { }
void GLimp_LogComment( char *comment ) { (void)comment; }

// No hardware gamma: renderergl2 bakes gamma into textures when
// deviceSupportsGamma is false.
void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] ) {
	(void)red; (void)green; (void)blue;
}
