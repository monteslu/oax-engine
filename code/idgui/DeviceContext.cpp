/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company. 

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).  

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
Adapted from DOOM-3 neo/ui/DeviceContext.cpp for the oa-engine GUI module.
Changes: everything is in namespace idgui and includes idgui_precompiled.h;
drawing goes to the engine import table (Q3 SetColor / StretchPic, and
DrawQuad for rotated or sheared windows) instead of DOOM-3's renderSystem;
fonts are built from OpenArena's bitmap fonts (menu/art/font1_prop.tga with
OA's propMap table, gfx/2d/bigchars.tga) in DOOM-3's glyph layout instead of
FreeType .dat files; OA assets replace guis/assets and ui/assets; the cursor
is scaled on copies (DOOM-3 scaled the GUI's own cursor position in place);
borders scale with the virtual screen.
*/

#include "idgui_precompiled.h"

namespace idgui {


idVec4 idDeviceContext::colorPurple;
idVec4 idDeviceContext::colorOrange;
idVec4 idDeviceContext::colorYellow;
idVec4 idDeviceContext::colorGreen;
idVec4 idDeviceContext::colorBlue;
idVec4 idDeviceContext::colorRed;
idVec4 idDeviceContext::colorBlack;
idVec4 idDeviceContext::colorWhite;
idVec4 idDeviceContext::colorNone;


idCVar gui_smallFontLimit( "gui_smallFontLimit", "0.30", CVAR_GUI | CVAR_ARCHIVE, "" );
idCVar gui_mediumFontLimit( "gui_mediumFontLimit", "0.60", CVAR_GUI | CVAR_ARCHIVE, "" );


static void SetDrawColor( const idVec4 &color ) {
	if ( gImport.SetColor ) {
		gImport.SetColor( color.ToFloatPtr() );
	}
}

idList<fontInfoEx_t> idDeviceContext::fonts;

/*
	OpenArena fonts. DOOM-3 fonts are FreeType renders in .dat files; OA
	ships two bitmap fonts instead, so the glyph tables are built here in
	DOOM-3's layout (glyph metrics at 48 points, scale 1.0) and the text
	code below is unchanged:
	  "fonts" and anything not named mono: menu/art/font1_prop.tga, OA's
	    proportional menu font (the propMap table from OA's q3_ui/ui_atoms.c,
	    GPLv2+, id Software); lower case draws as upper case, as in OA.
	  "fonts/mono" (or any name containing "mono" or "bigchars"):
	    gfx/2d/bigchars.tga, the 16x16 console font.
*/

static const int oaPropMap[128][3] = {
{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1},
{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1},
{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1},
{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1},
{0, 0, 8},		// SPACE
{11, 122, 7},	// !
{154, 181, 14},	// "
{55, 122, 17},	// #
{79, 122, 18},	// $
{101, 122, 23},	// %
{153, 122, 18},	// &
{9, 93, 7},		// '
{207, 122, 8},	// (
{230, 122, 9},	// )
{177, 122, 18},	// *
{30, 152, 18},	// +
{85, 181, 7},	// ,
{34, 93, 11},	// -
{110, 181, 6},	// .
{130, 152, 14},	// /
{22, 64, 17},	// 0
{41, 64, 12},	// 1
{58, 64, 17},	// 2
{78, 64, 18},	// 3
{98, 64, 19},	// 4
{120, 64, 18},	// 5
{141, 64, 18},	// 6
{204, 64, 16},	// 7
{162, 64, 17},	// 8
{182, 64, 18},	// 9
{59, 181, 7},	// :
{35,181, 7},	// ;
{203, 152, 14},	// <
{56, 93, 14},	// =
{228, 152, 14},	// >
{177, 181, 18},	// ?
{28, 122, 22},	// @
{5, 4, 18},		// A
{27, 4, 18},	// B
{48, 4, 18},	// C
{69, 4, 17},	// D
{90, 4, 13},	// E
{106, 4, 13},	// F
{121, 4, 18},	// G
{143, 4, 17},	// H
{164, 4, 8},	// I
{175, 4, 16},	// J
{195, 4, 18},	// K
{216, 4, 12},	// L
{230, 4, 23},	// M
{6, 34, 18},	// N
{27, 34, 18},	// O
{48, 34, 18},	// P
{68, 34, 18},	// Q
{90, 34, 17},	// R
{110, 34, 18},	// S
{130, 34, 14},	// T
{146, 34, 18},	// U
{166, 34, 19},	// V
{185, 34, 29},	// W
{215, 34, 18},	// X
{234, 34, 18},	// Y
{5, 64, 14},	// Z
{60, 152, 7},	// [
{106, 151, 13},	// '\'
{83, 152, 7},	// ]
{128, 122, 17},	// ^
{4, 152, 21},	// _
{134, 181, 5},	// '
{5, 4, 18},		// A
{27, 4, 18},	// B
{48, 4, 18},	// C
{69, 4, 17},	// D
{90, 4, 13},	// E
{106, 4, 13},	// F
{121, 4, 18},	// G
{143, 4, 17},	// H
{164, 4, 8},	// I
{175, 4, 16},	// J
{195, 4, 18},	// K
{216, 4, 12},	// L
{230, 4, 23},	// M
{6, 34, 18},	// N
{27, 34, 18},	// O
{48, 34, 18},	// P
{68, 34, 18},	// Q
{90, 34, 17},	// R
{110, 34, 18},	// S
{130, 34, 14},	// T
{146, 34, 18},	// U
{166, 34, 19},	// V
{185, 34, 29},	// W
{215, 34, 18},	// X
{234, 34, 18},	// Y
{5, 64, 14},	// Z
{153, 152, 13},	// {
{11, 181, 5},	// |
{180, 152, 13},	// }
{79, 93, 17},	// ~
{0, 0, -1}		// DEL
};

static const int OA_PROP_HEIGHT = 27;		// cell height in font1_prop.tga
static const int OA_PROP_BASELINE = 22;		// baseline inside the cell
static const int OA_PROP_GAP = 3;			// PROP_GAP_WIDTH

static void BuildOAFontInfo( fontInfo_t &f, bool mono ) {
	memset( &f, 0, sizeof( f ) );
	if ( mono ) {
		const idMaterial *mat = declManager->FindMaterial( "gfx/2d/bigchars" );
		// 16 x 16 cells of 16 x 16 texels; scale 1.0 draws 48 units
		f.glyphScale = 48.0f / 16.0f;
		for ( int c = 0; c < GLYPHS_PER_FONT; c++ ) {
			glyphInfo_t &g = f.glyphs[c];
			float s = ( c & 15 ) / 16.0f;
			float t = ( c >> 4 ) / 16.0f;
			g.height = g.imageHeight = 16;
			g.top = 14;
			g.bottom = 2;
			g.pitch = g.imageWidth = 16;
			g.xSkip = 16;
			// half a texel in, so linear filtering never reads a neighbour cell
			g.s = s + 0.5f / 256.0f;
			g.t = t + 0.5f / 256.0f;
			g.s2 = s + 15.5f / 256.0f;
			g.t2 = t + 15.5f / 256.0f;
			g.glyph = mat;
		}
		idStr::Copynz( f.name, "gfx/2d/bigchars", sizeof( f.name ) );
		return;
	}
	const idMaterial *mat = declManager->FindMaterial( "menu/art/font1_prop.tga" );
	f.glyphScale = 48.0f / OA_PROP_HEIGHT;
	for ( int c = 0; c < 128; c++ ) {
		glyphInfo_t &g = f.glyphs[c];
		int w = oaPropMap[c][2];
		if ( w < 0 ) {
			continue;
		}
		g.height = g.imageHeight = OA_PROP_HEIGHT;
		g.top = OA_PROP_BASELINE;
		g.bottom = OA_PROP_HEIGHT - OA_PROP_BASELINE;
		g.pitch = g.imageWidth = ( c == ' ' ) ? 0 : w;
		g.xSkip = w + OA_PROP_GAP;
		g.s = oaPropMap[c][0] / 256.0f;
		g.t = oaPropMap[c][1] / 256.0f;
		g.s2 = ( oaPropMap[c][0] + w ) / 256.0f;
		g.t2 = ( oaPropMap[c][1] + OA_PROP_HEIGHT ) / 256.0f;
		g.glyph = mat;
	}
	idStr::Copynz( f.name, "menu/art/font1_prop.tga", sizeof( f.name ) );
}

static void BuildOAFont( fontInfoEx_t &font, const char *name ) {
	idStr n = name;
	bool mono = n.Find( "mono", false ) >= 0 || n.Find( "bigchars", false ) >= 0;
	int maxW = 0, maxH = 0;

	// one bitmap serves all three DOOM-3 sizes
	BuildOAFontInfo( font.fontInfoSmall, mono );
	BuildOAFontInfo( font.fontInfoMedium, mono );
	BuildOAFontInfo( font.fontInfoLarge, mono );
	for ( int c = 0; c < GLYPHS_PER_FONT; c++ ) {
		maxW = Max( maxW, font.fontInfoLarge.glyphs[c].xSkip );
		maxH = Max( maxH, font.fontInfoLarge.glyphs[c].height );
	}
	font.maxWidth = font.maxWidthSmall = font.maxWidthMedium = font.maxWidthLarge = maxW;
	font.maxHeight = font.maxHeightSmall = font.maxHeightMedium = font.maxHeightLarge = maxH;
	idStr::Copynz( font.name, name, sizeof( font.name ) );
}

int idDeviceContext::FindFont( const char *name ) {
	int c = fonts.Num();
	for (int i = 0; i < c; i++) {
		if (idStr::Icmp(name, fonts[i].name) == 0) {
			return i;
		}
	}

	fontInfoEx_t fontInfo;
	int index = fonts.Append( fontInfo );
	BuildOAFont( fonts[index], name );
	return index;
}

void idDeviceContext::SetupFonts() {
	fonts.SetGranularity( 1 );

	// Default font has to be added first
	FindFont( "fonts" );
}

void idDeviceContext::SetFont( int num ) {
	if ( num >= 0 && num < fonts.Num() ) {
		activeFont = &fonts[num];
	} else {
		activeFont = &fonts[0];
	}
}


void idDeviceContext::Init() {
	xScale = 0.0;
	targetXScale = targetYScale = 1.0f;
	drawTime = 0;
	SetSize(VIRTUAL_WIDTH, VIRTUAL_HEIGHT);
	// OA assets in place of DOOM-3's guis/assets and ui/assets
	whiteImage = declManager->FindMaterial("white");
	whiteImage->SetSort( SS_GUI );
	mbcs = false;
	SetupFonts();
	activeFont = &fonts[0];
	colorPurple = idVec4(1, 0, 1, 1);
	colorOrange = idVec4(1, 1, 0, 1);
	colorYellow = idVec4(0, 1, 1, 1);
	colorGreen = idVec4(0, 1, 0, 1);
	colorBlue = idVec4(0, 0, 1, 1);
	colorRed = idVec4(1, 0, 0, 1);
	colorWhite = idVec4(1, 1, 1, 1);
	colorBlack = idVec4(0, 0, 0, 1);
	colorNone = idVec4(0, 0, 0, 0);
	cursorImages[CURSOR_ARROW] = declManager->FindMaterial("menu/art/3_cursor2");
	cursorImages[CURSOR_HAND] = declManager->FindMaterial("menu/art/3_cursor2");
	scrollBarImages[SCROLLBAR_HBACK] = whiteImage;
	scrollBarImages[SCROLLBAR_VBACK] = whiteImage;
	scrollBarImages[SCROLLBAR_THUMB] = whiteImage;
	scrollBarImages[SCROLLBAR_RIGHT] = whiteImage;
	scrollBarImages[SCROLLBAR_LEFT] = whiteImage;
	scrollBarImages[SCROLLBAR_UP] = whiteImage;
	scrollBarImages[SCROLLBAR_DOWN] = whiteImage;
	cursorImages[CURSOR_ARROW]->SetSort( SS_GUI );
	cursorImages[CURSOR_HAND]->SetSort( SS_GUI );
	scrollBarImages[SCROLLBAR_HBACK]->SetSort( SS_GUI );
	scrollBarImages[SCROLLBAR_VBACK]->SetSort( SS_GUI );
	scrollBarImages[SCROLLBAR_THUMB]->SetSort( SS_GUI );
	scrollBarImages[SCROLLBAR_RIGHT]->SetSort( SS_GUI );
	scrollBarImages[SCROLLBAR_LEFT]->SetSort( SS_GUI );
	scrollBarImages[SCROLLBAR_UP]->SetSort( SS_GUI );
	scrollBarImages[SCROLLBAR_DOWN]->SetSort( SS_GUI );
	cursor = CURSOR_ARROW;
	enableClipping = true;
	overStrikeMode = true;
	mat.Identity();
	origin.Zero();
	initialized = true;
}

void idDeviceContext::Shutdown() {
	fontName.Clear();
	clipRects.Clear();
	fonts.Clear();
	Clear();
}

void idDeviceContext::Clear() {
	initialized = false;
	targetXScale = targetYScale = 1.0f;
	aspectWidth = VIRTUAL_WIDTH;
	aspectHeight = VIRTUAL_HEIGHT;
	drawTime = 0;
	useFont = NULL;
	activeFont = NULL;
	mbcs = false;
}

idDeviceContext::idDeviceContext() {
	Clear();
}

void idDeviceContext::SetTransformInfo(const idVec3 &org, const idMat3 &m) {
	origin = org;
	mat = m;
}

// 
//  added method
void idDeviceContext::GetTransformInfo(idVec3& org, idMat3& m )
{
	m = mat;
	org = origin;
}
// 

void idDeviceContext::PopClipRect() {
	if (clipRects.Num()) {
		clipRects.RemoveIndex(clipRects.Num()-1);
	}
}

void idDeviceContext::PushClipRect(idRectangle r) {
	clipRects.Append(r);
}

void idDeviceContext::PushClipRect(float x, float y, float w, float h) {
	clipRects.Append(idRectangle(x, y, w, h));
}

bool idDeviceContext::ClippedCoords(float *x, float *y, float *w, float *h, float *s1, float *t1, float *s2, float *t2) {

	if ( enableClipping == false || clipRects.Num() == 0 ) {
		return false;
	}

	int c = clipRects.Num();
	while( --c > 0 ) {
		idRectangle *clipRect = &clipRects[c];
 
		float ox = *x;
		float oy = *y;
		float ow = *w;
		float oh = *h;

		if ( ow <= 0.0f || oh <= 0.0f ) {
			break;
		}

		if (*x < clipRect->x) {
			*w -= clipRect->x - *x;
			*x = clipRect->x;
		} else if (*x > clipRect->x + clipRect->w) {
			*x = *w = *y = *h = 0;
		}
		if (*y < clipRect->y) {
			*h -= clipRect->y - *y;
			*y = clipRect->y;
		} else if (*y > clipRect->y + clipRect->h) {
			*x = *w = *y = *h = 0;
		}
		if (*w > clipRect->w) {
			*w = clipRect->w - *x + clipRect->x;
		} else if (*x + *w > clipRect->x + clipRect->w) {
			*w = clipRect->Right() - *x;
		}
		if (*h > clipRect->h) {
			*h = clipRect->h - *y + clipRect->y;
		} else if (*y + *h > clipRect->y + clipRect->h) {
			*h = clipRect->Bottom() - *y;
		}

		if ( s1 && s2 && t1 && t2 && ow > 0.0f ) {
			float ns1, ns2, nt1, nt2;
			// upper left
			float u = ( *x - ox ) / ow;
			ns1 = *s1 * ( 1.0f - u ) + *s2 * ( u );

			// upper right
			u = ( *x + *w - ox ) / ow;
			ns2 = *s1 * ( 1.0f - u ) + *s2 * ( u );

			// lower left
			u = ( *y - oy ) / oh;
			nt1 = *t1 * ( 1.0f - u ) + *t2 * ( u );

			// lower right
			u = ( *y + *h - oy ) / oh;
			nt2 = *t1 * ( 1.0f - u ) + *t2 * ( u );

			// set values
			*s1 = ns1;
			*s2 = ns2;
			*t1 = nt1;
			*t2 = nt2;
		}
	}

	return (*w == 0 || *h == 0) ? true : false;
}


void idDeviceContext::AdjustCoords(float *x, float *y, float *w, float *h) {
	if (x) {
		*x *= xScale;
	}
	if (y) {
		*y *= yScale;
	}
	if (w) {
		*w *= xScale;
	}
	if (h) {
		*h *= yScale;
	}
}

// The renderer draws axis-aligned pictures (Q3 StretchPic); a window
// transform (rotate, shear) turns the picture into a general quad.
static void EmitQuad( const idVec3 *xyz, const float *st, const idMaterial *shader ) {
	float xy[8];
	for ( int i = 0; i < 4; i++ ) {
		xy[i * 2 + 0] = xyz[i].x;
		xy[i * 2 + 1] = xyz[i].y;
	}
	if ( gImport.DrawQuad ) {
		gImport.DrawQuad( xy, st, shader->Handle() );
	}
}

void idDeviceContext::DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, const idMaterial *shader) {
	if ( !shader || !gImport.DrawStretchPic ) {
		return;
	}
	if ( mat.IsIdentity() ) {
		gImport.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, shader->Handle() );
		return;
	}

	idVec3 xyz[4];
	float st[8] = { s1, t1, s2, t1, s2, t2, s1, t2 };
	xyz[0].Set( x, y, 0 );
	xyz[1].Set( x + w, y, 0 );
	xyz[2].Set( x + w, y + h, 0 );
	xyz[3].Set( x, y + h, 0 );
	for ( int i = 0; i < 4; i++ ) {
		xyz[i] -= origin;
		xyz[i] *= mat;
		xyz[i] += origin;
	}
	EmitQuad( xyz, st, shader );
}


