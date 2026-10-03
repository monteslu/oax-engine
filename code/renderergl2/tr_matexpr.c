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
tr_matexpr.c: id Tech 4 material extensions for Q3 shader scripts.

Adapted from DOOM-3 neo/renderer/Material.cpp (ParseTerm,
ParseExpressionPriority, EmitOp, GetExpressionConstant,
GetExpressionTemporary, EvaluateRegisters, the diffusemap / bumpmap /
specularmap stage shorthands, the rgb / red / green / blue / alpha / rgba /
color stage keywords and the light material keywords) and from
neo/framework/DeclTable.cpp (idDeclTable::Parse and TableLookup).
Changes: ported from C++ to C; expressions are parsed from the rest of a
Q3 shader line with a small lexer instead of idLexer; registers live in a
per-shader matExpr_t; tables are read from scripts/<name>.table; the per-frame
result is written into the stage's constant color so the stock Q3 stage
code draws it unchanged; the sound register reads 0.
*/

#include "tr_local.h"

// ---------------------------------------------------------------------------
// expression operators (Material.h expOpType_t)

enum {
	MOP_ADD, MOP_SUBTRACT, MOP_MULTIPLY, MOP_DIVIDE, MOP_MOD, MOP_TABLE,
	MOP_GT, MOP_GE, MOP_LT, MOP_LE, MOP_EQ, MOP_NE, MOP_AND, MOP_OR, MOP_SOUND
};

// ---------------------------------------------------------------------------
// tables (DeclTable.cpp)

#define MAX_TABLES        256
#define MAX_TABLE_VALUES  1024

typedef struct {
	char     name[MAX_QPATH];
	qboolean snap;
	qboolean clamp;
	int      numValues;     // including the duplicated first value at the end
	float   *values;
} matTable_t;

static matTable_t tables[MAX_TABLES];
static int        numTables;

int R_FindTable( const char *name ) {
	int i;

	for ( i = 0; i < numTables; i++ ) {
		if ( !Q_stricmp( tables[i].name, name ) ) {
			return i;
		}
	}
	return -1;
}

/*
=================
R_TableLookup

idDeclTable::TableLookup
=================
*/
float R_TableLookup( int table, float index ) {
	const matTable_t *t;
	int iIndex;
	float iFrac;
	int domain;

	if ( table < 0 || table >= numTables ) {
		return 1.0f;
	}
	t = &tables[table];
	domain = t->numValues - 1;
	if ( domain <= 1 ) {
		return 1.0f;
	}

	if ( t->clamp ) {
		index *= ( domain - 1 );
		if ( index >= domain - 1 ) {
			return t->values[domain - 1];
		} else if ( index <= 0 ) {
			return t->values[0];
		}
		iIndex = (int)index;
		iFrac = index - iIndex;
	} else {
		index *= domain;
		if ( index < 0 ) {
			index += domain * ceil( -index / domain );
		}
		iIndex = (int)floor( index );
		iFrac = index - iIndex;
		iIndex = iIndex % domain;
	}

	if ( !t->snap ) {
		// the 0 index is duplicated at the end, so lerping needs no wrap
		return t->values[iIndex] * ( 1.0f - iFrac ) + t->values[iIndex + 1] * iFrac;
	}

	return t->values[iIndex];
}

// ---------------------------------------------------------------------------
// a small lexer for expressions and tables: numbers, names and the
// operators of Material.cpp

typedef struct {
	const char *p;
	const char *end;
	char        tok[MAX_QPATH];
	qboolean    isNumber;
	qboolean    pushed;
	qboolean    error;
} matLex_t;

static void Lex_Init( matLex_t *lx, const char *start, const char *end ) {
	Com_Memset( lx, 0, sizeof( *lx ) );
	lx->p = start;
	lx->end = end;
}

