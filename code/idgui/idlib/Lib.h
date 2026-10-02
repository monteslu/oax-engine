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
*/

/*
Adapted from DOOM-3 neo/idlib/Lib.h for the oa-engine GUI module.
Changes: only the types and helpers the GUI port uses; idLib's interface
pointers became the engine import table (idgui_public.h); errors longjmp
to the active guard instead of throwing (the module builds with
-fno-exceptions -fno-rtti); everything lives in namespace idgui.
*/

#ifndef __IDGUI_LIB_H__
#define __IDGUI_LIB_H__

#define ID_INLINE					inline
#define ID_STATIC_TEMPLATE			static
#define id_attribute(x)				__attribute__(x)
#define ID_TIME_T					long

typedef unsigned char			byte;
typedef unsigned short			word;
typedef unsigned int			dword;
typedef unsigned int			uint;

typedef int						qhandle_t;

#define	MAX_STRING_CHARS		1024

#ifndef BIT
#define BIT( num )				( 1 << ( num ) )
#endif

class idVec3;
class idVec4;
class idStr;

/*
	The engine side of the library: messages, files and the error path.
	Error() longjmps to the innermost idLibGuard (see idgui_main.cpp).
*/
class idCommonLite {
public:
	void			Printf( const char *fmt, ... ) id_attribute((format(printf,2,3)));
	void			DPrintf( const char *fmt, ... ) id_attribute((format(printf,2,3)));
	void			Warning( const char *fmt, ... ) id_attribute((format(printf,2,3)));
	void			DWarning( const char *fmt, ... ) id_attribute((format(printf,2,3)));
	void			Error( const char *fmt, ... ) id_attribute((format(printf,2,3), noreturn));
	void			FatalError( const char *fmt, ... ) id_attribute((format(printf,2,3), noreturn));
};

class idFileSystemLite {
public:
	// -1 when the file is missing; buffer is NUL terminated
	int				ReadFile( const char *path, void **buffer );
	void			FreeFile( void *buffer );
};

class idLib {
public:
	static idCommonLite *		common;
	static idFileSystemLite *	fileSystem;

	static void					Error( const char *fmt, ... ) id_attribute((format(printf,1,2), noreturn));
	static void					Warning( const char *fmt, ... ) id_attribute((format(printf,1,2)));
};

extern idCommonLite *			common;
extern idFileSystemLite *		fileSystem;

// allocation: plain heap memory (D3's block allocators are not ported)
void *	Mem_Alloc( const int size );
void *	Mem_ClearedAlloc( const int size );
void	Mem_Free( void *ptr );
char *	Mem_CopyString( const char *in );

template<class T> ID_INLINE T	Max( T x, T y ) { return ( x > y ) ? x : y; }
template<class T> ID_INLINE T	Min( T x, T y ) { return ( x < y ) ? x : y; }

extern	idVec4 colorBlack;
extern	idVec4 colorWhite;

#endif