void idDeviceContext::DrawMaterial(float x, float y, float w, float h, const idMaterial *mat, const idVec4 &color, float scalex, float scaley) {

	SetDrawColor(color);

	float	s0, s1, t0, t1;
// 
//  handle negative scales as well	
	if ( scalex < 0 )
	{
		w *= -1;
		scalex *= -1;
	}
	if ( scaley < 0 )
	{
		h *= -1;
		scaley *= -1;
	}
// 
	if( w < 0 ) {	// flip about vertical
		w  = -w;
		s0 = 1 * scalex;
		s1 = 0;
	}
	else {
		s0 = 0;
		s1 = 1 * scalex;
	}

	if( h < 0 ) {	// flip about horizontal
		h  = -h;
		t0 = 1 * scaley;
		t1 = 0;
	}
	else {
		t0 = 0;
		t1 = 1 * scaley;
	}

	if ( ClippedCoords( &x, &y, &w, &h, &s0, &t0, &s1, &t1 ) ) {
		return;
	}

	AdjustCoords(&x, &y, &w, &h);

	DrawStretchPic( x, y, w, h, s0, t0, s1, t1, mat);
}

void idDeviceContext::DrawMaterialRotated(float x, float y, float w, float h, const idMaterial *mat, const idVec4 &color, float scalex, float scaley, float angle) {
	
	SetDrawColor(color);

	float	s0, s1, t0, t1;
	// 
	//  handle negative scales as well	
	if ( scalex < 0 )
	{
		w *= -1;
		scalex *= -1;
	}
	if ( scaley < 0 )
	{
		h *= -1;
		scaley *= -1;
	}
	// 
	if( w < 0 ) {	// flip about vertical
		w  = -w;
		s0 = 1 * scalex;
		s1 = 0;
	}
	else {
		s0 = 0;
		s1 = 1 * scalex;
	}

	if( h < 0 ) {	// flip about horizontal
		h  = -h;
		t0 = 1 * scaley;
		t1 = 0;
	}
	else {
		t0 = 0;
		t1 = 1 * scaley;
	}

	if ( angle == 0.0f && ClippedCoords( &x, &y, &w, &h, &s0, &t0, &s1, &t1 ) ) {
		return;
	}

	AdjustCoords(&x, &y, &w, &h);

	DrawStretchPicRotated( x, y, w, h, s0, t0, s1, t1, mat, angle);
}

