/*
===========================================================================

idlib_lite: the parts of the DOOM-3 idLib that the engine's C++ modules
(the id Tech 4 script VM in code/idscript) need, and nothing else.

This header replaces DOOM-3's precompiled.h + Lib.h + sys_public.h for
those modules. The adapted id files keep their own headers; this file is
new glue (GPLv3, as the rest of the id Tech 4 code in this tree).

Adapted from DOOM-3 neo/idlib/Lib.h, neo/idlib/precompiled.h and
neo/sys/sys_public.h: only the types, macros and the idLib::common
interface the copied idLib files use. idLib::common forwards to the
ioquake3 engine through idlib_bridge.c (plain C calls), so no engine
header is included in C++. Errors never throw: the module that runs the
code installs an error handler (idLib_SetErrorHandler) that longjmps.

All math that can reach gameplay goes through deterministic functions
(musl sin/cos/atan2 in qcommon/detmath, IEEE sqrt), so native and wasm
builds compute the same bits.

===========================================================================
*/

#ifndef __IDLIB_LITE_H__
#define __IDLIB_LITE_H__

#ifndef __cplusplus
#error idlib_lite.h is C++ only
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <new>

#define ID_INLINE						inline
#define ID_STATIC_TEMPLATE
#define id_attribute(x)					__attribute__(x)
#define ALIGN16( x )					x
#define ID_TIME_T						long

typedef unsigned char			byte;		// 8 bits
typedef unsigned short			word;		// 16 bits
typedef unsigned int			dword;		// 32 bits
typedef unsigned int			uint;
typedef unsigned long			ulong;

#ifndef BIT
#define BIT( num )				( 1 << ( num ) )
#endif

#define	MAX_STRING_CHARS		1024		// max length of a string

// ---- engine bridge (idlib_bridge.c, plain C) ---------------------------------
extern "C" {
void	idlib_Print( const char *text );
void	idlib_DPrint( const char *text );
int		idlib_ReadFile( const char *path, void **buffer );	// length, -1 if missing
void	idlib_FreeFile( void *buffer );
double	idlib_Sin( double x );
double	idlib_Cos( double x );
double	idlib_Atan2( double y, double x );
int		idlib_Milliseconds( void );
}

// ---- idLib::common ------------------------------------------------------------
class idCommon {
public:
	void			Printf( const char *fmt, ... ) id_attribute((format(printf,2,3)));
	void			DPrintf( const char *fmt, ... ) id_attribute((format(printf,2,3)));
	void			Warning( const char *fmt, ... ) id_attribute((format(printf,2,3)));
	void			DWarning( const char *fmt, ... ) id_attribute((format(printf,2,3)));
	// never returns: calls the installed error handler (see idLib_SetErrorHandler)
	void			Error( const char *fmt, ... ) id_attribute((format(printf,2,3), noreturn));
	void			FatalError( const char *fmt, ... ) id_attribute((format(printf,2,3), noreturn));
};

class idLib {
public:
	static idCommon *			common;
	static int					frameNumber;

	static void					Init( void ) {}
	static void					ShutDown( void ) {}

	static void					Error( const char *fmt, ... ) id_attribute((format(printf,1,2), noreturn));
	static void					Warning( const char *fmt, ... ) id_attribute((format(printf,1,2)));
};

// The error handler must not return (it longjmps). With none installed an
// error aborts through the engine's Com_Error( ERR_DROP ).
typedef void ( *idLibErrorHandler_t )( const char *text );
idLibErrorHandler_t	idLib_SetErrorHandler( idLibErrorHandler_t handler );
extern "C" void	idlib_FatalError( const char *text ) id_attribute((noreturn));

class idException {
public:
	char error[MAX_STRING_CHARS];
	idException( const char *text = "" ) { strncpy( error, text, sizeof( error ) - 1 ); error[sizeof( error ) - 1] = 0; }
};

template<class T> ID_INLINE T	Max( T x, T y ) { return ( x > y ) ? x : y; }
template<class T> ID_INLINE T	Min( T x, T y ) { return ( x < y ) ? x : y; }

// ---- memory (DOOM-3 Heap.h subset) -------------------------------------------
ID_INLINE void *	Mem_Alloc( const int size ) { return malloc( size ? size : 1 ); }
ID_INLINE void *	Mem_ClearedAlloc( const int size ) { return calloc( 1, size ? size : 1 ); }
ID_INLINE void		Mem_Free( void *ptr ) { free( ptr ); }
ID_INLINE char *	Mem_CopyString( const char *in ) { char *out = (char *)malloc( strlen( in ) + 1 ); strcpy( out, in ); return out; }

#include "Math_lite.h"
#include "List.h"
#include "Str.h"
#include "Token.h"
#include "Lexer.h"
#include "Parser.h"
#include "StaticList.h"
#include "HashIndex.h"
#include "StrList.h"

#endif /* !__IDLIB_LITE_H__ */
