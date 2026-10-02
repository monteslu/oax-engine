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
tr_image_program.c: load-time image programs.

Adapted from DOOM-3 neo/renderer/Image_program.cpp (and R_Dropsample from
neo/renderer/Image_process.cpp). Changes:
- C instead of C++: idLexer is replaced by a small tokenizer over the
  program string, idVec3 by float[3], R_StaticAlloc by ri.Malloc.
- Images load through renderergl2's R_LoadImage; only RGBA8 sources are
  accepted (no compressed DDS).
- No timestamps (no reloadImages), no textureDepth_t.
- idMath::RSqrt (heightmap) is kept bit for bit; idVec3::Normalize
  (addnormals, smoothnormals) uses 1.0f / sqrtf instead of id's table
  InvSqrt, so the result is the same on every host.
- scale() clamps before converting to a byte (D3 converted first, which is
  undefined behaviour above 255).
- Wired into R_FindImageFile: any image name containing '(' is a program.
  Names too long for an image_t are stored under a hashed key.
- `imageprogram <program>` (cheat) runs a program and publishes the size
  and an FNV-1a hash of the result as debug values, for tests.

Programs: heightmap(img, scale), addnormals(img, img), smoothnormals(img),
add(img, img), scale(img, r, g, b, a), invertAlpha(img), invertColor(img),
makeIntensity(img), makeAlpha(img); img is a path or a nested program.
*/

#include "tr_local.h"

void R_LoadImage( const char *name, byte **pic, int *width, int *height, GLenum *picFormat, int *numMips );

#define IP_MAX_TOKEN	MAX_QPATH

typedef struct {
	const char	*p;
	char		token[IP_MAX_TOKEN];
	qboolean	error;
} ipLexer_t;

// one token: ( ) , or a run of anything else up to whitespace or those
static const char *IP_ReadToken( ipLexer_t *lex ) {
	int n = 0;

	while ( *lex->p && ( *lex->p == ' ' || *lex->p == '\t' || *lex->p == '\n' || *lex->p == '\r' ) ) {
		lex->p++;
	}
	if ( *lex->p == '(' || *lex->p == ')' || *lex->p == ',' ) {
		lex->token[0] = *lex->p++;
		lex->token[1] = '\0';
		return lex->token;
	}
	while ( *lex->p && !strchr( " \t\n\r(),", *lex->p ) ) {
		if ( n < IP_MAX_TOKEN - 1 ) {
			lex->token[n++] = *lex->p;
		}
		lex->p++;
	}
	lex->token[n] = '\0';
	return lex->token;
}

static void IP_Expect( ipLexer_t *lex, const char *match ) {
	if ( strcmp( IP_ReadToken( lex ), match ) ) {
		ri.Printf( PRINT_WARNING, "WARNING: image program: expected '%s', found '%s'\n", match, lex->token );
		lex->error = qtrue;
	}
}

static byte *IP_Alloc( int size ) {
	return ri.Malloc( size );
}

/*
================
R_Dropsample

Used to resample images in a more general than quartering fashion.
Normal maps and such should not be bilerped.
================
*/
static byte *R_Dropsample( const byte *in, int inwidth, int inheight, int outwidth, int outheight ) {
	int		i, j, k;
	const byte	*inrow;
	const byte	*pix1;
	byte		*out, *out_p;

	out = IP_Alloc( outwidth * outheight * 4 );
	out_p = out;

	for (i=0 ; i<outheight ; i++, out_p += outwidth*4 ) {
		inrow = in + 4*inwidth*(int)((i+0.25)*inheight/outheight);
		for (j=0 ; j<outwidth ; j++) {
			k = j * inwidth / outwidth;
			pix1 = inrow + k * 4;
			out_p[j*4+0] = pix1[0];
			out_p[j*4+1] = pix1[1];
			out_p[j*4+2] = pix1[2];
			out_p[j*4+3] = pix1[3];
		}
	}

	return out;
}

// idMath::RSqrt: reciprocal square root, one Newton step
static float IP_RSqrt( float x ) {
	union { float f; int32_t i; } u;
	float y, r;

	y = x * 0.5f;
	u.f = x;
	u.i = 0x5f3759df - ( u.i >> 1 );
	r = u.f;
	r = r * ( 1.5f - r * r * y );
	return r;
}

