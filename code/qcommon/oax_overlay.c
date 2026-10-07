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
oax_overlay.c: the map sidecar merge (oax_overlay.h).
===========================================================================
*/
#include "q_shared.h"
#include "oax_overlay.h"

#define OVL_MAX_KEYS	256

typedef struct {
	char key[MAX_TOKEN_CHARS];
	char value[MAX_TOKEN_CHARS];
} ovlKey_t;

typedef struct {
	char	*out;
	int		size;
	int		len;
	char	last;			// the last byte written (the same in both passes)
} ovlOut_t;

static void Ovl_PutRaw( ovlOut_t *o, const char *s, int n ) {
	if ( n <= 0 ) {
		return;
	}
	if ( o->out && o->len + n < o->size ) {
		memcpy( o->out + o->len, s, n );
		o->out[o->len + n] = 0;
	}
	o->len += n;
	o->last = s[n - 1];
}

static void Ovl_Put( ovlOut_t *o, const char *s ) {
	Ovl_PutRaw( o, s, (int)strlen( s ) );
}

static void Ovl_PutKey( ovlOut_t *o, const char *key, const char *value ) {
	Ovl_Put( o, "\"" );
	Ovl_Put( o, key );
	Ovl_Put( o, "\" \"" );
	Ovl_Put( o, value );
	Ovl_Put( o, "\"\n" );
}

// parses one block after its opening brace: keys into keys[], returns the
// count, -1 on a malformed block; *p is left after the closing brace
static int Ovl_ParseBlock( const char **p, ovlKey_t *keys, int maxKeys ) {
	int n = 0;

	while ( 1 ) {
		char *tok = COM_ParseExt( (char **)p, qtrue );

		if ( !tok[0] ) {
			return -1;			// ran out inside a block
		}
		if ( tok[0] == '}' ) {
			return n;
		}
		if ( tok[0] == '{' ) {
			return -1;
		}
		if ( n >= maxKeys ) {
			return -1;
		}
		Q_strncpyz( keys[n].key, tok, sizeof( keys[n].key ) );
		tok = COM_ParseExt( (char **)p, qtrue );
		if ( !tok[0] || tok[0] == '}' || tok[0] == '{' ) {
			return -1;
		}
		Q_strncpyz( keys[n].value, tok, sizeof( keys[n].value ) );
		n++;
	}
}

static const char *Ovl_Find( const ovlKey_t *keys, int n, const char *key ) {
	int i;

	for ( i = 0; i < n; i++ ) {
		if ( !Q_stricmp( keys[i].key, key ) ) {
			return keys[i].value;
		}
	}
	return NULL;
}

int OAX_OverlayCountAdded( const char *overlay ) {
	static ovlKey_t keys[OVL_MAX_KEYS];
	const char *p = overlay;
	int added = 0;

	while ( 1 ) {
		char *tok = COM_ParseExt( (char **)&p, qtrue );
		int n;

		if ( !tok[0] ) {
			return added;
		}
		if ( tok[0] != '{' ) {
			return -1;
		}
		n = Ovl_ParseBlock( &p, keys, OVL_MAX_KEYS );
		if ( n < 0 ) {
			return -1;
		}
		if ( Q_stricmp( Ovl_Find( keys, n, "classname" ) ? Ovl_Find( keys, n, "classname" ) : "", "worldspawn" ) ) {
			added++;
		}
	}
}

