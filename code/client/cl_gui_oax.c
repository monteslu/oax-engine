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
cl_gui_oax.c: in-world GUI syscalls for the cgame module (CG_OAX_GUI_*,
1030-1049, see qcommon/oax.h).

The client keeps its own copy of each GUI only to draw it: the cgame
loads one per func_oax_gui, applies the state the server publishes in
CS_OAX_GUISTATE, and adds the panel entity with CG_OAX_R_ADDREFENTITYEXT.
The first time a GUI's entity is added in a frame, the GUI is redrawn
into its render target (BeginGuiTarget / EndGuiTarget) at cl.serverTime,
before the scene that shows it is rendered. Hover and the cursor are
cosmetic: clicks are the server's.
===========================================================================
*/

#include "client.h"
#include "../qcommon/oax.h"
#include "../idgui/idgui_public.h"

static cvar_t	*r_guiSize;
static int		guiDrawnFrame[IDGUI_MAX_GUIS + 1];
static float	guiTargetHeight;	// the target being drawn, for the flip below

/*
=================
CL_GuiDraw

Redraws a GUI into its render target, once per frame.
=================
*/
static void CL_GuiDraw( int handle ) {
	int size;

	if ( handle < 1 || handle > IDGUI_MAX_GUIS || !re.BeginGuiTarget ) {
		return;
	}
	if ( guiDrawnFrame[handle] == cls.framecount ) {
		return;
	}
	guiDrawnFrame[handle] = cls.framecount;

	size = r_guiSize->integer;
	if ( !re.BeginGuiTarget( handle, size, size ) ) {
		return;
	}
	guiTargetHeight = size;
	IDGUI_Redraw( IDGUI_CLIENT, handle, cl.serverTime, size, size );
	re.EndGuiTarget();
	// the HUD that follows draws in white unless it sets a color
	re.SetColor( NULL );
}

static qboolean CL_OAXGuiCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case CG_OAX_GUI_LOAD:
		*ret = IDGUI_Load( IDGUI_CLIENT, VMA( 1 ) );
		if ( *ret > 0 && *ret <= IDGUI_MAX_GUIS ) {
			guiDrawnFrame[*ret] = -1;
		}
		return qtrue;

	case CG_OAX_GUI_FREE:
		IDGUI_Free( IDGUI_CLIENT, args[1] );
		*ret = 0;
		return qtrue;

	case CG_OAX_GUI_SETSTATE:
		IDGUI_SetState( IDGUI_CLIENT, args[1], VMA( 2 ), VMA( 3 ) );
		IDGUI_StateChanged( IDGUI_CLIENT, args[1], cl.serverTime );
		*ret = 0;
		return qtrue;

	case CG_OAX_GUI_ACTIVATE:
		// the client's commands are not acted on: the server's GUI runs the game
		IDGUI_Activate( IDGUI_CLIENT, args[1], args[2], cl.serverTime );
		*ret = 0;
		return qtrue;

	case CG_OAX_R_ADDREFENTITYEXT: {
		const refEntityExt_t *ext = VMA( 2 );

		VM_CheckBlock( args[2], sizeof( refEntityExt_t ), "REFENTITYEXT" );
		if ( ext->guiHandle ) {
			CL_GuiDraw( ext->guiHandle );
		}
		if ( re.AddRefEntityToSceneExt ) {
			re.AddRefEntityToSceneExt( VMA( 1 ), ext );
		} else {
			re.AddRefEntityToScene( VMA( 1 ) );
		}
		*ret = 0;
		return qtrue;
	}

	case CG_OAX_GUI_TRACE: {
		// ( inlineModel, origin, angles, start, end, float xyf[3] ) -> hit
		float xy[3];

		*ret = 0;
		VM_CheckBlock( args[6], sizeof( xy ), "GUITRACE" );
		if ( CM_GuiTrace( args[1], VMA( 2 ), VMA( 3 ), VMA( 4 ), VMA( 5 ), &xy[0], &xy[1], &xy[2] ) ) {
			Com_Memcpy( VMA( 6 ), xy, sizeof( xy ) );
			*ret = 1;
		}
		return qtrue;
	}

	case CG_OAX_GUI_CURSOR:
		IDGUI_MouseEvent( IDGUI_CLIENT, args[1], VMF( 2 ), VMF( 3 ), 0, cl.serverTime );
		*ret = 0;
		return qtrue;
	}
	return qfalse;
}