// idVec3::NormalizeFast
static void IP_NormalizeFast( float v[3] ) {
	float sqrLength, invLength;

	sqrLength = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	invLength = IP_RSqrt( sqrLength );
	v[0] *= invLength;
	v[1] *= invLength;
	v[2] *= invLength;
}

// idVec3::Normalize, with an exact inverse square root
static void IP_Normalize( float v[3] ) {
	float sqrLength, invLength;

	sqrLength = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
	if ( sqrLength <= 0.0f ) {
		return;
	}
	invLength = 1.0f / sqrtf( sqrLength );
	v[0] *= invLength;
	v[1] *= invLength;
	v[2] *= invLength;
}

/*
=================
R_HeightmapToNormalMap

it is not possible to convert a heightmap into a normal map
properly without knowing the texture coordinate stretching.
We can assume constant and equal ST vectors for walls, but not for characters.
=================
*/
static void R_HeightmapToNormalMap( byte *data, int width, int height, float scale ) {
	int		i, j;
	byte	*depth;
	float	dir[3], dir2[3];

	scale = scale / 256;

	// copy and convert to grey scale
	j = width * height;
	depth = IP_Alloc( j );
	for ( i = 0 ; i < j ; i++ ) {
		depth[i] = ( data[i*4] + data[i*4+1] + data[i*4+2] ) / 3;
	}

	for ( i = 0 ; i < height ; i++ ) {
		for ( j = 0 ; j < width ; j++ ) {
			int		d1, d2, d3, d4;
			int		a1, a2, a3, a4;

			// FIXME: look at five points?

			// look at three points to estimate the gradient
			a1 = d1 = depth[ ( i * width + j ) ];
			a2 = d2 = depth[ ( i * width + ( ( j + 1 ) & ( width - 1 ) ) ) ];
			a3 = d3 = depth[ ( ( ( i + 1 ) & ( height - 1 ) ) * width + j ) ];
			a4 = d4 = depth[ ( ( ( i + 1 ) & ( height - 1 ) ) * width + ( ( j + 1 ) & ( width - 1 ) ) ) ];
			(void)a2; (void)d4;

			d2 -= d1;
			d3 -= d1;

			dir[0] = -d2 * scale;
			dir[1] = -d3 * scale;
			dir[2] = 1;
			IP_NormalizeFast( dir );

			a1 -= a3;
			a4 -= a3;

			dir2[0] = -a4 * scale;
			dir2[1] = a1 * scale;
			dir2[2] = 1;
			IP_NormalizeFast( dir2 );

			dir[0] += dir2[0];
			dir[1] += dir2[1];
			dir[2] += dir2[2];
			IP_NormalizeFast( dir );

			a1 = ( i * width + j ) * 4;
			data[ a1 + 0 ] = (byte)(dir[0] * 127 + 128);
			data[ a1 + 1 ] = (byte)(dir[1] * 127 + 128);
			data[ a1 + 2 ] = (byte)(dir[2] * 127 + 128);
			data[ a1 + 3 ] = 255;
		}
	}

	ri.Free( depth );
}

/*
=================
R_ImageScale
=================
*/
static void R_ImageScale( byte *data, int width, int height, float scale[4] ) {
	int		i;
	int		c;
	float	v;

	c = width * height * 4;

	for ( i = 0 ; i < c ; i++ ) {
		v = data[i] * scale[i&3];
		if ( v < 0 ) {
			v = 0;
		} else if ( v > 255 ) {
			v = 255;
		}
		data[i] = (byte)v;
	}
}

/*
=================
R_InvertAlpha
=================
*/
static void R_InvertAlpha( byte *data, int width, int height ) {
	int		i;
	int		c;

	c = width * height* 4;

	for ( i = 0 ; i < c ; i+=4 ) {
		data[i+3] = 255 - data[i+3];
	}
}

/*
=================
R_InvertColor
=================
*/
static void R_InvertColor( byte *data, int width, int height ) {
	int		i;
	int		c;

	c = width * height* 4;

	for ( i = 0 ; i < c ; i+=4 ) {
		data[i+0] = 255 - data[i+0];
		data[i+1] = 255 - data[i+1];
		data[i+2] = 255 - data[i+2];
	}
}