int OAX_OverlayMerge( const char *base, int baseLen, const char *overlay, char *out, int outSize ) {
	static ovlKey_t baseKeys[OVL_MAX_KEYS], keys[OVL_MAX_KEYS];
	static ovlKey_t wsKeys[OVL_MAX_KEYS];
	ovlOut_t o;
	const char *bp, *op, *rest;
	int nBase, nWs = 0, i, k, n, haveBase = 0;

	o.out = out;
	o.size = outSize;
	o.len = 0;
	o.last = '\n';
	if ( out && outSize > 0 ) {
		out[0] = 0;
	}
	if ( !overlay || OAX_OverlayCountAdded( overlay ) < 0 ) {
		return -1;
	}

	// the map's entity string up to its first block (worldspawn)
	if ( baseLen > 0 && base ) {
		bp = base;
		if ( COM_ParseExt( (char **)&bp, qtrue )[0] == '{' ) {
			nBase = Ovl_ParseBlock( &bp, baseKeys, OVL_MAX_KEYS );
			if ( nBase < 0 ) {
				return -1;
			}
			haveBase = 1;
			rest = bp;			// everything after worldspawn's closing brace
		} else {
			return -1;
		}
	} else {
		nBase = 0;
		rest = "";
	}

	// the overlay's worldspawn keys, merged over the base's
	for ( i = 0; i < nBase; i++ ) {
		wsKeys[i] = baseKeys[i];
	}
	nWs = nBase;
	op = overlay;
	while ( 1 ) {
		char *tok = COM_ParseExt( (char **)&op, qtrue );
		const char *cn;

		if ( !tok[0] ) {
			break;
		}
		n = Ovl_ParseBlock( &op, keys, OVL_MAX_KEYS );
		cn = Ovl_Find( keys, n, "classname" );
		if ( cn && !Q_stricmp( cn, "worldspawn" ) ) {
			for ( k = 0; k < n; k++ ) {
				int j;

				if ( !Q_stricmp( keys[k].key, "classname" ) ) {
					continue;
				}
				for ( j = 0; j < nWs && Q_stricmp( wsKeys[j].key, keys[k].key ); j++ ) {
				}
				if ( j == nWs ) {
					if ( nWs >= OVL_MAX_KEYS ) {
						return -1;
					}
					nWs++;
				}
				wsKeys[j] = keys[k];
			}
		}
	}

	// write: worldspawn, the map's remaining entities, the overlay's others
	if ( haveBase || nWs > 0 ) {
		Ovl_Put( &o, "{\n" );
		for ( i = 0; i < nWs; i++ ) {
			Ovl_PutKey( &o, wsKeys[i].key, wsKeys[i].value );
		}
		Ovl_Put( &o, "}\n" );
	}
	{
		// the rest of the map's own text, as it is (skip the whitespace
		// after the worldspawn block)
		const char *r = rest;

		while ( *r == ' ' || *r == '\t' || *r == '\r' || *r == '\n' ) {
			r++;
		}
		if ( *r && ( baseLen <= 0 || r - base < baseLen ) ) {
			int len = baseLen > 0 ? baseLen - (int)( r - base ) : (int)strlen( r );
			int actual = (int)strlen( r );

			if ( actual < len ) {
				len = actual;
			}
			Ovl_PutRaw( &o, r, len );
		}
	}
	op = overlay;
	while ( 1 ) {
		char *tok = COM_ParseExt( (char **)&op, qtrue );
		const char *cn;

		if ( !tok[0] ) {
			break;
		}
		n = Ovl_ParseBlock( &op, keys, OVL_MAX_KEYS );
		cn = Ovl_Find( keys, n, "classname" );
		if ( cn && !Q_stricmp( cn, "worldspawn" ) ) {
			continue;
		}
		if ( o.last != '\n' ) {
			Ovl_Put( &o, "\n" );
		}
		Ovl_Put( &o, "{\n" );
		for ( k = 0; k < n; k++ ) {
			Ovl_PutKey( &o, keys[k].key, keys[k].value );
		}
		Ovl_Put( &o, "}\n" );
	}
	return o.len;
}

void OAX_OverlayPath( const char *bspName, char *out, int outSize ) {
	char *dot;

	Q_strncpyz( out, bspName, outSize );
	dot = strrchr( out, '.' );
	if ( dot && !strchr( dot, '/' ) ) {
		*dot = 0;
	}
	Q_strcat( out, outSize, OAX_OVERLAY_EXT );
}
