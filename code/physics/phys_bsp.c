/*
===========================================================================
phys_bsp.c: static Box3D collision from the loaded map, and height fields.

Brushes become convex hulls: each side's plane is clipped by every other
side (in double precision) and the corner points go to b3CreateHull.
Curved surfaces (patches) are tessellated at a fixed level, triangle soups
are taken as they are, and both become one triangle mesh per world, wound
so the triangles face along the map's vertex normals.

The map is read from its .bsp (the one the collision model loaded), so the
server and the client build the same shapes in the same order: same
handles, same hash.
===========================================================================
*/

#include "phys_local.h"
#include "../qcommon/cm_local.h"

#define PHYS_PATCH_SUBDIV	4		// segments per bezier span
#define PHYS_HULL_EPSILON	0.01	// units: points closer than this are one point
#define PHYS_MAX_SIDES		64

typedef struct {
	byte			*buf;
	int				len;
	const dshader_t	*shaders;
	int				numShaders;
	const dplane_t	*planes;
	int				numPlanes;
	const dmodel_t	*models;
	int				numModels;
	const dbrush_t	*brushes;
	int				numBrushes;
	const dbrushside_t *sides;
	int				numSides;
	const dsurface_t *surfaces;
	int				numSurfaces;
	const drawVert_t *verts;
	int				numVerts;
	const int		*indexes;
	int				numIndexes;
} physBsp_t;

static const void *Phys_Lump( physBsp_t *b, int lump, int size, int *count ) {
	const dheader_t *h = (const dheader_t *)b->buf;
	int ofs = LittleLong( h->lumps[lump].fileofs );
	int len = LittleLong( h->lumps[lump].filelen );

	if ( ofs < 0 || len < 0 || ofs + len > b->len || len % size ) {
		*count = 0;
		return NULL;
	}
	*count = len / size;
	return b->buf + ofs;
}

// the .bsp the collision model came from (server and client load the same)
static qboolean Phys_LoadBsp( physBsp_t *b ) {
	const dheader_t *h;
	void *buf;

	Com_Memset( b, 0, sizeof( *b ) );
	if ( !cm.name[0] ) {
		Com_Printf( S_COLOR_YELLOW "physics: no map loaded\n" );
		return qfalse;
	}
	b->len = FS_ReadFile( cm.name, &buf );
	if ( b->len < (int)sizeof( dheader_t ) || !buf ) {
		Com_Printf( S_COLOR_YELLOW "physics: can't read %s\n", cm.name );
		if ( buf ) {
			FS_FreeFile( buf );
		}
		return qfalse;
	}
	b->buf = buf;
	h = (const dheader_t *)b->buf;
	if ( LittleLong( h->ident ) != BSP_IDENT ) {
		FS_FreeFile( b->buf );
		return qfalse;
	}
	// the lumps are read in place: every field the code below uses is
	// little-endian already on every target we build (x86-64, wasm32)
	b->shaders = Phys_Lump( b, LUMP_SHADERS, sizeof( dshader_t ), &b->numShaders );
	b->planes = Phys_Lump( b, LUMP_PLANES, sizeof( dplane_t ), &b->numPlanes );
	b->models = Phys_Lump( b, LUMP_MODELS, sizeof( dmodel_t ), &b->numModels );
	b->brushes = Phys_Lump( b, LUMP_BRUSHES, sizeof( dbrush_t ), &b->numBrushes );
	b->sides = Phys_Lump( b, LUMP_BRUSHSIDES, sizeof( dbrushside_t ), &b->numSides );
	b->surfaces = Phys_Lump( b, LUMP_SURFACES, sizeof( dsurface_t ), &b->numSurfaces );
	b->verts = Phys_Lump( b, LUMP_DRAWVERTS, sizeof( drawVert_t ), &b->numVerts );
	b->indexes = Phys_Lump( b, LUMP_DRAWINDEXES, sizeof( int ), &b->numIndexes );
	return qtrue;
}

/*
==============================================================================
brushes -> hulls
==============================================================================
*/

typedef struct {
	int		n;
	double	p[PHYS_MAX_SIDES + 8][3];
} physWinding_t;