static qboolean Lex_Next( matLex_t *lx ) {
	const char *p;
	int n = 0;

	if ( lx->pushed ) {
		lx->pushed = qfalse;
		return lx->tok[0] != 0;
	}

	p = lx->p;
	lx->tok[0] = 0;
	lx->isNumber = qfalse;

	for ( ;; ) {
		while ( p < lx->end && ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) ) {
			p++;
		}
		// comments
		if ( p + 1 < lx->end && p[0] == '/' && p[1] == '/' ) {
			while ( p < lx->end && *p != '\n' ) {
				p++;
			}
			continue;
		}
		if ( p + 1 < lx->end && p[0] == '/' && p[1] == '*' ) {
			p += 2;
			while ( p + 1 < lx->end && !( p[0] == '*' && p[1] == '/' ) ) {
				p++;
			}
			p += 2;
			continue;
		}
		break;
	}
	if ( p >= lx->end ) {
		lx->p = p;
		return qfalse;
	}

	if ( ( *p >= '0' && *p <= '9' ) || ( *p == '.' && p + 1 < lx->end && p[1] >= '0' && p[1] <= '9' ) ) {
		while ( p < lx->end && ( ( *p >= '0' && *p <= '9' ) || *p == '.' ) && n < MAX_QPATH - 1 ) {
			lx->tok[n++] = *p++;
		}
		lx->isNumber = qtrue;
	} else if ( ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) || *p == '_' || *p == '$' ) {
		while ( p < lx->end && ( ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) || ( *p >= '0' && *p <= '9' )
			|| *p == '_' || *p == '$' || *p == '/' || *p == '.' ) && n < MAX_QPATH - 1 ) {
			lx->tok[n++] = *p++;
		}
	} else {
		static const char *two[] = { ">=", "<=", "==", "!=", "&&", "||" };
		int i;

		for ( i = 0; i < 6; i++ ) {
			if ( p + 1 < lx->end && p[0] == two[i][0] && p[1] == two[i][1] ) {
				lx->tok[n++] = *p++;
				lx->tok[n++] = *p++;
				break;
			}
		}
		if ( !n ) {
			lx->tok[n++] = *p++;
		}
	}
	lx->tok[n] = 0;
	lx->p = p;
	return qtrue;
}

static void Lex_Unread( matLex_t *lx ) {
	lx->pushed = qtrue;
}

// ---------------------------------------------------------------------------
// table declarations: table <name> { [snap] [clamp] { v, v, ... } }

static void R_ParseTables( const char *text, int len, const char *file ) {
	matLex_t lx;
	float values[MAX_TABLE_VALUES];

	Lex_Init( &lx, text, text + len );
	while ( Lex_Next( &lx ) ) {
		matTable_t t;
		int n = 0;
		qboolean ok = qtrue;

		if ( Q_stricmp( lx.tok, "table" ) ) {
			ri.Printf( PRINT_WARNING, "WARNING: %s: expected 'table', found '%s'\n", file, lx.tok );
			return;
		}
		Com_Memset( &t, 0, sizeof( t ) );
		if ( !Lex_Next( &lx ) ) {
			return;
		}
		Q_strncpyz( t.name, lx.tok, sizeof( t.name ) );
		if ( !Lex_Next( &lx ) || strcmp( lx.tok, "{" ) ) {
			ri.Printf( PRINT_WARNING, "WARNING: %s: table %s: expected '{'\n", file, t.name );
			return;
		}
		while ( Lex_Next( &lx ) ) {
			if ( !strcmp( lx.tok, "}" ) ) {
				break;
			}
			if ( !Q_stricmp( lx.tok, "snap" ) ) {
				t.snap = qtrue;
			} else if ( !Q_stricmp( lx.tok, "clamp" ) ) {
				t.clamp = qtrue;
			} else if ( !strcmp( lx.tok, "{" ) ) {
				while ( Lex_Next( &lx ) ) {
					float sign = 1.0f;

					if ( !strcmp( lx.tok, "-" ) ) {
						sign = -1.0f;
						Lex_Next( &lx );
					}
					if ( !lx.isNumber ) {
						ok = qfalse;
						break;
					}
					if ( n < MAX_TABLE_VALUES - 1 ) {
						values[n++] = sign * atof( lx.tok );
					}
					if ( !Lex_Next( &lx ) || !strcmp( lx.tok, "}" ) ) {
						break;
					}
					if ( strcmp( lx.tok, "," ) ) {
						ok = qfalse;
						break;
					}
				}
			} else {
				ok = qfalse;
			}
			if ( !ok ) {
				break;
			}
		}
		if ( !ok || !n ) {
			ri.Printf( PRINT_WARNING, "WARNING: %s: bad table %s, using { 0 }\n", file, t.name );
			values[0] = 0;
			n = 1;
		}
		if ( numTables == MAX_TABLES ) {
			ri.Printf( PRINT_WARNING, "WARNING: MAX_TABLES hit\n" );
			return;
		}
		if ( R_FindTable( t.name ) >= 0 ) {
			continue;   // first definition wins, as with shaders
		}
		// copy the 0 element to the end, so lerping needs no wrap
		values[n++] = values[0];
		t.numValues = n;
		t.values = ri.Hunk_Alloc( n * sizeof( float ), h_low );
		Com_Memcpy( t.values, values, n * sizeof( float ) );
		tables[numTables++] = t;
	}
}