/*
=================
CL_OAXGuiReset

Every cgame start begins with no client GUIs; the renderer may have been
restarted, so shader handles register again.
=================
*/
void CL_OAXGuiReset( void ) {
	IDGUI_FreeAll( IDGUI_CLIENT );
	IDGUI_PurgeShaders();
	Com_Memset( guiDrawnFrame, 0, sizeof( guiDrawnFrame ) );
}

static int CL_GuiRegisterShader( const char *name ) {
	return re.RegisterShaderNoMip ? re.RegisterShaderNoMip( name ) : 0;
}

static void CL_GuiSetColor( const float *rgba ) {
	re.SetColor( rgba );
}

/*
	A render target's row 0 is texture row 0 (t = 0, the GUI's top), but 2D
	drawing puts y = 0 at the top of the target as on the screen, which is
	the texture's last row. So GUIs draw upside down: y mirrors and t swaps,
	which keeps every quad's winding (and the 2D shaders' culling) as is.
*/
static void CL_GuiDrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int shader ) {
	re.DrawStretchPic( x, guiTargetHeight - y - h, w, h, s1, t2, s2, t1, shader );
}

static void CL_GuiDrawQuad( const float *xy, const float *st, int shader ) {
	float fxy[8], fst[8];
	int i;

	if ( !re.DrawQuad ) {
		return;
	}
	// mirror y and reverse the corner order (the mirror flips the winding)
	for ( i = 0; i < 4; i++ ) {
		fxy[i * 2 + 0] = xy[( 3 - i ) * 2 + 0];
		fxy[i * 2 + 1] = guiTargetHeight - xy[( 3 - i ) * 2 + 1];
		fst[i * 2 + 0] = st[( 3 - i ) * 2 + 0];
		fst[i * 2 + 1] = st[( 3 - i ) * 2 + 1];
	}
	re.DrawQuad( fxy, fst, shader );
}

static void CL_GuiLocalSound( const char *name ) {
	sfxHandle_t sfx = S_RegisterSound( name, qfalse );

	if ( sfx ) {
		S_StartLocalSound( sfx, CHAN_LOCAL_SOUND );
	}
}

static void CL_GuiPrint( const char *msg ) {
	Com_Printf( "%s", msg );
}

static void CL_GuiWarning( const char *msg ) {
	Com_Printf( S_COLOR_YELLOW "%s\n", msg );
}

static int CL_GuiReadFile( const char *path, void **buffer ) {
	return FS_ReadFile( path, buffer );
}

static void CL_GuiFreeFile( void *buffer ) {
	FS_FreeFile( buffer );
}

void CL_OAXGuiInit( void ) {
	idguiImport_t imp;

	r_guiSize = Cvar_Get( "r_guiSize", "512", CVAR_ARCHIVE );
	Cvar_CheckRange( r_guiSize, 64, 2048, qtrue );
	Cvar_SetDescription( r_guiSize, "Pixel size of the square texture each in-world GUI draws into (its 640x480 screen is scaled to fit)." );

	Com_Memset( &imp, 0, sizeof( imp ) );
	imp.Print = CL_GuiPrint;
	imp.Warning = CL_GuiWarning;
	imp.ReadFile = CL_GuiReadFile;
	imp.FreeFile = CL_GuiFreeFile;
	imp.RegisterShader = CL_GuiRegisterShader;
	imp.SetColor = CL_GuiSetColor;
	imp.DrawStretchPic = CL_GuiDrawStretchPic;
	imp.DrawQuad = CL_GuiDrawQuad;
	imp.LocalSound = CL_GuiLocalSound;
	IDGUI_Init( &imp );

	CL_OAXRegisterCgameHandler( CL_OAXGuiCalls );
}