static void Phys_BaseWinding( const double *n, double d, physWinding_t *w ) {
	double up[3] = { 0, 0, 0 }, right[3], org[3], v, len;
	int i, x = -1;
	double max = -1;

	for ( i = 0; i < 3; i++ ) {
		v = fabs( n[i] );
		if ( v > max ) {
			x = i;
			max = v;
		}
	}
	if ( x == 2 ) {
		up[0] = 1;
	} else {
		up[2] = 1;
	}
	v = up[0] * n[0] + up[1] * n[1] + up[2] * n[2];
	for ( i = 0; i < 3; i++ ) {
		up[i] -= v * n[i];
	}
	len = sqrt( up[0] * up[0] + up[1] * up[1] + up[2] * up[2] );
	for ( i = 0; i < 3; i++ ) {
		up[i] /= len;
		org[i] = n[i] * d;
	}
	right[0] = up[1] * n[2] - up[2] * n[1];
	right[1] = up[2] * n[0] - up[0] * n[2];
	right[2] = up[0] * n[1] - up[1] * n[0];
	for ( i = 0; i < 3; i++ ) {
		up[i] *= 131072.0;
		right[i] *= 131072.0;
	}
	w->n = 4;
	for ( i = 0; i < 3; i++ ) {
		w->p[0][i] = org[i] - right[i] + up[i];
		w->p[1][i] = org[i] + right[i] + up[i];
		w->p[2][i] = org[i] + right[i] - up[i];
		w->p[3][i] = org[i] - right[i] - up[i];
	}
}

// keep the part of w behind the plane (dot(p, n) <= d)
static void Phys_ClipWinding( physWinding_t *w, const double *n, double d ) {
	physWinding_t out;
	double dists[PHYS_MAX_SIDES + 9];
	int i, j, front = 0;

	for ( i = 0; i < w->n; i++ ) {
		dists[i] = w->p[i][0] * n[0] + w->p[i][1] * n[1] + w->p[i][2] * n[2] - d;
		if ( dists[i] > 1e-6 ) {
			front++;
		}
	}
	if ( !front ) {
		return;
	}
	dists[w->n] = dists[0];
	out.n = 0;
	for ( i = 0; i < w->n; i++ ) {
		const double *p1 = w->p[i];
		const double *p2 = w->p[( i + 1 ) % w->n];
		if ( dists[i] <= 1e-6 && out.n < PHYS_MAX_SIDES + 8 ) {
			for ( j = 0; j < 3; j++ ) out.p[out.n][j] = p1[j];
			out.n++;
		}
		if ( ( dists[i] > 1e-6 ) != ( dists[i + 1] > 1e-6 ) && out.n < PHYS_MAX_SIDES + 8 ) {
			double t = dists[i] / ( dists[i] - dists[i + 1] );
			for ( j = 0; j < 3; j++ ) out.p[out.n][j] = p1[j] + t * ( p2[j] - p1[j] );
			out.n++;
		}
	}
	*w = out;
}

// corner points of a brush; returns the count
static int Phys_BrushPoints( physBsp_t *b, const dbrush_t *brush, b3Vec3 *pts, int max ) {
	double n[PHYS_MAX_SIDES][3], d[PHYS_MAX_SIDES];
	int i, j, k, numSides = 0, numPts = 0;

	for ( i = 0; i < brush->numSides && numSides < PHYS_MAX_SIDES; i++ ) {
		int s = brush->firstSide + i;
		const dplane_t *pl;
		if ( s < 0 || s >= b->numSides || b->sides[s].planeNum < 0 || b->sides[s].planeNum >= b->numPlanes ) {
			return 0;
		}
		pl = &b->planes[b->sides[s].planeNum];
		for ( j = 0; j < 3; j++ ) n[numSides][j] = pl->normal[j];
		d[numSides] = pl->dist;
		numSides++;
	}
	for ( i = 0; i < numSides; i++ ) {
		physWinding_t w;
		Phys_BaseWinding( n[i], d[i], &w );
		for ( j = 0; j < numSides && w.n; j++ ) {
			if ( j != i ) {
				Phys_ClipWinding( &w, n[j], d[j] );
			}
		}
		for ( j = 0; j < w.n; j++ ) {
			b3Vec3 p;
			p.x = (float)w.p[j][0];
			p.y = (float)w.p[j][1];
			p.z = (float)w.p[j][2];
			for ( k = 0; k < numPts; k++ ) {
				if ( fabsf( pts[k].x - p.x ) < PHYS_HULL_EPSILON && fabsf( pts[k].y - p.y ) < PHYS_HULL_EPSILON
					&& fabsf( pts[k].z - p.z ) < PHYS_HULL_EPSILON ) {
					break;
				}
			}
			if ( k == numPts && numPts < max ) {
				pts[numPts++] = p;
			}
		}
	}
	return numPts;
}