void idDeviceContext::DrawStretchPicRotated(float x, float y, float w, float h, float s1, float t1, float s2, float t2, const idMaterial *shader, float angle) {
	if ( !shader ) {
		return;
	}
	if ( angle == 0.0f ) {
		DrawStretchPic( x, y, w, h, s1, t1, s2, t2, shader );
		return;
	}

	idVec3 xyz[4];
	float st[8] = { s1, t1, s2, t1, s2, t2, s1, t2 };
	xyz[0].Set( x, y, 0 );
	xyz[1].Set( x + w, y, 0 );
	xyz[2].Set( x + w, y + h, 0 );
	xyz[3].Set( x, y + h, 0 );

	if ( !mat.IsIdentity() ) {
		for ( int i = 0; i < 4; i++ ) {
			xyz[i] -= origin;
			xyz[i] *= mat;
			xyz[i] += origin;
		}
	}

	// rotate about the center of the picture (z axis)
	idVec3 origTrans( x + ( w / 2 ), y + ( h / 2 ), 0 );
	float sinAng = idMath::Sin( angle );
	float cosAng = idMath::Cos( angle );
	for ( int i = 0; i < 4; i++ ) {
		idVec3 v = xyz[i] - origTrans;
		xyz[i].Set( cosAng * v.x + sinAng * v.y, -sinAng * v.x + cosAng * v.y, v.z );
		xyz[i] += origTrans;
	}
	EmitQuad( xyz, st, shader );
}

