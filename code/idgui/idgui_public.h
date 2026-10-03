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
idgui_public.h: the C interface of the in-world GUI module (code/idgui/).

The module is a C++ port of DOOM-3's GUI system (neo/ui). Nothing outside
code/idgui/ sees its classes: the engine's C code talks to it through these
functions, and the module reaches the engine only through idguiImport_t,
so no engine header is ever compiled as C++.

Two instance tables share one factory: IDGUI_SERVER instances are headless
(events, transitions, gui scripts, state; they never draw) and
IDGUI_CLIENT instances draw. Handles are 1-based per table, 0 is "none".
===========================================================================
*/
#ifndef IDGUI_PUBLIC_H
#define IDGUI_PUBLIC_H

#ifdef __cplusplus
extern "C" {
#endif

#define IDGUI_SERVER		0
#define IDGUI_CLIENT		1

#define IDGUI_MAX_GUIS		64

// the GUI's virtual screen, as in DOOM-3
#define IDGUI_VIRTUAL_WIDTH	640
#define IDGUI_VIRTUAL_HEIGHT	480

// mouse buttons for IDGUI_MouseEvent
#define IDGUI_BUTTON1		1

typedef struct idguiImport_s {
	void	(*Print)( const char *msg );
	void	(*Warning)( const char *msg );
	// whole-file reads through the engine's filesystem; -1 when missing
	int		(*ReadFile)( const char *path, void **buffer );
	void	(*FreeFile)( void *buffer );

	// drawing: set by the client, NULL on a dedicated server
	int		(*RegisterShader)( const char *name );
	void	(*SetColor)( const float *rgba );
	void	(*DrawStretchPic)( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int shader );
	// a transformed quad (rotation, shear): 4 corners, x y pairs and s t pairs
	void	(*DrawQuad)( const float *xy, const float *st, int shader );
	// the localSound gui command (client GUIs only)
	void	(*LocalSound)( const char *name );
} idguiImport_t;

void		IDGUI_Init( const idguiImport_t *imp );
// the client re-registers shaders after a renderer restart
void		IDGUI_PurgeShaders( void );
void		IDGUI_FreeAll( int table );

int			IDGUI_Load( int table, const char *path );
void		IDGUI_Free( int table, int handle );
int			IDGUI_IsInteractive( int table, int handle );

void		IDGUI_SetState( int table, int handle, const char *key, const char *value );
int			IDGUI_GetState( int table, int handle, const char *key, char *buf, int size );
// the whole state dictionary as an info string (\key\value...), for syncing
int			IDGUI_StateInfo( int table, int handle, char *buf, int size );
void		IDGUI_StateChanged( int table, int handle, int time );

// Events return the GUI's command string ("" when none): commands the
// window scripts issued with `set "cmd" ...`, followed by `set "<key>"
// "<value>"` for every state variable the event changed (server tables).
// x and y are the surface position, 0 to 1 across and down (IDGUI_BUTTON1 = held)
const char *IDGUI_MouseEvent( int table, int handle, float x, float y, int buttons, int time );
const char *IDGUI_Activate( int table, int handle, int activate, int time );
const char *IDGUI_NamedEvent( int table, int handle, const char *name, int time );

// draw into the current target: width x height pixels for the 640x480
// virtual screen. The cursor shows while the GUI is active (IDGUI_Activate).
void		IDGUI_Redraw( int table, int handle, int time, int width, int height );

#ifdef __cplusplus
}
#endif

#endif