static int Phys_AddBrushes( physBsp_t *b, b3BodyId body, int firstBrush, int numBrushes, int contentsMask,
		const b3ShapeDef *sd ) {
	b3Vec3 pts[B3_MAX_SHAPE_CAST_POINTS * 4];
	int i, added = 0;

	for ( i = 0; i < numBrushes; i++ ) {
		const dbrush_t *brush;
		b3HullData *hull;
		int n, shader;

		if ( firstBrush + i < 0 || firstBrush + i >= b->numBrushes ) {
			break;
		}
		brush = &b->brushes[firstBrush + i];
		shader = brush->shaderNum;
		if ( shader < 0 || shader >= b->numShaders || !( b->shaders[shader].contentFlags & contentsMask ) ) {
			continue;
		}
		n = Phys_BrushPoints( b, brush, pts, ARRAY_LEN( pts ) );
		if ( n < 4 ) {
			continue;
		}
		hull = b3CreateHull( pts, n, n );
		if ( !hull ) {
			continue;
		}
		b3CreateHullShape( body, sd, hull );
		b3DestroyHull( hull );
		added++;
	}
	return added;
}

/*
==============================================================================
patches and triangle soups -> one triangle mesh
==============================================================================
*/

typedef struct {
	b3Vec3	*v;
	int		numV, maxV;
	int		*idx;
	int		numI, maxI;
} physMeshBuild_t;

static void Phys_MeshGrow( physMeshBuild_t *m, int addV, int addI ) {
	if ( m->numV + addV > m->maxV ) {
		int n = ( m->numV + addV ) * 2;
		b3Vec3 *v = Z_Malloc( n * sizeof( *v ) );
		if ( m->v ) {
			Com_Memcpy( v, m->v, m->numV * sizeof( *v ) );
			Z_Free( m->v );
		}
		m->v = v;
		m->maxV = n;
	}
	if ( m->numI + addI > m->maxI ) {
		int n = ( m->numI + addI ) * 2;
		int *idx = Z_Malloc( n * sizeof( *idx ) );
		if ( m->idx ) {
			Com_Memcpy( idx, m->idx, m->numI * sizeof( *idx ) );
			Z_Free( m->idx );
		}
		m->idx = idx;
		m->maxI = n;
	}
}

// add triangle a b c, wound so its normal points along `want`
static void Phys_MeshTri( physMeshBuild_t *m, int a, int b, int c, const float *want ) {
	b3Vec3 e1 = b3Sub( m->v[b], m->v[a] ), e2 = b3Sub( m->v[c], m->v[a] );
	b3Vec3 n = b3Cross( e1, e2 );
	float d = n.x * want[0] + n.y * want[1] + n.z * want[2];

	if ( b3Dot( n, n ) < 1e-8f ) {
		return;		// degenerate
	}
	m->idx[m->numI++] = a;
	if ( d >= 0.0f ) {
		m->idx[m->numI++] = b;
		m->idx[m->numI++] = c;
	} else {
		m->idx[m->numI++] = c;
		m->idx[m->numI++] = b;
	}
}

static float Phys_Bez( float a, float b, float c, float t ) {
	float s = 1.0f - t;
	return s * s * a + 2.0f * s * t * b + t * t * c;
}