void idDeviceContext::DrawFilledRect( float x, float y, float w, float h, const idVec4 &color) {

	if ( color.w == 0.0f ) {
		return;
	}

	SetDrawColor(color);
	
	if (ClippedCoords(&x, &y, &w, &h, NULL, NULL, NULL, NULL)) {
		return;
	}

	AdjustCoords(&x, &y, &w, &h);
	DrawStretchPic( x, y, w, h, 0, 0, 0, 0, whiteImage);
}


void idDeviceContext::DrawRect( float x, float y, float w, float h, float size, const idVec4 &color) {

	if ( color.w == 0.0f ) {
		return;
	}

	SetDrawColor(color);
	
	if (ClippedCoords(&x, &y, &w, &h, NULL, NULL, NULL, NULL)) {
		return;
	}

	AdjustCoords(&x, &y, &w, &h);
	// the border is in virtual pixels too (DOOM-3's renderer scaled the
	// whole 640x480 screen, borders included)
	float sw = size * xScale, sh = size * yScale;
	DrawStretchPic( x, y, sw, h, 0, 0, 0, 0, whiteImage );
	DrawStretchPic( x + w - sw, y, sw, h, 0, 0, 0, 0, whiteImage );
	DrawStretchPic( x, y, w, sh, 0, 0, 0, 0, whiteImage );
	DrawStretchPic( x, y + h - sh, w, sh, 0, 0, 0, 0, whiteImage );
}