/*
===================
R_AddNormalMaps
===================
*/
static void R_AddNormalMaps( byte *data1, int width1, int height1, byte *data2, int width2, int height2 ) {
	int		i, j;
	byte	*newMap;

	// resample pic2 to the same size as pic1
	if ( width2 != width1 || height2 != height1 ) {
		newMap = R_Dropsample( data2, width2, height2, width1, height1 );
		data2 = newMap;
	} else {
		newMap = NULL;
	}

	// add the normal change from the second and renormalize
	for ( i = 0 ; i < height1 ; i++ ) {
		for ( j = 0 ; j < width1 ; j++ ) {
			byte	*d1, *d2;
			float	n[3];
			float   len;

			d1 = data1 + ( i * width1 + j ) * 4;
			d2 = data2 + ( i * width1 + j ) * 4;

			n[0] = ( d1[0] - 128 ) / 127.0;
			n[1] = ( d1[1] - 128 ) / 127.0;
			n[2] = ( d1[2] - 128 ) / 127.0;

			// There are some normal maps that blend to 0,0,0 at the edges
			// this screws up compression, so we try to correct that here by instead fading it to 0,0,1
			{
				float sqrLength = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
				len = sqrLength * IP_RSqrt( sqrLength );	// idVec3::LengthFast
			}
			if ( len < 1.0f ) {
				float s = 1.0f - ( n[0] * n[0] ) - ( n[1] * n[1] );
				n[2] = s > 0.0f ? sqrtf( s ) : 0.0f;
			}

			n[0] += ( d2[0] - 128 ) / 127.0;
			n[1] += ( d2[1] - 128 ) / 127.0;
			IP_Normalize( n );

			d1[0] = (byte)(n[0] * 127 + 128);
			d1[1] = (byte)(n[1] * 127 + 128);
			d1[2] = (byte)(n[2] * 127 + 128);
			d1[3] = 255;
		}
	}

	if ( newMap ) {
		ri.Free( newMap );
	}
}

/*
================
R_SmoothNormalMap
================
*/
static void R_SmoothNormalMap( byte *data, int width, int height ) {
	byte	*orig;
	int		i, j, k, l;
	float	normal[3];
	byte	*out;
	static float	factors[3][3] = {
		{ 1, 1, 1 },
		{ 1, 1, 1 },
		{ 1, 1, 1 }
	};

	orig = IP_Alloc( width * height * 4 );
	memcpy( orig, data, width * height * 4 );

	for ( i = 0 ; i < width ; i++ ) {
		for ( j = 0 ; j < height ; j++ ) {
			VectorClear( normal );
			for ( k = -1 ; k < 2 ; k++ ) {
				for ( l = -1 ; l < 2 ; l++ ) {
					byte	*in;

					in = orig + ( ((j+l)&(height-1))*width + ((i+k)&(width-1)) ) * 4;

					// ignore 000 and -1 -1 -1
					if ( in[0] == 0 && in[1] == 0 && in[2] == 0 ) {
						continue;
					}
					if ( in[0] == 128 && in[1] == 128 && in[2] == 128 ) {
						continue;
					}

					normal[0] += factors[k+1][l+1] * ( in[0] - 128 );
					normal[1] += factors[k+1][l+1] * ( in[1] - 128 );
					normal[2] += factors[k+1][l+1] * ( in[2] - 128 );
				}
			}
			IP_Normalize( normal );
			out = data + ( j * width + i ) * 4;
			out[0] = (byte)(128 + 127 * normal[0]);
			out[1] = (byte)(128 + 127 * normal[1]);
			out[2] = (byte)(128 + 127 * normal[2]);
		}
	}

	ri.Free( orig );
}

/*
===================
R_ImageAdd
===================
*/
static void R_ImageAdd( byte *data1, int width1, int height1, byte *data2, int width2, int height2 ) {
	int		i, j;
	int		c;
	byte	*newMap;

	// resample pic2 to the same size as pic1
	if ( width2 != width1 || height2 != height1 ) {
		newMap = R_Dropsample( data2, width2, height2, width1, height1 );
		data2 = newMap;
	} else {
		newMap = NULL;
	}

	c = width1 * height1 * 4;

	for ( i = 0 ; i < c ; i++ ) {
		j = data1[i] + data2[i];
		if ( j > 255 ) {
			j = 255;
		}
		data1[i] = j;
	}

	if ( newMap ) {
		ri.Free( newMap );
	}
}