/*
=================
R_BuiltinTables

Tables the physical light effects use (step 7.5 B, docs/lights.md), added
after the .table files in scripts/ so a map's own table of the same name wins:
  oax_sin      one cycle of sin, 64 values (lerped)
  oax_blink    { 1, 0 } snapped: on for the first half of a cycle
  oax_flicker  64 snapped values: 0 below 0.5, else the value (UE1 LT_Flicker)
  oax_strobe   64 snapped values: 0 or 1, half each (UE1 LT_Strobe)
The random tables come from a fixed LCG, so every host draws the same light.
=================
*/
static void R_AddBuiltinTable( const char *name, const float *values, int n, qboolean snap ) {
	matTable_t t;

	if ( R_FindTable( name ) >= 0 || numTables == MAX_TABLES ) {
		return;
	}
	Com_Memset( &t, 0, sizeof( t ) );
	Q_strncpyz( t.name, name, sizeof( t.name ) );
	t.snap = snap;
	t.numValues = n + 1;
	t.values = ri.Hunk_Alloc( ( n + 1 ) * sizeof( float ), h_low );
	Com_Memcpy( t.values, values, n * sizeof( float ) );
	t.values[n] = values[0];
	tables[numTables++] = t;
}

// sin by its Taylor series on [-pi/2, pi/2]: plain IEEE arithmetic, so every
// host builds the same table without the host libm
static double DetSin( double x ) {
	double x2, term, sum;
	int n;

	while ( x > M_PI ) {
		x -= 2.0 * M_PI;
	}
	if ( x > M_PI / 2 ) {
		x = M_PI - x;
	} else if ( x < -M_PI / 2 ) {
		x = -M_PI - x;
	}
	x2 = x * x;
	term = x;
	sum = x;
	for ( n = 1; n < 12; n++ ) {
		term *= -x2 / ( ( 2 * n ) * ( 2 * n + 1 ) );
		sum += term;
	}
	return sum;
}

static void R_BuiltinTables( void ) {
	float v[64];
	unsigned int seed = 12345u;
	int i;

	for ( i = 0; i < 64; i++ ) {
		v[i] = (float)DetSin( i * ( 2.0 * M_PI / 64.0 ) );
	}
	R_AddBuiltinTable( "oax_sin", v, 64, qfalse );
	v[0] = 1;
	v[1] = 0;
	R_AddBuiltinTable( "oax_blink", v, 2, qtrue );
	for ( i = 0; i < 64; i++ ) {
		float r;

		seed = seed * 1103515245u + 12345u;
		r = ( ( seed >> 8 ) & 0xffff ) / 65536.0f;
		v[i] = r < 0.5f ? 0.0f : r;
	}
	R_AddBuiltinTable( "oax_flicker", v, 64, qtrue );
	for ( i = 0; i < 64; i++ ) {
		seed = seed * 1103515245u + 12345u;
		v[i] = ( ( seed >> 8 ) & 0xffff ) < 0x8000 ? 0.0f : 1.0f;
	}
	R_AddBuiltinTable( "oax_strobe", v, 64, qtrue );
}

void R_MatExprInit( void ) {
	char **files;
	int numFiles, i;

	numTables = 0;
	files = ri.FS_ListFiles( "scripts", ".table", &numFiles );
	for ( i = 0; i < numFiles; i++ ) {
		char path[MAX_QPATH];
		char *buf;
		long len;

		Com_sprintf( path, sizeof( path ), "scripts/%s", files[i] );
		len = ri.FS_ReadFile( path, (void **)&buf );
		if ( buf ) {
			R_ParseTables( buf, len, path );
			ri.FS_FreeFile( buf );
		}
	}
	ri.FS_FreeFileList( files );
	R_BuiltinTables();
	if ( numTables ) {
		ri.Printf( PRINT_DEVELOPER, "%d material tables\n", numTables );
	}
}

