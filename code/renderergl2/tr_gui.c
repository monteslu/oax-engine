/*
===========================================================================
tr_gui.c: render targets for in-world GUIs (oax extension, qcommon/oax.h).

The client draws each visible GUI with the stock 2D calls (SetColor,
StretchPic) plus DrawQuad, between BeginGuiTarget and EndGuiTarget. The 2D
commands then land in the GUI's own texture instead of the screen, and a
shader stage "map $gui" samples that texture on the entity whose
refEntityExt_t carries the GUI handle (R_BindGuiImage, from tr_shade.c).

One framebuffer object serves every GUI: each target attaches its texture
before drawing. A map that uses no GUI never creates either.
===========================================================================
*/

#include "tr_local.h"
#include "tr_fbo.h"

/*
=============
R_GuiBlankImage

What a "map $gui" stage shows before its entity's GUI has been drawn.
=============
*/
image_t *R_GuiBlankImage( void ) {
	if ( !tr.guiBlankImage ) {
		byte data[4 * 4 * 4];
		int i;

		for ( i = 0; i < 16; i++ ) {
			data[i * 4 + 0] = data[i * 4 + 1] = data[i * 4 + 2] = 0;
			data[i * 4 + 3] = 255;
		}
		tr.guiBlankImage = R_CreateImage( "*guiblank", data, 4, 4, IMGTYPE_COLORALPHA, IMGFLAG_NONE, 0 );
	}
	return tr.guiBlankImage;
}

/*
=============
R_BindGuiImage
=============
*/
void R_BindGuiImage( int tmu ) {
	image_t *image = NULL;
	int h = backEnd.currentEntity ? backEnd.currentEntity->guiHandle : 0;

	if ( h >= 1 && h <= MAX_GUI_TARGETS ) {
		image = tr.guiImages[h - 1];
	}
	GL_BindToTMU( image ? image : R_GuiBlankImage(), tmu );
}

/*
=============
RE_AddRefEntityToSceneExt
=============
*/
extern int r_numentities;

void RE_AddRefEntityToSceneExt( const refEntity_t *ent, const refEntityExt_t *ext ) {
	int n = r_numentities;

	RE_AddRefEntityToScene( ent );
	if ( r_numentities > n && ext ) {
		backEndData->entities[n].guiHandle = ext->guiHandle;
	}
}

/*
=============
RE_BeginGuiTarget
=============
*/
qboolean RE_BeginGuiTarget( int handle, int width, int height ) {
	guiTargetCommand_t *cmd;
	image_t *image;
	int maxSize;

	if ( !tr.registered || !glRefConfig.framebufferObject ) {
		return qfalse;
	}
	if ( handle < 1 || handle > MAX_GUI_TARGETS ) {
		return qfalse;
	}
	maxSize = MIN( glRefConfig.maxRenderbufferSize, 2048 );
	width = MAX( 16, MIN( width, maxSize ) );
	height = MAX( 16, MIN( height, maxSize ) );

	image = tr.guiImages[handle - 1];
	if ( !image || image->width != width || image->height != height ) {
		image = R_CreateImage( va( "*gui%d_%dx%d", handle, width, height ), NULL, width, height,
			IMGTYPE_COLORALPHA, IMGFLAG_NO_COMPRESSION | IMGFLAG_CLAMPTOEDGE, GL_RGBA8 );
		tr.guiImages[handle - 1] = image;
	}
	if ( !tr.guiFbo ) {
		tr.guiFbo = FBO_Create( "_gui", width, height );
	}

	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return qfalse;
	}
	cmd->commandId = RC_BEGIN_GUI_TARGET;
	cmd->image = image;
	return qtrue;
}

/*
=============
RE_EndGuiTarget
=============
*/
void RE_EndGuiTarget( void ) {
	guiTargetCommand_t *cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		// RB_DrawSurfs, RB_DrawBuffer and RB_SwapBuffers drop the target anyway
		return;
	}
	cmd->commandId = RC_END_GUI_TARGET;
	cmd->image = NULL;
}

/*
=============
RE_DrawQuad
=============
*/
void RE_DrawQuad( const float *xy, const float *st, qhandle_t hShader ) {
	stretchQuadCommand_t *cmd;

	if ( !tr.registered ) {
		return;
	}
	cmd = R_GetCommandBuffer( sizeof( *cmd ) );
	if ( !cmd ) {
		return;
	}
	cmd->commandId = RC_STRETCH_QUAD;
	cmd->shader = R_GetShaderByHandle( hShader );
	Com_Memcpy( cmd->xy, xy, sizeof( cmd->xy ) );
	Com_Memcpy( cmd->st, st, sizeof( cmd->st ) );
}