void idDeviceContext::DrawMaterialRect( float x, float y, float w, float h, float size, const idMaterial *mat, const idVec4 &color) {

	if ( color.w == 0.0f ) {
		return;
	}

	SetDrawColor(color);
	DrawMaterial( x, y, size, h, mat, color );
	DrawMaterial( x + w - size, y, size, h, mat, color );
	DrawMaterial( x, y, w, size, mat, color );
	DrawMaterial( x, y + h - size, w, size, mat, color );
}


void idDeviceContext::SetCursor(int n) {
	cursor = (n < CURSOR_ARROW || n >= CURSOR_COUNT) ? CURSOR_ARROW : n;
}

void idDeviceContext::DrawCursor(float *x, float *y, float size) {
	if (*x < 0) {
		*x = 0;
	}

	if (*x >= vidWidth) {
		*x = vidWidth;
	}

	if (*y < 0) {
		*y = 0;
	}

	if (*y >= vidHeight) {
		*y = vidHeight;
	}

	SetDrawColor(colorWhite);
	// scale copies: DOOM-3 scaled the GUI's own cursor position in place,
	// harmless only while the scale was 1
	float sx = *x, sy = *y, sw = size, sh = size;
	AdjustCoords(&sx, &sy, &sw, &sh);
	DrawStretchPic( sx, sy, sw, sh, 0, 0, 1, 1, cursorImages[cursor]);
}
/*
 =======================================================================================================================
 =======================================================================================================================
 */