// ---------------------------------------------------------------------------
// expressions (Material.cpp)

static matExpr_t *Expr_Get( shader_t *sh ) {
	if ( !sh->oaxExpr ) {
		int i;

		sh->oaxExpr = ri.Hunk_Alloc( sizeof( matExpr_t ), h_low );
		sh->oaxExpr->numRegisters = MEXP_REG_NUM_PREDEFINED;
		for ( i = 0; i < MEXP_REG_NUM_PREDEFINED; i++ ) {
			sh->oaxExpr->registerIsTemporary[i] = qtrue;
		}
		sh->oaxExpr->constant = qtrue;
		sh->oaxExpr->lastFrame = -1;
	}
	return sh->oaxExpr;
}

static int Expr_Constant( matExpr_t *e, float f ) {
	int i;

	for ( i = MEXP_REG_NUM_PREDEFINED; i < e->numRegisters; i++ ) {
		if ( !e->registerIsTemporary[i] && e->constRegisters[i] == f ) {
			return i;
		}
	}
	if ( e->numRegisters == MATEXPR_MAX_REGISTERS ) {
		ri.Printf( PRINT_WARNING, "WARNING: material expression hit MATEXPR_MAX_REGISTERS\n" );
		return 0;
	}
	e->registerIsTemporary[i] = qfalse;
	e->constRegisters[i] = f;
	e->numRegisters++;
	return i;
}

static int Expr_Temporary( matExpr_t *e ) {
	if ( e->numRegisters == MATEXPR_MAX_REGISTERS ) {
		ri.Printf( PRINT_WARNING, "WARNING: material expression hit MATEXPR_MAX_REGISTERS\n" );
		return 0;
	}
	e->registerIsTemporary[e->numRegisters] = qtrue;
	e->numRegisters++;
	return e->numRegisters - 1;
}

static int Expr_EmitOp( matExpr_t *e, int a, int b, int opType ) {
	matExprOp_t *op;
	qboolean ca = !e->registerIsTemporary[a], cb = !e->registerIsTemporary[b];

	// optimize away identity operations
	if ( opType == MOP_ADD ) {
		if ( ca && e->constRegisters[a] == 0 ) {
			return b;
		}
		if ( cb && e->constRegisters[b] == 0 ) {
			return a;
		}
		if ( ca && cb ) {
			return Expr_Constant( e, e->constRegisters[a] + e->constRegisters[b] );
		}
	}
	if ( opType == MOP_MULTIPLY ) {
		if ( ca && e->constRegisters[a] == 1 ) {
			return b;
		}
		if ( ca && e->constRegisters[a] == 0 ) {
			return a;
		}
		if ( cb && e->constRegisters[b] == 1 ) {
			return a;
		}
		if ( cb && e->constRegisters[b] == 0 ) {
			return b;
		}
		if ( ca && cb ) {
			return Expr_Constant( e, e->constRegisters[a] * e->constRegisters[b] );
		}
	}

	if ( e->numOps == MATEXPR_MAX_OPS ) {
		ri.Printf( PRINT_WARNING, "WARNING: material expression hit MATEXPR_MAX_OPS\n" );
		return 0;
	}
	op = &e->ops[e->numOps++];
	op->opType = opType;
	op->a = a;
	op->b = b;
	op->c = Expr_Temporary( e );
	return op->c;
}

static int Expr_Parse( matExpr_t *e, matLex_t *lx );