static void Phys_AddPatch( physBsp_t *b, const dsurface_t *s, physMeshBuild_t *m ) {
	int w = s->patchWidth, h = s->patchHeight;
	int spansX, spansY, cols, rows, base, x, y, i, j;
	float *normals;

	if ( w < 3 || h < 3 || !( w & 1 ) || !( h & 1 ) || s->firstVert < 0 || s->firstVert + w * h > b->numVerts ) {
		return;
	}
	spansX = ( w - 1 ) / 2;
	spansY = ( h - 1 ) / 2;
	cols = spansX * PHYS_PATCH_SUBDIV + 1;
	rows = spansY * PHYS_PATCH_SUBDIV + 1;
	Phys_MeshGrow( m, cols * rows, ( cols - 1 ) * ( rows - 1 ) * 6 );
	normals = Z_Malloc( cols * rows * 3 * sizeof( float ) );
	base = m->numV;
	for ( y = 0; y < rows; y++ ) {
		int sy = y / PHYS_PATCH_SUBDIV, ty;
		float fy;
		if ( sy >= spansY ) sy = spansY - 1;
		ty = y - sy * PHYS_PATCH_SUBDIV;
		fy = (float)ty / PHYS_PATCH_SUBDIV;
		for ( x = 0; x < cols; x++ ) {
			int sx = x / PHYS_PATCH_SUBDIV, tx;
			float fx, p[3][3], nrm[3][3], out[3], on[3];
			if ( sx >= spansX ) sx = spansX - 1;
			tx = x - sx * PHYS_PATCH_SUBDIV;
			fx = (float)tx / PHYS_PATCH_SUBDIV;
			// evaluate along x for each of the 3 control rows, then along y
			for ( j = 0; j < 3; j++ ) {
				const drawVert_t *row = &b->verts[s->firstVert + ( sy * 2 + j ) * w + sx * 2];
				for ( i = 0; i < 3; i++ ) {
					p[j][i] = Phys_Bez( row[0].xyz[i], row[1].xyz[i], row[2].xyz[i], fx );
					nrm[j][i] = Phys_Bez( row[0].normal[i], row[1].normal[i], row[2].normal[i], fx );
				}
			}
			for ( i = 0; i < 3; i++ ) {
				out[i] = Phys_Bez( p[0][i], p[1][i], p[2][i], fy );
				on[i] = Phys_Bez( nrm[0][i], nrm[1][i], nrm[2][i], fy );
			}
			m->v[m->numV].x = out[0];
			m->v[m->numV].y = out[1];
			m->v[m->numV].z = out[2];
			VectorCopy( on, normals + ( y * cols + x ) * 3 );
			m->numV++;
		}
	}
	for ( y = 0; y < rows - 1; y++ ) {
		for ( x = 0; x < cols - 1; x++ ) {
			int a = base + y * cols + x, bb = a + 1, c = a + cols, d = c + 1;
			const float *n = normals + ( y * cols + x ) * 3;
			Phys_MeshTri( m, a, bb, d, n );
			Phys_MeshTri( m, a, d, c, n );
		}
	}
	Z_Free( normals );
}

static void Phys_AddSoup( physBsp_t *b, const dsurface_t *s, physMeshBuild_t *m ) {
	int i, base;

	if ( s->firstVert < 0 || s->firstVert + s->numVerts > b->numVerts || s->firstIndex < 0
		|| s->firstIndex + s->numIndexes > b->numIndexes ) {
		return;
	}
	Phys_MeshGrow( m, s->numVerts, s->numIndexes );
	base = m->numV;
	for ( i = 0; i < s->numVerts; i++ ) {
		const drawVert_t *dv = &b->verts[s->firstVert + i];
		m->v[m->numV].x = dv->xyz[0];
		m->v[m->numV].y = dv->xyz[1];
		m->v[m->numV].z = dv->xyz[2];
		m->numV++;
	}
	for ( i = 0; i + 2 < s->numIndexes; i += 3 ) {
		int a = b->indexes[s->firstIndex + i], bb = b->indexes[s->firstIndex + i + 1], c = b->indexes[s->firstIndex + i + 2];
		float n[3];
		if ( a < 0 || bb < 0 || c < 0 || a >= s->numVerts || bb >= s->numVerts || c >= s->numVerts ) {
			continue;
		}
		VectorAdd( b->verts[s->firstVert + a].normal, b->verts[s->firstVert + bb].normal, n );
		VectorAdd( b->verts[s->firstVert + c].normal, n, n );
		Phys_MeshTri( m, base + a, base + bb, base + c, n );
	}
}