/*
=============
RB_BeginGuiTarget
=============
*/
const void *RB_BeginGuiTarget( const void *data ) {
	const guiTargetCommand_t *cmd = (const guiTargetCommand_t *)data;

	if ( tess.numIndexes ) {
		RB_EndSurface();
	}
	if ( cmd->image && tr.guiFbo ) {
		tr.guiFbo->width = cmd->image->width;
		tr.guiFbo->height = cmd->image->height;
		FBO_Bind( tr.guiFbo );
		FBO_AttachImage( tr.guiFbo, cmd->image, GL_COLOR_ATTACHMENT0, 0 );
		backEnd.guiTarget = tr.guiFbo;

		// the target may have changed size behind the same FBO
		backEnd.projection2D = qfalse;
		RB_SetGL2D();
		qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
		qglClear( GL_COLOR_BUFFER_BIT );
	}
	return (const void *)( cmd + 1 );
}

/*
=============
RB_EndGuiTarget
=============
*/
const void *RB_EndGuiTarget( const void *data ) {
	const guiTargetCommand_t *cmd = (const guiTargetCommand_t *)data;

	RB_FinishGuiTarget();
	return (const void *)( cmd + 1 );
}

/*
=============
RB_FinishGuiTarget

Flushes the GUI's last draws, mipmaps its texture (it is seen from any
distance) and sends 2D drawing back to the screen.
=============
*/
void RB_FinishGuiTarget( void ) {
	image_t *image;

	if ( !backEnd.guiTarget ) {
		return;
	}
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}
	image = backEnd.guiTarget->colorImage[0];
	backEnd.guiTarget = NULL;
	backEnd.projection2D = qfalse;
	FBO_Bind( NULL );
	if ( image ) {
		qglGenerateTextureMipmapEXT( image->texnum, GL_TEXTURE_2D );
		qglTextureParameterfEXT( image->texnum, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	}
}

/*
=============
RB_StretchQuad

RB_StretchPic with four free corners (transformed GUI windows).
=============
*/
const void *RB_StretchQuad( const void *data ) {
	const stretchQuadCommand_t *cmd = (const stretchQuadCommand_t *)data;
	shader_t *shader;
	int numVerts, numIndexes, i;
	uint16_t color[4];

	if ( glRefConfig.framebufferObject ) {
		FBO_Bind( backEnd.guiTarget ? backEnd.guiTarget : tr.renderFbo );
	}

	RB_SetGL2D();

	shader = cmd->shader;
	if ( shader != tess.shader ) {
		if ( tess.numIndexes ) {
			RB_EndSurface();
		}
		backEnd.currentEntity = &backEnd.entity2D;
		RB_BeginSurface( shader, 0, 0 );
	}

	RB_CHECKOVERFLOW( 4, 6 );
	numVerts = tess.numVertexes;
	numIndexes = tess.numIndexes;

	tess.numVertexes += 4;
	tess.numIndexes += 6;

	tess.indexes[ numIndexes ] = numVerts + 3;
	tess.indexes[ numIndexes + 1 ] = numVerts + 0;
	tess.indexes[ numIndexes + 2 ] = numVerts + 2;
	tess.indexes[ numIndexes + 3 ] = numVerts + 2;
	tess.indexes[ numIndexes + 4 ] = numVerts + 0;
	tess.indexes[ numIndexes + 5 ] = numVerts + 1;

	VectorScale4( backEnd.color2D, 257, color );

	for ( i = 0; i < 4; i++ ) {
		VectorCopy4( color, tess.color[ numVerts + i ] );
		tess.xyz[ numVerts + i ][0] = cmd->xy[i * 2 + 0];
		tess.xyz[ numVerts + i ][1] = cmd->xy[i * 2 + 1];
		tess.xyz[ numVerts + i ][2] = 0;
		tess.texCoords[ numVerts + i ][0] = cmd->st[i * 2 + 0];
		tess.texCoords[ numVerts + i ][1] = cmd->st[i * 2 + 1];
	}

	return (const void *)( cmd + 1 );
}