static int Expr_Term( matExpr_t *e, matLex_t *lx ) {
	static const char *regs[] = {
		"time", "parm0", "parm1", "parm2", "parm3", "parm4", "parm5", "parm6", "parm7",
		"parm8", "parm9", "parm10", "parm11", "global0", "global1", "global2", "global3",
		"global4", "global5", "global6", "global7"
	};
	int i, a, b, table;

	if ( !Lex_Next( lx ) ) {
		lx->error = qtrue;
		return 0;
	}

	if ( !strcmp( lx->tok, "(" ) ) {
		a = Expr_Parse( e, lx );
		if ( !Lex_Next( lx ) || strcmp( lx->tok, ")" ) ) {
			lx->error = qtrue;
		}
		return a;
	}

	for ( i = 0; i < MEXP_REG_NUM_PREDEFINED; i++ ) {
		if ( !Q_stricmp( lx->tok, regs[i] ) ) {
			e->constant = qfalse;
			return i;
		}
	}
	if ( !Q_stricmp( lx->tok, "fragmentPrograms" ) ) {
		return Expr_Constant( e, 1.0f );
	}
	if ( !Q_stricmp( lx->tok, "sound" ) ) {
		e->constant = qfalse;
		return Expr_EmitOp( e, 0, 0, MOP_SOUND );
	}

	// negative numbers
	if ( !strcmp( lx->tok, "-" ) ) {
		Lex_Next( lx );
		if ( lx->isNumber ) {
			return Expr_Constant( e, -(float)atof( lx->tok ) );
		}
		ri.Printf( PRINT_WARNING, "WARNING: bad negative number '%s' in shader '%s'\n", lx->tok, "material" );
		lx->error = qtrue;
		return 0;
	}

	if ( lx->isNumber ) {
		return Expr_Constant( e, (float)atof( lx->tok ) );
	}

	// a table name
	table = R_FindTable( lx->tok );
	if ( table < 0 ) {
		ri.Printf( PRINT_WARNING, "WARNING: bad term '%s' in material expression\n", lx->tok );
		lx->error = qtrue;
		return 0;
	}
	e->constant = qfalse;
	if ( !Lex_Next( lx ) || strcmp( lx->tok, "[" ) ) {
		lx->error = qtrue;
		return 0;
	}
	b = Expr_Parse( e, lx );
	if ( !Lex_Next( lx ) || strcmp( lx->tok, "]" ) ) {
		lx->error = qtrue;
		return 0;
	}
	return Expr_EmitOp( e, table, b, MOP_TABLE );
}

#define TOP_PRIORITY 4
static int Expr_Priority( matExpr_t *e, matLex_t *lx, int priority ) {
	static const struct { int priority; const char *tok; int op; } ops[] = {
		{ 1, "*", MOP_MULTIPLY }, { 1, "/", MOP_DIVIDE }, { 1, "%", MOP_MOD },
		{ 2, "+", MOP_ADD }, { 2, "-", MOP_SUBTRACT },
		{ 3, ">", MOP_GT }, { 3, ">=", MOP_GE }, { 3, "<", MOP_LT }, { 3, "<=", MOP_LE },
		{ 3, "==", MOP_EQ }, { 3, "!=", MOP_NE },
		{ 4, "&&", MOP_AND }, { 4, "||", MOP_OR },
	};
	int a, i;

	if ( priority == 0 ) {
		return Expr_Term( e, lx );
	}

	a = Expr_Priority( e, lx, priority - 1 );
	if ( lx->error ) {
		return 0;
	}
	if ( !Lex_Next( lx ) ) {
		return a;
	}
	for ( i = 0; i < (int)ARRAY_LEN( ops ); i++ ) {
		if ( ops[i].priority == priority && !strcmp( lx->tok, ops[i].tok ) ) {
			int b = Expr_Priority( e, lx, priority );
			return Expr_EmitOp( e, a, b, ops[i].op );
		}
	}

	// anything else terminates the expression
	Lex_Unread( lx );
	return a;
}

static int Expr_Parse( matExpr_t *e, matLex_t *lx ) {
	return Expr_Priority( e, lx, TOP_PRIORITY );
}