static void IP_Free( byte **pic ) {
	if ( pic && *pic ) {
		ri.Free( *pic );
		*pic = NULL;
	}
}

/*
===================
R_ParseImageProgram_r

If pic is NULL the program is only parsed.
===================
*/
static qboolean R_ParseImageProgram_r( ipLexer_t *src, byte **pic, int *width, int *height ) {
	char	token[IP_MAX_TOKEN];
	float	scale;

	Q_strncpyz( token, IP_ReadToken( src ), sizeof( token ) );
	if ( !token[0] ) {
		src->error = qtrue;
		return qfalse;
	}

	if ( !Q_stricmp( token, "heightmap" ) ) {
		IP_Expect( src, "(" );
		if ( !R_ParseImageProgram_r( src, pic, width, height ) ) {
			return qfalse;
		}
		IP_Expect( src, "," );
		scale = atof( IP_ReadToken( src ) );
		if ( pic ) {
			R_HeightmapToNormalMap( *pic, *width, *height, scale );
		}
		IP_Expect( src, ")" );
		return !src->error;
	}

	if ( !Q_stricmp( token, "addnormals" ) || !Q_stricmp( token, "add" ) ) {
		byte	*pic2 = NULL;
		int		width2, height2;
		qboolean normals = !Q_stricmp( token, "addnormals" );

		IP_Expect( src, "(" );
		if ( !R_ParseImageProgram_r( src, pic, width, height ) ) {
			return qfalse;
		}
		IP_Expect( src, "," );
		if ( !R_ParseImageProgram_r( src, pic ? &pic2 : NULL, &width2, &height2 ) ) {
			IP_Free( pic );
			return qfalse;
		}
		if ( pic ) {
			if ( normals ) {
				R_AddNormalMaps( *pic, *width, *height, pic2, width2, height2 );
			} else {
				R_ImageAdd( *pic, *width, *height, pic2, width2, height2 );
			}
			IP_Free( &pic2 );
		}
		IP_Expect( src, ")" );
		return !src->error;
	}

	if ( !Q_stricmp( token, "smoothnormals" ) ) {
		IP_Expect( src, "(" );
		if ( !R_ParseImageProgram_r( src, pic, width, height ) ) {
			return qfalse;
		}
		if ( pic ) {
			R_SmoothNormalMap( *pic, *width, *height );
		}
		IP_Expect( src, ")" );
		return !src->error;
	}

	if ( !Q_stricmp( token, "scale" ) ) {
		float	s[4];
		int		i;

		IP_Expect( src, "(" );
		if ( !R_ParseImageProgram_r( src, pic, width, height ) ) {
			return qfalse;
		}
		for ( i = 0 ; i < 4 ; i++ ) {
			IP_Expect( src, "," );
			s[i] = atof( IP_ReadToken( src ) );
		}
		if ( pic ) {
			R_ImageScale( *pic, *width, *height, s );
		}
		IP_Expect( src, ")" );
		return !src->error;
	}

	if ( !Q_stricmp( token, "invertAlpha" ) || !Q_stricmp( token, "invertColor" )
		|| !Q_stricmp( token, "makeIntensity" ) || !Q_stricmp( token, "makeAlpha" ) ) {
		IP_Expect( src, "(" );
		if ( !R_ParseImageProgram_r( src, pic, width, height ) ) {
			return qfalse;
		}
		if ( pic ) {
			int		i, c = *width * *height * 4;
			byte	*p = *pic;

			if ( !Q_stricmp( token, "invertAlpha" ) ) {
				R_InvertAlpha( p, *width, *height );
			} else if ( !Q_stricmp( token, "invertColor" ) ) {
				R_InvertColor( p, *width, *height );
			} else if ( !Q_stricmp( token, "makeIntensity" ) ) {
				// copy red to green, blue, and alpha
				for ( i = 0 ; i < c ; i+=4 ) {
					p[i+1] = p[i+2] = p[i+3] = p[i];
				}
			} else {
				// average RGB into alpha, then set RGB to white
				for ( i = 0 ; i < c ; i+=4 ) {
					p[i+3] = ( p[i+0] + p[i+1] + p[i+2] ) / 3;
					p[i+0] = p[i+1] = p[i+2] = 255;
				}
			}
		}
		IP_Expect( src, ")" );
		return !src->error;
	}

	// if we are just parsing, don't load the image
	if ( !pic ) {
		return qtrue;
	}

	// load it as an image
	{
		GLenum	format;
		int		numMips;

		R_LoadImage( token, pic, width, height, &format, &numMips );
		if ( !*pic ) {
			ri.Printf( PRINT_WARNING, "WARNING: image program: couldn't load '%s'\n", token );
			return qfalse;
		}
		if ( format != GL_RGBA8 ) {
			ri.Printf( PRINT_WARNING, "WARNING: image program: '%s' is not an RGBA8 image\n", token );
			IP_Free( pic );
			return qfalse;
		}
	}
	return qtrue;
}