static int Phys_AddSurfaces( physWorld_t *w, physBsp_t *b, b3BodyId body, int firstSurface, int numSurfaces,
		int contentsMask, int flags, const b3ShapeDef *sd ) {
	physMeshBuild_t m;
	b3MeshDef md;
	b3MeshData *mesh;
	int i;

	Com_Memset( &m, 0, sizeof( m ) );
	for ( i = 0; i < numSurfaces; i++ ) {
		const dsurface_t *s;
		if ( firstSurface + i < 0 || firstSurface + i >= b->numSurfaces ) {
			break;
		}
		s = &b->surfaces[firstSurface + i];
		if ( s->shaderNum < 0 || s->shaderNum >= b->numShaders || !( b->shaders[s->shaderNum].contentFlags & contentsMask ) ) {
			continue;
		}
		if ( s->surfaceType == MST_PATCH && ( flags & PHYS_BSP_PATCHES ) ) {
			Phys_AddPatch( b, s, &m );
		} else if ( s->surfaceType == MST_TRIANGLE_SOUP && ( flags & PHYS_BSP_TRISOUPS ) ) {
			Phys_AddSoup( b, s, &m );
		}
	}
	if ( m.numI < 3 ) {
		if ( m.v ) Z_Free( m.v );
		if ( m.idx ) Z_Free( m.idx );
		return 0;
	}
	Com_Memset( &md, 0, sizeof( md ) );
	md.vertices = m.v;
	md.indices = m.idx;
	md.vertexCount = m.numV;
	md.triangleCount = m.numI / 3;
	md.weldVertices = true;
	md.weldTolerance = PHYS_HULL_EPSILON;
	md.identifyEdges = true;
	mesh = b3CreateMesh( &md, NULL, 0 );
	Z_Free( m.v );
	Z_Free( m.idx );
	if ( !mesh ) {
		return 0;
	}
	Phys_WorldOwn( w, mesh, 0 );
	b3CreateMeshShape( body, sd, mesh, b3Vec3_one );
	return 1;
}

/*
==================
Phys_AddBSP

The world model's brushes (contents & contentsMask) as hulls on one static
body, plus its patches and soups as one mesh when the flags ask.
==================
*/
int Phys_AddBSP( physOwner_t owner, int world, int contentsMask, int flags, const oaxPhysShapeDef_t *material ) {
	physWorld_t *w = Phys_World( owner, world );
	physBsp_t b;
	b3BodyDef bd;
	b3ShapeDef sd;
	b3BodyId body;
	int hulls, meshes = 0, handle;

	if ( !w || !Phys_LoadBsp( &b ) ) {
		return 0;
	}
	if ( b.numModels < 1 ) {
		FS_FreeFile( b.buf );
		return 0;
	}
	Phys_ShapeDefFrom( material, &sd );
	if ( !material ) {
		sd.baseMaterial.friction = 0.6f;
	}
	bd = b3DefaultBodyDef();
	bd.type = b3_staticBody;
	body = b3CreateBody( w->id, &bd );
	hulls = Phys_AddBrushes( &b, body, b.models[0].firstBrush, b.models[0].numBrushes, contentsMask, &sd );
	if ( flags & ( PHYS_BSP_PATCHES | PHYS_BSP_TRISOUPS ) ) {
		meshes = Phys_AddSurfaces( w, &b, body, b.models[0].firstSurface, b.models[0].numSurfaces, contentsMask, flags, &sd );
	}
	FS_FreeFile( b.buf );
	handle = Phys_BodyRegister( owner, world, body, material ? material->userData : 0 );
	Com_DPrintf( "physics: %s: %d brush hulls, %d meshes for world %d\n", cm.name, hulls, meshes, world );
	return handle;
}

