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
Adapted from DOOM-3 neo/idlib/Dict.h for the oa-engine GUI module.
Changes: keys and values are plain idStr pairs in insertion order (no
idStrPool, no hash index: a GUI state dictionary holds a few dozen keys),
and only the accessors the GUI port uses. Insertion order is kept so the
state the server syncs to clients is listed the same way on every build.
Lives in namespace idgui.
*/

#ifndef __IDGUI_DICT_H__
#define __IDGUI_DICT_H__

class idKeyValue {
	friend class idDict;
public:
	const idStr &		GetKey( void ) const { return key; }
	const idStr &		GetValue( void ) const { return value; }

private:
	idStr				key;
	idStr				value;
};

class idDict {
public:
						idDict( void ) { args.SetGranularity( 16 ); }

	void				Clear( void ) { args.Clear(); }
	void				Copy( const idDict &other ) { args = other.args; }
	int					GetNumKeyVals( void ) const { return args.Num(); }
	const idKeyValue *	GetKeyVal( int index ) const { return ( index >= 0 && index < args.Num() ) ? &args[ index ] : NULL; }

	void				Set( const char *key, const char *value );
	void				SetFloat( const char *key, float val ) { Set( key, va( "%f", val ) ); }
	void				SetInt( const char *key, int val ) { Set( key, va( "%i", val ) ); }
	void				SetBool( const char *key, bool val ) { Set( key, va( "%i", val ) ); }
	void				SetVector( const char *key, const idVec3 &val ) { Set( key, val.ToString() ); }
	void				SetVec2( const char *key, const idVec2 &val ) { Set( key, val.ToString() ); }
	void				SetVec4( const char *key, const idVec4 &val ) { Set( key, val.ToString() ); }

	const char *		GetString( const char *key, const char *defaultString = "" ) const {
							const idKeyValue *kv = FindKey( key );
							return kv ? kv->GetValue().c_str() : defaultString;
						}
	bool				GetString( const char *key, const char *defaultString, idStr &out ) const {
							const idKeyValue *kv = FindKey( key );
							out = kv ? kv->GetValue().c_str() : defaultString;
							return kv != NULL;
						}
	size_t				Size( void ) const { return sizeof( *this ) + args.Num() * sizeof( idKeyValue ); }
	float				GetFloat( const char *key, const char *defaultString = "0" ) const { return atof( GetString( key, defaultString ) ); }
	int					GetInt( const char *key, const char *defaultString = "0" ) const { return atoi( GetString( key, defaultString ) ); }
	bool				GetBool( const char *key, const char *defaultString = "0" ) const { return atoi( GetString( key, defaultString ) ) != 0; }
	idVec3				GetVector( const char *key, const char *defaultString = NULL ) const;
	idVec2				GetVec2( const char *key, const char *defaultString = NULL ) const;
	idVec4				GetVec4( const char *key, const char *defaultString = NULL ) const;

	const idKeyValue *	FindKey( const char *key ) const;
	int					FindKeyIndex( const char *key ) const;
	void				Delete( const char *key );
	const idKeyValue *	MatchPrefix( const char *prefix, const idKeyValue *lastMatch = NULL ) const;

private:
	idList<idKeyValue>	args;
};

typedef idList<idStr> idStrList;

#endif
