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
idgui_sys.h: the slices of DOOM-3's framework the GUI windows touch.

Adapted from DOOM-3 neo/sys/sys_public.h (sysEvent_t), neo/framework/
KeyInput.h (key numbers), neo/framework/CVarSystem.h (idCVar) and
neo/renderer/Material.h (idMaterial).
Changes: in-world GUIs run on the server, so nothing here reaches engine
state. idCVar keeps a private value (a map GUI can never read or write an
engine cvar; cvar-bound choice, slider and edit windows act unbound).
idMaterial is a shader name the client registers on first draw. Key state
queries report nothing held (world GUIs only see clicks). Lives in
namespace idgui.
*/

#ifndef __IDGUI_SYS_H__
#define __IDGUI_SYS_H__

typedef enum {
	SE_NONE,				// evTime is still valid
	SE_KEY,					// evValue is a key code, evValue2 is the down flag
	SE_CHAR,				// evValue is an ascii char
	SE_MOUSE,				// evValue and evValue2 are reletive signed x / y moves
	SE_JOYSTICK_AXIS,		// evValue is an axis number and evValue2 is the current state (-127 to 127)
	SE_CONSOLE				// evPtr is a char*, from typing something at a non-game console
} sysEventType_t;

typedef struct sysEvent_s {
	sysEventType_t	evType;
	int				evValue;
	int				evValue2;
	int				evPtrLength;
	void *			evPtr;
} sysEvent_t;

// DOOM-3 key numbers (neo/framework/KeyInput.h)
typedef enum {
	K_TAB = 9,
	K_ENTER = 13,
	K_ESCAPE = 27,
	K_SPACE = 32,
	K_BACKSPACE = 127,
	K_CAPSLOCK = 129,
	K_SCROLL,
	K_POWER,
	K_PAUSE,
	K_UPARROW = 133,
	K_DOWNARROW,
	K_LEFTARROW,
	K_RIGHTARROW,
	K_LWIN = 137,
	K_RWIN,
	K_MENU,
	K_ALT = 140,
	K_CTRL,
	K_SHIFT,
	K_INS,
	K_DEL,
	K_PGDN,
	K_PGUP,
	K_HOME,
	K_END,
	K_F1 = 149,
	K_KP_HOME = 160,
	K_KP_UPARROW,
	K_KP_PGUP,
	K_KP_LEFTARROW,
	K_KP_5,
	K_KP_RIGHTARROW,
	K_KP_END,
	K_KP_DOWNARROW,
	K_KP_PGDN,
	K_KP_ENTER,
	K_KP_INS,
	K_KP_DEL,
	K_KP_SLASH,
	K_SUPERSCRIPT_TWO = 178,
	K_KP_MINUS,
	K_ACUTE_ACCENT = 180,
	K_KP_PLUS,
	K_KP_NUMLOCK,
	K_KP_STAR,
	K_KP_EQUALS,
	K_MASCULINE_ORDINATOR = 186,
	K_MOUSE1 = 187,
	K_MOUSE2,
	K_MOUSE3,
	K_MOUSE4,
	K_MOUSE5,
	K_MOUSE6,
	K_MOUSE7,
	K_MOUSE8,
	K_MWHEELDOWN = 195,
	K_MWHEELUP,
	K_LAST_KEY = 254
} keyNum_t;

class idKeyInput {
public:
	static bool			IsDown( int keyNum ) { return false; }
	static const char *	KeysFromBinding( const char *bind ) { return ""; }
};

typedef enum {
	CVAR_ALL				= -1,
	CVAR_BOOL				= BIT(0),
	CVAR_INTEGER			= BIT(1),
	CVAR_FLOAT				= BIT(2),
	CVAR_SYSTEM				= BIT(3),
	CVAR_RENDERER			= BIT(4),
	CVAR_SOUND				= BIT(5),
	CVAR_GUI				= BIT(6),
	CVAR_GAME				= BIT(7),
	CVAR_TOOL				= BIT(8),
	CVAR_USERINFO			= BIT(9),
	CVAR_SERVERINFO			= BIT(10),
	CVAR_NETWORKSYNC		= BIT(11),
	CVAR_STATIC				= BIT(12),
	CVAR_CHEAT				= BIT(13),
	CVAR_NOCHEAT			= BIT(14),
	CVAR_INIT				= BIT(15),
	CVAR_ROM				= BIT(16),
	CVAR_ARCHIVE			= BIT(17),
	CVAR_MODIFIED			= BIT(30)
} cvarFlags_t;