/*
==================
Phys_BodyFromBSPModel

An inline model (*1, *2 ...: doors, platforms, movers) as one body of hulls
in map coordinates; gamecode moves it (kinematic) with SET_TARGET.
==================
*/
int Phys_BodyFromBSPModel( physOwner_t owner, int world, int inlineModel, int type, const oaxPhysShapeDef_t *material ) {
	physWorld_t *w = Phys_World( owner, world );
	physBsp_t b;
	b3BodyDef bd;
	b3ShapeDef sd;
	b3BodyId body;
	int hulls;

	if ( !w || inlineModel < 1 || !Phys_LoadBsp( &b ) ) {
		return 0;
	}
	if ( inlineModel >= b.numModels ) {
		FS_FreeFile( b.buf );
		return 0;
	}
	Phys_ShapeDefFrom( material, &sd );
	if ( !material ) {
		sd.baseMaterial.friction = 0.6f;
	}
	bd = b3DefaultBodyDef();
	bd.type = type == PHYS_BODY_DYNAMIC ? b3_dynamicBody : type == PHYS_BODY_KINEMATIC ? b3_kinematicBody : b3_staticBody;
	body = b3CreateBody( w->id, &bd );
	hulls = Phys_AddBrushes( &b, body, b.models[inlineModel].firstBrush, b.models[inlineModel].numBrushes,
		CONTENTS_SOLID | CONTENTS_PLAYERCLIP, &sd );
	FS_FreeFile( b.buf );
	if ( !hulls ) {
		b3DestroyBody( body );
		return 0;
	}
	return Phys_BodyRegister( owner, world, body, material ? material->userData : 0 );
}

/*
==================
Phys_AddHeightField

Box3D height fields are Y-up (rows along z); ours are Z-up (rows along y).
The static body is turned +90 degrees about x (local y -> world z, local z
-> world -y) and the rows are stored in reverse, so sample (i, j) lands at
origin + (i * cellX, j * cellY, height).
==================
*/
int Phys_AddHeightField( physOwner_t owner, int world, const oaxPhysHeightField_t *hf,
		const float *heights, const oaxPhysShapeDef_t *material ) {
	physWorld_t *w = Phys_World( owner, world );
	b3HeightFieldDef hd;
	b3HeightFieldData *data;
	b3BodyDef bd;
	b3ShapeDef sd;
	b3BodyId body;
	float *local;
	uint8_t *mats;
	int i, j;

	if ( !w || hf->countX < 2 || hf->countY < 2 || hf->countX > 4096 || hf->countY > 4096
		|| hf->cellSize[0] <= 0.0f || hf->cellSize[1] <= 0.0f || hf->maxHeight <= hf->minHeight ) {
		return 0;
	}
	local = Z_Malloc( hf->countX * hf->countY * sizeof( float ) );
	mats = Z_Malloc( ( hf->countX - 1 ) * ( hf->countY - 1 ) );
	for ( j = 0; j < hf->countY; j++ ) {
		for ( i = 0; i < hf->countX; i++ ) {
			local[( hf->countY - 1 - j ) * hf->countX + i] = heights[j * hf->countX + i];
		}
	}
	Com_Memset( &hd, 0, sizeof( hd ) );
	hd.heights = local;
	hd.materialIndices = mats;
	hd.scale.x = hf->cellSize[0];
	hd.scale.y = 1.0f;
	hd.scale.z = hf->cellSize[1];
	hd.countX = hf->countX;
	hd.countZ = hf->countY;
	hd.globalMinimumHeight = hf->minHeight;
	hd.globalMaximumHeight = hf->maxHeight;
	data = b3CreateHeightField( &hd );
	Z_Free( local );
	Z_Free( mats );
	if ( !data ) {
		return 0;
	}
	Phys_WorldOwn( w, data, 1 );

	bd = b3DefaultBodyDef();
	bd.type = b3_staticBody;
	bd.position.x = hf->origin[0];
	bd.position.y = hf->origin[1] + ( hf->countY - 1 ) * hf->cellSize[1];
	bd.position.z = hf->origin[2];
	bd.rotation.v.x = 0.70710678f;
	bd.rotation.v.y = 0.0f;
	bd.rotation.v.z = 0.0f;
	bd.rotation.s = 0.70710678f;
	body = b3CreateBody( w->id, &bd );
	Phys_ShapeDefFrom( material, &sd );
	if ( !material ) {
		sd.baseMaterial.friction = 0.6f;
	}
	b3CreateHeightFieldShape( body, &sd, data );
	return Phys_BodyRegister( owner, world, body, material ? material->userData : 0 );
}
