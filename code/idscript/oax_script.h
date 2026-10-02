/*
===========================================================================
oax_script.h: the id Tech 4 script VM as the engine's C code sees it.

The game module (QVM) drives the VM through the G_OAX_SCRIPT_* syscalls
(qcommon/oax.h, server/sv_script_oax.c), in a pump: RUN executes every
script thread that is due, and whenever a thread calls an event the game
module implements (an entity event like $door.moveTo(), or a game sys
event like sys.getEntity()), RUN stops and hands that call back as an
oaxScriptCall_t. The game executes it, passes the result to RETURN and
calls RUN again, which resumes the thread. The VM never calls into the
game module, so its state stays simple and replayable.

Determinism: the VM reads no clock but the level time RUN is given, its
random numbers come from a xorshift32 seeded by INIT, its math is the
engine's deterministic musl functions, and a thread that runs more than
script_maxInstructions instructions in one frame is killed at the same
instruction on every build.

New code for the oax engine (GPLv3, like the id Tech 4 code it serves).
This header is plain C; it is shared with the QVM side (mirrored in
oa-gamecode code/game/oax_public.h), so its layout never changes once
shipped.
===========================================================================
*/

#ifndef OAX_SCRIPT_H
#define OAX_SCRIPT_H

#ifdef __cplusplus
extern "C" {
#endif

// value types in a call record or a RETURN
#define OAX_SV_VOID			0
#define OAX_SV_FLOAT		1		// f[0]
#define OAX_SV_INT			2		// i ('d' arguments: the script's float, truncated)
#define OAX_SV_VECTOR		3		// f[0..2]
#define OAX_SV_STRING		4		// i = offset into the call's strings[] (RETURN: the string argument)
#define OAX_SV_ENTITY		5		// i = entity handle, 0 = none

#define OAX_SCRIPT_MAX_ARGS	8
#define OAX_SCRIPT_STRINGS	1024

typedef struct {
	int		type;
	int		i;
	float	f[3];
} oaxScriptValue_t;

typedef struct {
	int					event;		// number REGISTER_EVENT returned
	int					self;		// entity the event is called on (0 for sys events)
	int					thread;		// thread number making the call
	int					argc;
	oaxScriptValue_t	args[OAX_SCRIPT_MAX_ARGS];
	char				strings[OAX_SCRIPT_STRINGS];
} oaxScriptCall_t;

// REGISTER_EVENT flags
#define OAX_EVENT_ENTITY	2		// called on an entity: $name.event( ... )
#define OAX_EVENT_SYS		4		// called as sys.event( ... )

// Every entry point below is safe to call in any state; before INIT they
// do nothing and return 0.

// Reset the VM: no program, no threads, only the engine's own thread events
// (wait, print, sin, ...) registered. seed seeds sys.random().
int		OAXScript_Init( int randomSeed );
// An event the game module executes. argfmt is D3's event format
// ("d" int, "f" float, "v" vector, "s" string, "e" entity, "E" entity or
// null), ret the return type character (0 for void). The name must also be
// declared in script as a scriptEvent with matching types. Returns the
// event number, -1 on error.
int		OAXScript_RegisterEvent( const char *name, const char *argfmt, int ret, int flags );
// 1 compiled, 0 compile error (printed, and in g_script_error), -1 missing
int		OAXScript_CompileFile( const char *path );
// Point the script's $name at an entity handle. 1 if the scripts use $name.
int		OAXScript_SetEntity( const char *name, int handle );
// Start func as a new thread on the next RUN. If func takes one entity
// parameter it gets self. Returns the thread number, 0 if no such function.
int		OAXScript_StartThread( const char *func, int self );
// Run every due thread at levelTime. 0: done for now. 1: *call holds an
// event the game must execute, then RETURN, then RUN again.
int		OAXScript_Run( int levelTime, oaxScriptCall_t *call );
// The pending call's result (string: used when value->type is OAX_SV_STRING).
void	OAXScript_Return( const oaxScriptValue_t *value, const char *string );
// D3 idThread::ObjectMoveDone: a thread waiting on this entity (sys.waitFor)
// wakes up.
void	OAXScript_ObjectDone( int threadNum, int handle );
void	OAXScript_KillThread( int threadNum );
int		OAXScript_NumThreads( void );
void	OAXScript_Shutdown( void );

// settings (cvars live in server/sv_script_oax.c)
void	OAXScript_SetMaxInstructions( int count );
void	OAXScript_SetDebug( int debugScript, int disasm, int developer );
// console helpers
void	OAXScript_ListThreads( void );
// the event log as text, one "time event self thread" row per line;
// returns the length
int		OAXScript_LogText( char *buf, int size );

#ifdef __cplusplus
}
#endif

#endif