// a module-private variable with DOOM-3's idCVar interface
class idCVar {
public:
						idCVar( const char *name, const char *value, int flags, const char *description ) {
							this->name = name; this->flags = flags; SetString( value );
						}
	const char *		GetName( void ) const { return name; }
	int					GetFlags( void ) const { return flags; }
	bool				IsModified( void ) const { return false; }
	void				SetModified( void ) {}
	void				ClearModified( void ) {}
	const char *		GetString( void ) const { return value; }
	bool				GetBool( void ) const { return integerValue != 0; }
	int					GetInteger( void ) const { return integerValue; }
	float				GetFloat( void ) const { return floatValue; }
	void				SetString( const char *v ) {
							int i;
							for ( i = 0; v && v[i] && i < (int)sizeof( value ) - 1; i++ ) {
								value[i] = v[i];
							}
							value[i] = '\0';
							floatValue = atof( value );
							integerValue = atoi( value );
						}
	void				SetBool( const bool v ) { SetString( v ? "1" : "0" ); }
	void				SetInteger( const int v ) { char b[16]; sprintf( b, "%d", v ); SetString( b ); }
	void				SetFloat( const float v ) { char b[32]; sprintf( b, "%f", v ); SetString( b ); }

private:
	const char *		name;
	int					flags;
	char				value[64];
	float				floatValue;
	int					integerValue;
};

// in-world GUIs never bind engine cvars: every lookup misses
class idCVarSystemLite {
public:
	idCVar *			Find( const char *name ) { return NULL; }
	const char *		GetCVarString( const char *name ) const { return ""; }
};
extern idCVarSystemLite *cvarSystem;

// neo/renderer/Material.h
const int MAX_EXPRESSION_REGISTERS = 4096;
const int MAX_EXPRESSION_OPS = 4096;

typedef enum {
	SS_SUBVIEW = -3,
	SS_GUI = -2,
	SS_BAD = -1,
	SS_OPAQUE,
	SS_PORTAL_SKY,
	SS_DECAL,
	SS_FAR,
	SS_MEDIUM,
	SS_CLOSE,
	SS_ALMOST_NEAREST,
	SS_NEAREST,
	SS_POST_PROCESS = 100
} materialSort_t;

typedef enum {
	MF_DEFAULTED				= BIT(0),
	MF_POLYGONOFFSET			= BIT(1),
	MF_NOSHADOWS				= BIT(2),
	MF_FORCESHADOWS				= BIT(3),
	MF_NOSELFSHADOW				= BIT(4),
	MF_NOPORTALFOG				= BIT(5),
	MF_EDITOR_VISIBLE			= BIT(6)
} materialFlags_t;

// a GUI material: a Q3 shader name, registered by the client on first draw
class idMaterial {
public:
						idMaterial( const char *name ) : name( name ), handle( 0 ), registered( false ) {}
	const char *		GetName( void ) const { return name.c_str(); }
	void				SetSort( float s ) const {}
	void				SetImageClassifications( int tag ) const {}
	bool				TestMaterialFlag( const int flag ) const { return false; }
	int					GetImageWidth( void ) const { return 64; }
	int					GetImageHeight( void ) const { return 64; }
	int					CinematicLength( void ) const { return 0; }
	void				UpdateCinematic( int time ) const {}
	void				ResetCinematicTime( int time ) const {}
	// the renderer's shader handle (client only; 0 when drawing is off)
	int					Handle( void ) const;
	void				Purge( void ) const { handle = 0; registered = false; }

private:
	idStr				name;
	mutable int			handle;
	mutable bool		registered;
};

class idDeclManagerLite {
public:
	const idMaterial *	FindMaterial( const char *name, bool makeDefault = true );
	void				PurgeMaterials( void );
	void				FreeMaterials( void );

private:
	idList<idMaterial *> materials;
};
extern idDeclManagerLite *declManager;

/*
	DOOM-3 savegame and demo files. The windows keep their save and demo
	code (a later phase can serialize GUI state through it), but nothing
	in the engine calls it yet: these files read and write nothing.
*/
class idFile {
public:
	virtual				~idFile( void ) {}
	virtual int			Read( void *buffer, int len ) { memset( buffer, 0, len ); return 0; }
	virtual int			Write( const void *buffer, int len ) { return 0; }
	virtual const char *GetName( void ) { return ""; }
};

class idDemoFile : public idFile {
public:
	void				SetLog( bool b, const char *p ) {}
	void				Log( const char *p ) {}
	void				ReadInt( int &i ) { i = 0; }
	void				ReadUnsignedInt( unsigned int &i ) { i = 0; }
	void				ReadShort( short &i ) { i = 0; }
	void				ReadUnsignedShort( unsigned short &i ) { i = 0; }
	void				ReadChar( char &c ) { c = 0; }
	void				ReadUnsignedChar( unsigned char &c ) { c = 0; }
	void				ReadFloat( float &f ) { f = 0.0f; }
	void				ReadBool( bool &b ) { b = false; }
	void				ReadDict( idDict &dict ) { dict.Clear(); }
	const char *		ReadHashString( void ) { return ""; }
	void				WriteInt( const int i ) {}
	void				WriteUnsignedInt( const unsigned int i ) {}
	void				WriteShort( const short s ) {}
	void				WriteUnsignedShort( unsigned short s ) {}
	void				WriteChar( const char c ) {}
	void				WriteUnsignedChar( const unsigned char c ) {}
	void				WriteFloat( const float f ) {}
	void				WriteBool( const bool b ) {}
	void				WriteDict( const idDict &dict ) {}
	void				WriteHashString( const char *str ) {}
};

#endif