void idDeviceContext::PaintChar(float x,float y,float width,float height,float scale,float	s,float	t,float	s2,float t2,const idMaterial *hShader) {
	float	w, h;
	w = width * scale;
	h = height * scale;

	if (ClippedCoords(&x, &y, &w, &h, &s, &t, &s2, &t2)) {
		return;
	}

	AdjustCoords(&x, &y, &w, &h);
	DrawStretchPic(x, y, w, h, s, t, s2, t2, hShader);
}


void idDeviceContext::SetFontByScale(float scale) {
	if (scale <= gui_smallFontLimit.GetFloat()) {
		useFont = &activeFont->fontInfoSmall;
		activeFont->maxHeight = activeFont->maxHeightSmall;
		activeFont->maxWidth = activeFont->maxWidthSmall;
	} else if (scale <= gui_mediumFontLimit.GetFloat()) {
		useFont = &activeFont->fontInfoMedium;
		activeFont->maxHeight = activeFont->maxHeightMedium;
		activeFont->maxWidth = activeFont->maxWidthMedium;
	} else {
		useFont = &activeFont->fontInfoLarge;
		activeFont->maxHeight = activeFont->maxHeightLarge;
		activeFont->maxWidth = activeFont->maxWidthLarge;
	}
}

int idDeviceContext::DrawText(float x, float y, float scale, idVec4 color, const char *text, float adjust, int limit, int style, int cursor) {
	int			len, count;
	idVec4		newColor;
	const glyphInfo_t *glyph;
	float		useScale;
	SetFontByScale(scale);
	useScale = scale * useFont->glyphScale;
	count = 0;
	if ( text && color.w != 0.0f ) {
		const unsigned char	*s = (const unsigned char*)text;
		SetDrawColor(color);
		memcpy(&newColor[0], &color[0], sizeof(idVec4));
		len = strlen(text);
		if (limit > 0 && len > limit) {
			len = limit;
		}

		while (s && *s && count < len) {
			if ( *s < GLYPH_START || *s > GLYPH_END ) {
				s++;
				continue;
			}
			glyph = &useFont->glyphs[*s];

			//
			// int yadj = Assets.textFont.glyphs[text[i]].bottom +
			// Assets.textFont.glyphs[text[i]].top; float yadj = scale *
			// (Assets.textFont.glyphs[text[i]].imageHeight -
			// Assets.textFont.glyphs[text[i]].height);
			//
			if ( idStr::IsColor((const char*)s) ) {
				if ( *(s+1) == C_COLOR_DEFAULT ) {
					newColor = color;
				} else {
					newColor = idStr::ColorForIndex( *(s+1) );
					newColor[3] = color[3];
				}
				if (cursor == count || cursor == count+1) {
					float partialSkip = ((glyph->xSkip * useScale) + adjust) / 5.0f;
					if ( cursor == count ) {
						partialSkip *= 2.0f;
					} else {
						SetDrawColor(newColor);
					}
					DrawEditCursor(x - partialSkip, y, scale);
				}
				SetDrawColor(newColor);
				s += 2;
				count += 2;
				continue;
			} else {
				float yadj = useScale * glyph->top;
				PaintChar(x,y - yadj,glyph->imageWidth,glyph->imageHeight,useScale,glyph->s,glyph->t,glyph->s2,glyph->t2,glyph->glyph);

				if (cursor == count) {
					DrawEditCursor(x, y, scale);
				}
				x += (glyph->xSkip * useScale) + adjust;
				s++;
				count++;
			}
		}
		if (cursor == len) {
			DrawEditCursor(x, y, scale);
		}
	}
	return count;
}

void idDeviceContext::SetSize(float width, float height) {
	vidWidth = VIRTUAL_WIDTH;
	vidHeight = VIRTUAL_HEIGHT;
	aspectWidth = width;
	aspectHeight = height;
	xScale = yScale = 0.0f;
	if ( width != 0.0f && height != 0.0f ) {
		// DOOM-3 left the virtual-to-pixel step to the renderer; here the
		// GUI draws straight into its render target
		xScale = vidWidth * ( 1.0f / width ) * targetXScale;
		yScale = vidHeight * ( 1.0f / height ) * targetYScale;
	}
}

void idDeviceContext::SetTargetSize(float width, float height) {
	targetXScale = width / VIRTUAL_WIDTH;
	targetYScale = height / VIRTUAL_HEIGHT;
	SetSize( aspectWidth, aspectHeight );
}

int idDeviceContext::CharWidth( const char c, float scale ) {
	glyphInfo_t *glyph;
	float		useScale;
	SetFontByScale(scale);
	fontInfo_t	*font = useFont;
	useScale = scale * font->glyphScale;
	glyph = &font->glyphs[(const unsigned char)c];
	return idMath::FtoiFast( glyph->xSkip * useScale );
}