/*
=================
R_MatExprEvaluate

idMaterial::EvaluateRegisters
=================
*/
void R_MatExprEvaluate( const matExpr_t *e, float *registers, const float *parms, float time ) {
	int i, b;
	const matExprOp_t *op;

	for ( i = MEXP_REG_NUM_PREDEFINED; i < e->numRegisters; i++ ) {
		registers[i] = e->constRegisters[i];
	}
	registers[MEXP_REG_TIME] = time;
	for ( i = 0; i < 12; i++ ) {
		registers[MEXP_REG_PARM0 + i] = parms ? parms[i] : ( i < 4 ? 1.0f : 0.0f );
	}
	for ( i = 0; i < 8; i++ ) {
		registers[MEXP_REG_GLOBAL0 + i] = 0;
	}

	for ( i = 0, op = e->ops; i < e->numOps; i++, op++ ) {
		switch ( op->opType ) {
		case MOP_ADD:      registers[op->c] = registers[op->a] + registers[op->b]; break;
		case MOP_SUBTRACT: registers[op->c] = registers[op->a] - registers[op->b]; break;
		case MOP_MULTIPLY: registers[op->c] = registers[op->a] * registers[op->b]; break;
		case MOP_DIVIDE:   registers[op->c] = registers[op->a] / registers[op->b]; break;
		case MOP_MOD:
			b = (int)registers[op->b];
			b = b != 0 ? b : 1;
			registers[op->c] = (int)registers[op->a] % b;
			break;
		case MOP_TABLE:    registers[op->c] = R_TableLookup( op->a, registers[op->b] ); break;
		case MOP_SOUND:    registers[op->c] = 0; break;
		case MOP_GT:       registers[op->c] = registers[op->a] > registers[op->b]; break;
		case MOP_GE:       registers[op->c] = registers[op->a] >= registers[op->b]; break;
		case MOP_LT:       registers[op->c] = registers[op->a] < registers[op->b]; break;
		case MOP_LE:       registers[op->c] = registers[op->a] <= registers[op->b]; break;
		case MOP_EQ:       registers[op->c] = registers[op->a] == registers[op->b]; break;
		case MOP_NE:       registers[op->c] = registers[op->a] != registers[op->b]; break;
		case MOP_AND:      registers[op->c] = registers[op->a] && registers[op->b]; break;
		case MOP_OR:       registers[op->c] = registers[op->a] || registers[op->b]; break;
		default:
			ri.Error( ERR_DROP, "R_MatExprEvaluate: bad opcode" );
		}
	}
}

// ---------------------------------------------------------------------------
// shader keywords

// the rest of the current line, and advance past it
static void RestOfLine( char **text, const char **start, const char **end ) {
	char *p = *text;

	while ( *p == ' ' || *p == '\t' ) {
		p++;
	}
	*start = p;
	while ( *p && *p != '\n' ) {
		p++;
	}
	*end = p;
	*text = p;
}

static int Expr_ParseLine( shader_t *sh, matLex_t *lx ) {
	matExpr_t *e = Expr_Get( sh );
	int r = Expr_Parse( e, lx );

	if ( lx->error ) {
		ri.Printf( PRINT_WARNING, "WARNING: bad expression in shader '%s'\n", sh->name );
		return -1;
	}
	return r;
}

/*
=================
R_MatExprStageShorthand

The id Tech 4 stage shorthands at shader level: `diffusemap <image>`,
`bumpmap <image>`, `specularmap <image>`. Writes the text of the
equivalent Q3 stage (without the opening brace) for ParseStage.
=================
*/
qboolean R_MatExprStageShorthand( const char *token, char **text, char *out, int outSize ) {
	const char *type, *start, *end;
	char image[MAX_QPATH];
	int len;

	if ( !Q_stricmp( token, "diffusemap" ) ) {
		type = "diffuseMap";
	} else if ( !Q_stricmp( token, "bumpmap" ) ) {
		type = "normalMap";
	} else if ( !Q_stricmp( token, "specularmap" ) ) {
		type = "specularMap";
	} else {
		return qfalse;
	}

	RestOfLine( text, &start, &end );
	while ( end > start && ( end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ) ) {
		end--;
	}
	len = end - start;
	if ( len >= (int)sizeof( image ) ) {
		len = sizeof( image ) - 1;
	}
	Com_Memcpy( image, start, len );
	image[len] = 0;
	// the stage type goes first: "map" picks the image type (normal maps) from it
	Com_sprintf( out, outSize, "stage %s\nmap %s\n}\n", type, image );
	return qtrue;
}

