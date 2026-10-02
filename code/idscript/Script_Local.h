/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================

Script_Local.h: what the script VM sees of "the game". Adapted from DOOM-3
neo/game/Game_local.h, whose place it takes for the files in this directory
(only idGameLocal's program, time and printers remain).

The VM knows no entity classes. An entity is an opaque int handle the
game module (QVM) chose, 0 meaning none; scripts store handles where D3
stored entityNumber + 1.

===========================================================================
*/

#ifndef __SCRIPT_LOCAL_H__
#define __SCRIPT_LOCAL_H__

#include "idlib_lite.h"
#include <setjmp.h>
#include <stdint.h>
#include "oax_script.h"

class idThread;
class idScriptObject;

// what D3 called idEntity is a handle from the game module
typedef int scriptEntity_t;
#define ENTITY_NONE					0

// every compiled file starts with #include of this (D3: script/doom_defs.script)
#define SCRIPT_DEFAULTDEFS			"script/oax_defs.script"

// errors leave through longjmp (D3 threw idCompileError / called common->Error):
// the innermost handler gets the message in text
typedef struct scriptErrorJump_s {
	jmp_buf						jb;
	char						text[ 1024 ];
	struct scriptErrorJump_s *	prev;
} scriptErrorJump_t;
extern scriptErrorJump_t *		scriptErrorJump;

// what DOOM-3's disassembler printed into; here it prints to the console
class idFile {
public:
	void				Printf( const char *fmt, ... ) id_attribute((format(printf,2,3)));
};

#include "Script_Event.h"
#include "Script_Program.h"
#include "Script_Compiler.h"
#include "Script_Interpreter.h"
#include "Script_Thread.h"

class idScriptGame {
public:
	idProgram			program;

	int					time;					// level time of the current RUN, ms
	int					previousTime;			// level time of the previous RUN
	int					msec;					// time - previousTime (D3 gameLocal.msec)
	unsigned int		randomState;			// xorshift32, seeded by the game
	bool				debugScript;			// g_debugScript
	bool				disasm;					// g_disasm
	bool				developer;
	int					maxInstructions;		// per thread per frame; a runaway thread is killed

						idScriptGame();

	void				Printf( const char *fmt, ... ) const id_attribute((format(printf,2,3)));
	void				DPrintf( const char *fmt, ... ) const id_attribute((format(printf,2,3)));
	void				Warning( const char *fmt, ... ) const id_attribute((format(printf,2,3)));
	// leaves the current compile or the current thread (longjmp), never returns
	void				Error( const char *fmt, ... ) const id_attribute((format(printf,2,3), noreturn));

	float				RandomFloat( void );
};

extern idScriptGame *	scriptGame;
#define gameLocal		( *scriptGame )

// compile errors (D3 threw idCompileError); never returns
void					Script_CompileError( const char *text ) id_attribute((noreturn));
// runtime errors in a thread (D3 stopped the game); never returns
void					Script_ThreadError( const char *text ) id_attribute((noreturn));

// oax_script.cpp: the pump's bookkeeping
void					Script_ReportError( const char *text );
void					Script_CountInstruction( void );
void					Script_ReportRunaway( int threadNum, const char *threadName, int instructionPointer, const char *file, int line, int count );
void					Script_GameCallPosted( idThread *thread, oaxScriptCall_t *call, const idEventDef *ev );

#endif /* !__SCRIPT_LOCAL_H__ */