qboolean R_OAXIsImageProgram( const char *name ) {
	return name && strchr( name, '(' ) != NULL;
}

static unsigned IP_Fnv1a( const byte *data, int len ) {
	unsigned h = 2166136261u;
	int i;

	for ( i = 0; i < len; i++ ) {
		h ^= data[i];
		h *= 16777619u;
	}
	return h;
}

/*
===================
R_OAXImageProgramKey

The image_t name for a program: the program itself when it fits, else a
hash of it.
===================
*/
const char *R_OAXImageProgramKey( const char *name ) {
	static char key[MAX_QPATH];

	if ( strlen( name ) < MAX_QPATH ) {
		return name;
	}
	Com_sprintf( key, sizeof( key ), "_imageprogram_%08x", IP_Fnv1a( (const byte *)name, strlen( name ) ) );
	return key;
}

/*
===================
R_OAXLoadImageProgram

Runs an image program; *pic is ri.Malloc'd RGBA8 (NULL on failure).
===================
*/
qboolean R_OAXLoadImageProgram( const char *name, byte **pic, int *width, int *height ) {
	ipLexer_t lex;

	Com_Memset( &lex, 0, sizeof( lex ) );
	lex.p = name;
	*pic = NULL;
	*width = *height = 0;
	if ( !R_ParseImageProgram_r( &lex, pic, width, height ) || lex.error ) {
		IP_Free( pic );
		return qfalse;
	}
	return *pic != NULL;
}

/*
===================
R_OAXParseImageName

Shader stages read an image name as one token; a program written with
spaces ("heightmap(textures/a.tga, 4)") spans several. Joins tokens from
the same line until the parentheses balance.
===================
*/
const char *R_OAXParseImageName( const char *token, char **text ) {
	static char buf[MAX_STRING_CHARS];
	int depth = 0;
	const char *c;

	Q_strncpyz( buf, token, sizeof( buf ) );
	for ( c = buf; *c; c++ ) {
		depth += ( *c == '(' ) - ( *c == ')' );
	}
	while ( depth > 0 ) {
		const char *next = COM_ParseExt( text, qfalse );

		if ( !next[0] ) {
			break;
		}
		Q_strcat( buf, sizeof( buf ), " " );
		Q_strcat( buf, sizeof( buf ), next );
		for ( c = next; *c; c++ ) {
			depth += ( *c == '(' ) - ( *c == ')' );
		}
	}
	return buf;
}

/*
===================
R_OAXImageProgram_f

imageprogram <program ...>: runs it and publishes imageprogram_size and
imageprogram_hash (FNV-1a of the RGBA bytes) as debug values.
===================
*/
void R_OAXImageProgram_f( void ) {
	char	program[MAX_STRING_CHARS];
	byte	*pic;
	int		w, h, i;

	program[0] = '\0';
	for ( i = 1; i < ri.Cmd_Argc(); i++ ) {
		if ( i > 1 ) {
			Q_strcat( program, sizeof( program ), " " );
		}
		Q_strcat( program, sizeof( program ), ri.Cmd_Argv( i ) );
	}
	if ( !R_OAXLoadImageProgram( program, &pic, &w, &h ) ) {
		ri.Printf( PRINT_ALL, "imageprogram: '%s' failed\n", program );
		ri.Cvar_Set( "r_imageprogram_result", "failed" );
		return;
	}
	ri.Printf( PRINT_ALL, "imageprogram: %dx%d hash %08x\n", w, h, IP_Fnv1a( pic, w * h * 4 ) );
	ri.Cvar_Set( "r_imageprogram_result", va( "%dx%d %08x", w, h, IP_Fnv1a( pic, w * h * 4 ) ) );
	ri.Free( pic );
}