/*
=================
R_MatExprParseShaderKeyword

Light material keywords at shader level.
=================
*/
qboolean R_MatExprParseShaderKeyword( const char *token, char **text, shader_t *sh ) {
	if ( !Q_stricmp( token, "lightFalloffImage" ) ) {
		const char *start, *end;
		char name[MAX_QPATH];
		int len;

		RestOfLine( text, &start, &end );
		while ( end > start && ( end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ) ) {
			end--;
		}
		len = MIN( end - start, (int)sizeof( name ) - 1 );
		Com_Memcpy( name, start, len );
		name[len] = 0;
		sh->oaxLightFalloff = R_FindImageFile( name, IMGTYPE_COLORALPHA, IMGFLAG_CLAMPTOEDGE | IMGFLAG_NO_COMPRESSION );
		if ( !sh->oaxLightFalloff ) {
			ri.Printf( PRINT_WARNING, "WARNING: shader '%s': lightFalloffImage '%s' not found\n", sh->name, name );
		}
		sh->oaxLightFlags |= ULSF_LIGHTSHADER;
		return qtrue;
	}
	if ( !Q_stricmp( token, "ambientLight" ) ) {
		sh->oaxLightFlags |= ULSF_AMBIENT | ULSF_LIGHTSHADER;
		return qtrue;
	}
	if ( !Q_stricmp( token, "fogLight" ) ) {
		sh->oaxLightFlags |= ULSF_FOG | ULSF_LIGHTSHADER;
		return qtrue;
	}
	if ( !Q_stricmp( token, "blendLight" ) ) {
		sh->oaxLightFlags |= ULSF_BLEND | ULSF_LIGHTSHADER;
		return qtrue;
	}
	if ( !Q_stricmp( token, "noShadows" ) ) {
		sh->oaxLightFlags |= ULSF_NOSHADOWS;
		return qtrue;
	}
	if ( !Q_stricmp( token, "noSelfShadow" ) ) {
		sh->oaxLightFlags |= ULSF_NOSELFSHADOW;
		return qtrue;
	}
	if ( !Q_stricmp( token, "forceShadows" ) ) {
		sh->oaxLightFlags |= ULSF_FORCESHADOWS;
		return qtrue;
	}
	if ( !Q_stricmp( token, "oaxLightMask" ) ) {
		// light-mask groups of the surfaces using this material (step 7.5 B)
		char *t = COM_ParseExt( text, qfalse );

		sh->oaxLightMask = (int)strtol( t, NULL, 0 ) & ULIGHT_MASK_ALL;
		if ( !sh->oaxLightMask ) {
			sh->oaxLightMask = -1;      // explicitly in no group: lit by no light
		}
		return qtrue;
	}
	return qfalse;
}

/*
=================
R_MatExprParseStageKeyword

Stage keywords: `blend diffusemap|bumpmap|specularmap` and the color
expressions `rgb`, `rgba`, `red`, `green`, `blue`, `alpha`, `color`,
`colored`.
=================
*/
qboolean R_MatExprParseStageKeyword( const char *token, char **text, shader_t *sh, shaderStage_t *stage ) {
	const char *start, *end;
	matLex_t lx;
	int r, i;

	if ( !Q_stricmp( token, "blend" ) ) {
		char *t = COM_ParseExt( text, qfalse );

		if ( !Q_stricmp( t, "diffusemap" ) ) {
			stage->type = ST_DIFFUSEMAP;
			return qtrue;
		}
		if ( !Q_stricmp( t, "bumpmap" ) ) {
			stage->type = ST_NORMALMAP;
			VectorSet4( stage->normalScale, r_baseNormalX->value, r_baseNormalY->value, 1.0f, r_baseParallax->value );
			return qtrue;
		}
		if ( !Q_stricmp( t, "specularmap" ) ) {
			stage->type = ST_SPECULARMAP;
			VectorSet4( stage->specularScale, 1.0f, 1.0f, 1.0f, 1.0f );
			return qtrue;
		}
		ri.Printf( PRINT_WARNING, "WARNING: 'blend %s' in shader '%s': use blendfunc\n", t, sh->name );
		return qtrue;
	}

	if ( !Q_stricmp( token, "colored" ) ) {
		Expr_Get( sh )->constant = qfalse;
		for ( i = 0; i < 4; i++ ) {
			stage->oaxColorReg[i] = MEXP_REG_PARM0 + i;
		}
		stage->oaxColorExpr = qtrue;
		stage->rgbGen = CGEN_CONST;
		stage->alphaGen = AGEN_CONST;
		return qtrue;
	}

	if ( Q_stricmp( token, "rgb" ) && Q_stricmp( token, "rgba" ) && Q_stricmp( token, "red" ) && Q_stricmp( token, "green" )
		&& Q_stricmp( token, "blue" ) && Q_stricmp( token, "alpha" ) && Q_stricmp( token, "color" ) ) {
		return qfalse;
	}

	if ( !stage->oaxColorExpr ) {
		r = Expr_Constant( Expr_Get( sh ), 1.0f );
		for ( i = 0; i < 4; i++ ) {
			stage->oaxColorReg[i] = r;
		}
		stage->oaxColorExpr = qtrue;
	}

	RestOfLine( text, &start, &end );
	Lex_Init( &lx, start, end );

	if ( !Q_stricmp( token, "color" ) ) {
		for ( i = 0; i < 4; i++ ) {
			r = Expr_ParseLine( sh, &lx );
			if ( r < 0 ) {
				return qtrue;
			}
			stage->oaxColorReg[i] = r;
			if ( i < 3 && ( !Lex_Next( &lx ) || strcmp( lx.tok, "," ) ) ) {
				ri.Printf( PRINT_WARNING, "WARNING: 'color' needs four comma-separated values in shader '%s'\n", sh->name );
				return qtrue;
			}
		}
	} else {
		r = Expr_ParseLine( sh, &lx );
		if ( r < 0 ) {
			return qtrue;
		}
		if ( !Q_stricmp( token, "red" ) ) {
			stage->oaxColorReg[0] = r;
		} else if ( !Q_stricmp( token, "green" ) ) {
			stage->oaxColorReg[1] = r;
		} else if ( !Q_stricmp( token, "blue" ) ) {
			stage->oaxColorReg[2] = r;
		} else if ( !Q_stricmp( token, "alpha" ) ) {
			stage->oaxColorReg[3] = r;
		} else {
			stage->oaxColorReg[0] = stage->oaxColorReg[1] = stage->oaxColorReg[2] = r;
			if ( !Q_stricmp( token, "rgba" ) ) {
				stage->oaxColorReg[3] = r;
			}
		}
	}

	stage->rgbGen = CGEN_CONST;
	stage->alphaGen = AGEN_CONST;
	return qtrue;
}