int idDeviceContext::TextWidth( const char *text, float scale, int limit ) {
	int i, width;

	SetFontByScale( scale );
	const glyphInfo_t *glyphs = useFont->glyphs;

	if ( text == NULL ) {
		return 0;
	}

	width = 0;
	if ( limit > 0 ) {
		for ( i = 0; text[i] != '\0' && i < limit; i++ ) {
			if ( idStr::IsColor( text + i ) ) {
				i++;
			} else {
				width += glyphs[((const unsigned char *)text)[i]].xSkip;
			}
		}
	} else {
		for ( i = 0; text[i] != '\0'; i++ ) {
			if ( idStr::IsColor( text + i ) ) {
				i++;
			} else {
				width += glyphs[((const unsigned char *)text)[i]].xSkip;
			}
		}
	}
	return idMath::FtoiFast( scale * useFont->glyphScale * width );
}

int idDeviceContext::TextHeight(const char *text, float scale, int limit) {
	int			len, count;
	float		max;
	glyphInfo_t *glyph;
	float		useScale;
	const char	*s = text;
	SetFontByScale(scale);
	fontInfo_t	*font = useFont;

	useScale = scale * font->glyphScale;
	max = 0;
	if (text) {
		len = strlen(text);
		if (limit > 0 && len > limit) {
			len = limit;
		}

		count = 0;
		while (s && *s && count < len) {
			if ( idStr::IsColor(s) ) {
				s += 2;
				continue;
			}
			else {
				glyph = &font->glyphs[*(const unsigned char*)s];
				if (max < glyph->height) {
					max = glyph->height;
				}

				s++;
				count++;
			}
		}
	}

	return idMath::FtoiFast( max * useScale );
}

int idDeviceContext::MaxCharWidth(float scale) {
	SetFontByScale(scale);
	float useScale = scale * useFont->glyphScale;
	return idMath::FtoiFast( activeFont->maxWidth * useScale );
}

int idDeviceContext::MaxCharHeight(float scale) {
	SetFontByScale(scale);
	float useScale = scale * useFont->glyphScale;
	return idMath::FtoiFast( activeFont->maxHeight * useScale );
}

const idMaterial *idDeviceContext::GetScrollBarImage(int index) {
	if (index >= SCROLLBAR_HBACK && index < SCROLLBAR_COUNT) {
		return scrollBarImages[index];
	}
	return scrollBarImages[SCROLLBAR_HBACK];
}

// this only supports left aligned text
idRegion *idDeviceContext::GetTextRegion(const char *text, float textScale, idRectangle rectDraw, float xStart, float yStart) {
#if 0
	const char	*p, *textPtr, *newLinePtr;
	char		buff[1024];
	int			len, textWidth, newLine, newLineWidth;
	float		y;

	float charSkip = MaxCharWidth(textScale) + 1;
	float lineSkip = MaxCharHeight(textScale);

	textWidth = 0;
	newLinePtr = NULL;
#endif
	return NULL;
/*
	if (text == NULL) {
		return;
	}

	textPtr = text;
	if (*textPtr == '\0') {
		return;
	}

	y = lineSkip + rectDraw.y + yStart; 
	len = 0;
	buff[0] = '\0';
	newLine = 0;
	newLineWidth = 0;
	p = textPtr;

	textWidth = 0;
	while (p) {
		if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\0') {
			newLine = len;
			newLinePtr = p + 1;
			newLineWidth = textWidth;
		}

		if ((newLine && textWidth > rectDraw.w) || *p == '\n' || *p == '\0') {
			if (len) {

				float x = rectDraw.x ;
				
				buff[newLine] = '\0';
				DrawText(x, y, textScale, color, buff, 0, 0, 0);
				if (!wrap) {
					return;
				}
			}

			if (*p == '\0') {
				break;
			}

			y += lineSkip + 5;
			p = newLinePtr;
			len = 0;
			newLine = 0;
			newLineWidth = 0;
			continue;
		}

		buff[len++] = *p++;
		buff[len] = '\0';
		textWidth = TextWidth( buff, textScale, -1 );
	}
*/
}

void idDeviceContext::DrawEditCursor( float x, float y, float scale ) {
	// DOOM-3 blinks on com_ticNumber (60 Hz); the GUI's own time keeps a
	// replay identical
	if ( ( drawTime / 267 ) & 1 ) {
		return;
	}
	SetFontByScale(scale);
	float useScale = scale * useFont->glyphScale;
	const glyphInfo_t *glyph2 = &useFont->glyphs[(overStrikeMode) ? '_' : '|'];
	float	yadj = useScale * glyph2->top;
 	PaintChar(x, y - yadj,glyph2->imageWidth,glyph2->imageHeight,useScale,glyph2->s,glyph2->t,glyph2->s2,glyph2->t2,glyph2->glyph);
}

