/*
===========================================================================
cl_cgame_oax_render.c: cgame syscalls of the world-rendering block
(CG_OAX_R_BASE, 1010-1019): light styles and view fog. Sky portals need
no syscall; the cgame flags its refdefs (RDF_OAX_SKYPORTAL /
RDF_OAX_UNDERSKY).

The renderer implements these through refexport_t's OAX* entries, which
stay NULL in renderers without them; the feature tokens are advertised
only when the loaded renderer reports them (CL_OAXRendererFeatures).
===========================================================================
*/

#include "client.h"
#include "../qcommon/oax.h"

static qboolean CL_OAXRenderCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case CG_OAX_R_SETLIGHTSTYLE:
		if ( re.OAXSetLightStyle ) {
			re.OAXSetLightStyle( args[1], VMF( 2 ), VMF( 3 ), VMF( 4 ) );
		}
		if ( args[1] >= 0 && args[1] < 64 ) {
			Com_DebugSetFloat( va( "r_lightstyle%d", (int)args[1] ), VMF( 2 ) );
		}
		*ret = 0;
		return qtrue;
	case CG_OAX_R_SETVIEWFOG:
		if ( args[1] ) {
			VM_CheckBlock( args[1], 3 * sizeof( float ), "SETVIEWFOG" );
		}
		if ( re.OAXSetViewFog ) {
			re.OAXSetViewFog( args[1] ? VMA( 1 ) : NULL, VMF( 2 ), VMF( 3 ), VMF( 4 ) );
		}
		Com_DebugSetFloat( "r_viewfog_density", args[1] ? VMF( 2 ) : 0.0f );
		*ret = 0;
		return qtrue;
	}
	return qfalse;
}

/*
=================
CL_DebugCvar_f

debugcvar <cvar> ...: publishes cvars as named debug values (the renderer
reports test results, such as imageprogram's, through cvars).
=================
*/
static void CL_DebugCvar_f( void ) {
	int i;

	for ( i = 1; i < Cmd_Argc(); i++ ) {
		Com_DebugSet( Cmd_Argv( i ), Cvar_VariableString( Cmd_Argv( i ) ) );
	}
}

void CL_OAXRenderInit( void ) {
	CL_OAXRegisterCgameHandler( CL_OAXRenderCalls );
	Cmd_AddCommand( "debugcvar", CL_DebugCvar_f );
}

/*
=================
CL_OAXRendererFeatures

After the renderer loads: advertise the tokens it implements.
=================
*/
void CL_OAXRendererFeatures( void ) {
	char		buf[MAX_STRING_CHARS];
	char		*p, *tok;

	if ( !re.OAXFeatures ) {
		return;
	}
	Q_strncpyz( buf, re.OAXFeatures(), sizeof( buf ) );
	Com_DebugSet( "r_oax_features", buf );
	p = buf;
	while ( 1 ) {
		tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;
		}
		OAX_AddFeature( tok );
	}
}