/*
=================
R_StageExprColor

The color of a stage with expressions, for the given parms and time.
=================
*/
void R_StageExprColor( shader_t *sh, int stageNum, const float *parms, float time, vec4_t out ) {
	float regs[MATEXPR_MAX_REGISTERS];
	shaderStage_t *stage;
	int i;

	VectorSet4( out, 1, 1, 1, 1 );
	if ( !sh || stageNum < 0 || stageNum >= MAX_SHADER_STAGES || !sh->stages[stageNum] ) {
		return;
	}
	stage = sh->stages[stageNum];
	if ( !stage->oaxColorExpr || !sh->oaxExpr ) {
		if ( stage->rgbGen == CGEN_CONST ) {
			for ( i = 0; i < 4; i++ ) {
				out[i] = stage->constantColor[i] / 255.0f;
			}
		}
		return;
	}
	R_MatExprEvaluate( sh->oaxExpr, regs, parms, time );
	for ( i = 0; i < 4; i++ ) {
		out[i] = regs[stage->oaxColorReg[i]];
	}
}

/*
=================
R_MatExprUpdateFrame

Evaluate every material with expressions once per frame and store the
result in the stages' constant colors, which the stock stage code draws.
=================
*/
void R_MatExprUpdateFrame( void ) {
	float regs[MATEXPR_MAX_REGISTERS];
	int i, j, k;

	for ( i = 0; i < tr.numShaders; i++ ) {
		shader_t *sh = tr.shaders[i];

		if ( !sh->oaxExpr || sh->oaxExpr->lastFrame == tr.frameCount ) {
			continue;
		}
		sh->oaxExpr->lastFrame = tr.frameCount;
		R_MatExprEvaluate( sh->oaxExpr, regs, NULL, tr.refdef.floatTime );
		for ( j = 0; j < MAX_SHADER_STAGES; j++ ) {
			shaderStage_t *st = sh->stages[j];

			if ( !st || !st->oaxColorExpr ) {
				continue;
			}
			for ( k = 0; k < 4; k++ ) {
				float v = regs[st->oaxColorReg[k]];
				st->constantColor[k] = (byte)( 255.0f * ( v < 0 ? 0 : v > 1 ? 1 : v ) );
			}
		}
	}
}