int idDeviceContext::DrawText( const char *text, float textScale, int textAlign, idVec4 color, idRectangle rectDraw, bool wrap, int cursor, bool calcOnly, idList<int> *breaks, int limit ) {
	const char	*p, *textPtr, *newLinePtr;
	char		buff[1024];
	int			len, newLine, newLineWidth, count;
	float		y;
	float		textWidth;

	float		charSkip = MaxCharWidth( textScale ) + 1;
	float		lineSkip = MaxCharHeight( textScale );

	float		cursorSkip = ( cursor >= 0 ? charSkip : 0 );

	bool		lineBreak, wordBreak;

	SetFontByScale( textScale );

	textWidth = 0;
	newLinePtr = NULL;

	if (!calcOnly && !(text && *text)) {
		if (cursor == 0) {
			SetDrawColor(color);
			DrawEditCursor(rectDraw.x, lineSkip + rectDraw.y, textScale);
		}
		return idMath::FtoiFast( rectDraw.w / charSkip );
	}

	textPtr = text;

	y = lineSkip + rectDraw.y; 
	len = 0;
	buff[0] = '\0';
	newLine = 0;
	newLineWidth = 0;
	p = textPtr;

	if ( breaks ) {
		breaks->Append(0);
	}
	count = 0;
	textWidth = 0;
	lineBreak = false;
	wordBreak = false;
	while (p) {

		if ( *p == '\n' || *p == '\r' || *p == '\0' ) {
			lineBreak = true;
			if ((*p == '\n' && *(p + 1) == '\r') || (*p == '\r' && *(p + 1) == '\n')) {
				p++;
			}
		}

		int nextCharWidth = ( idStr::CharIsPrintable(*p) ? CharWidth( *p, textScale ) : cursorSkip );
		// FIXME: this is a temp hack until the guis can be fixed not not overflow the bounding rectangles
		//		  the side-effect is that list boxes and edit boxes will draw over their scroll bars
		//	The following line and the !linebreak in the if statement below should be removed
		nextCharWidth = 0;

		if ( !lineBreak && ( textWidth + nextCharWidth ) > rectDraw.w ) {
			// The next character will cause us to overflow, if we haven't yet found a suitable
			// break spot, set it to be this character
			if ( len > 0 && newLine == 0 ) {
				newLine = len;
				newLinePtr = p;
				newLineWidth = textWidth;
			}
			wordBreak = true;
		} else if ( lineBreak || ( wrap && (*p == ' ' || *p == '\t') ) ) {
			// The next character is in view, so if we are a break character, store our position
			newLine = len;
			newLinePtr = p + 1;
			newLineWidth = textWidth;
		}

		if ( lineBreak || wordBreak ) {
			float x = rectDraw.x;

			if (textAlign == ALIGN_RIGHT) {
				x = rectDraw.x + rectDraw.w - newLineWidth;
			} else if (textAlign == ALIGN_CENTER) {
				x = rectDraw.x + (rectDraw.w - newLineWidth) / 2;
			}

			if ( wrap || newLine > 0 ) {
				buff[newLine] = '\0';

				// This is a special case to handle breaking in the middle of a word.
				// if we didn't do this, the cursor would appear on the end of this line
				// and the beginning of the next.
				if ( wordBreak && cursor >= newLine && newLine == len ) {
					cursor++;
				}
			}

			if (!calcOnly) {
				count += DrawText(x, y, textScale, color, buff, 0, 0, 0, cursor);
			}

			if ( cursor < newLine ) {
				cursor = -1;
			} else if ( cursor >= 0 ) {
				cursor -= ( newLine + 1 );
			}

			if ( !wrap ) {
				return newLine;
			}

			if ( ( limit && count > limit ) || *p == '\0' ) {
				break;
			}

			y += lineSkip + 5;

			if ( !calcOnly && y > rectDraw.Bottom() ) {
				break;
			}

			p = newLinePtr;

			if (breaks) {
				breaks->Append(p - text);
			}

			len = 0;
			newLine = 0;
			newLineWidth = 0;
			textWidth = 0;
			lineBreak = false;
			wordBreak = false;
			continue;
		}

		buff[len++] = *p++;
		buff[len] = '\0';
		// update the width
		if ( *( buff + len - 1 ) != C_COLOR_ESCAPE && (len <= 1 || *( buff + len - 2 ) != C_COLOR_ESCAPE)) {
			textWidth += textScale * useFont->glyphScale * useFont->glyphs[ (const unsigned char)*( buff + len - 1 ) ].xSkip;
		}
	}

	return idMath::FtoiFast( rectDraw.w / charSkip );
}

/*
=============
idRectangle::String
=============
*/
char *idRectangle::String( void ) const {
	static	int		index = 0;
	static	char	str[ 8 ][ 48 ];
	char	*s;

	// use an array so that multiple toString's won't collide
	s = str[ index ];
	index = (index + 1)&7;

	sprintf( s, "%.2f %.2f %.2f %.2f", x, y, w, h );

	return s;
}

} // namespace idgui
