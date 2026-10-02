/*
===========================================================================
snd_reverb.c: zone reverb on the DMA mix.

A Freeverb-style network (Jezar at Dreampoint's Freeverb, public domain):
eight parallel low-pass feedback comb filters into four series all-pass
filters, per channel, the right channel's delays spread by 23 samples. The
tunings are Freeverb's, scaled from 44.1 kHz to the mixer's rate. This file
is written fresh for ioquake3's integer paint buffer; it carries Freeverb's
structure and tuning numbers, not its source.

The cgame sets it with CG_OAX_S_SETREVERB( preset, decay, wet ) while the
listener is in a reverb zone (oax token "reverb"). Presets are lines of
`name decay damp wet` in sound/reverb.txt: decay is the RT60 time in
seconds, damp 0..1 high-frequency damping, wet the level added on top of
the untouched dry signal. A decay or wet above 0 in the call overrides the
preset's. Turning it off lets the tail ring out first.

Audio is not gameplay: nothing here needs to be deterministic across
builds. While reverb has never been switched on, S_PaintChannels takes its
stock path exactly.
===========================================================================
*/

#include "client.h"
#include "snd_local.h"
#include "../qcommon/oax.h"

#define RV_COMBS      8
#define RV_ALLPASSES  4
#define RV_SPREAD     23
#define RV_MAXRATE    96000
#define RV_MAXCOMB    ( ( 1617 + RV_SPREAD ) * RV_MAXRATE / 44100 + 2 )
#define RV_MAXALLPASS ( ( 556 + RV_SPREAD ) * RV_MAXRATE / 44100 + 2 )
#define RV_INGAIN     0.015f
#define RV_MAXPRESETS 32

static const int combTuning[RV_COMBS] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int allpassTuning[RV_ALLPASSES] = { 556, 441, 341, 225 };

typedef struct {
	float buf[RV_MAXCOMB];
	int   len, idx;
	float store;
} rvComb_t;

typedef struct {
	float buf[RV_MAXALLPASS];
	int   len, idx;
} rvAllpass_t;

typedef struct {
	char  name[32];
	float decay, damp, wet;
} rvPreset_t;

typedef struct {
	rvComb_t    combs[2][RV_COMBS];
	rvAllpass_t allpasses[2][RV_ALLPASSES];
} rvState_t;

// The mixer repaints its mix-ahead window every update (S_GetSoundtime puts
// s_paintedtime back near the play cursor), so the same sample times are
// painted several times. The network must step through each sample once:
// "committed" is its state at rvCommitTime, advanced over the dry input
// recorded for samples that will never be repainted; each paint runs a
// scratch copy from there.
#define RV_HIST       65536     // dry input history, sample frames (power of 2)

static rvState_t   committed, work;
static float       hist[RV_HIST];
static int         rvCommitTime, rvHistEnd;
static int         rvRate;
static float       rvFeedback[RV_COMBS], rvDamp, rvWet;
static qboolean    rvOn;            // input is fed in
static int         rvOffTime;       // commit time when it was switched off
static qboolean    rvEverUsed;
static qboolean    rvTimeValid;

static rvPreset_t  presets[RV_MAXPRESETS];
static int         numPresets = -1;

static void S_ReverbLoadPresets( void ) {
	char       *text;
	const char *p, *tok;
	union { char *c; void *v; } buf;
	rvPreset_t *r;

	numPresets = 0;
	// a usable room even without the file
	Q_strncpyz( presets[0].name, "default", sizeof( presets[0].name ) );
	presets[0].decay = 1.5f;
	presets[0].damp = 0.5f;
	presets[0].wet = 0.3f;
	numPresets = 1;

	if ( FS_ReadFile( "sound/reverb.txt", &buf.v ) <= 0 ) {
		return;
	}
	text = buf.c;
	p = text;
	while ( numPresets < RV_MAXPRESETS ) {
		tok = COM_ParseExt( (char **)&p, qtrue );
		if ( !tok[0] ) {
			break;
		}
		r = &presets[numPresets];
		Q_strncpyz( r->name, tok, sizeof( r->name ) );
		r->decay = atof( COM_ParseExt( (char **)&p, qfalse ) );
		r->damp = atof( COM_ParseExt( (char **)&p, qfalse ) );
		r->wet = atof( COM_ParseExt( (char **)&p, qfalse ) );
		SkipRestOfLine( (char **)&p );
		if ( !Q_stricmp( r->name, "default" ) ) {
			presets[0] = *r;
		} else {
			numPresets++;
		}
	}
	FS_FreeFile( buf.v );
}

static const rvPreset_t *S_ReverbPreset( const char *name ) {
	int i;

	if ( numPresets < 0 ) {
		S_ReverbLoadPresets();
	}
	for ( i = 0; i < numPresets; i++ ) {
		if ( !Q_stricmp( presets[i].name, name ) ) {
			return &presets[i];
		}
	}
	return &presets[0];
}

static void S_ReverbSetupRate( void ) {
	int c, i;

	rvRate = dma.speed > 0 ? dma.speed : 22050;
	if ( rvRate > RV_MAXRATE ) {
		rvRate = RV_MAXRATE;
	}
	Com_Memset( &committed, 0, sizeof( committed ) );
	for ( c = 0; c < 2; c++ ) {
		for ( i = 0; i < RV_COMBS; i++ ) {
			committed.combs[c][i].len = ( combTuning[i] + c * RV_SPREAD ) * rvRate / 44100;
		}
		for ( i = 0; i < RV_ALLPASSES; i++ ) {
			committed.allpasses[c][i].len = ( allpassTuning[i] + c * RV_SPREAD ) * rvRate / 44100;
		}
	}
	rvTimeValid = qfalse;
}

/*
=================
S_ReverbSet

preset "" with decay 0 switches reverb off (the tail rings out).
=================
*/
void S_ReverbSet( const char *preset, float decay, float wet ) {
	const rvPreset_t *p;
	float             damp;
	int               i;

	if ( !preset ) {
		preset = "";
	}
	if ( !preset[0] && decay <= 0 ) {
		if ( rvOn ) {
			rvOn = qfalse;
			rvOffTime = rvCommitTime;
		}
		Com_DebugSet( "s_reverb", "off" );
		return;
	}
	p = S_ReverbPreset( preset[0] ? preset : "default" );
	if ( decay <= 0 ) {
		decay = p->decay;
	}
	if ( wet <= 0 ) {
		wet = p->wet;
	}
	damp = p->damp;
	if ( decay < 0.05f ) {
		decay = 0.05f;
	}

	if ( !rvEverUsed || rvRate != dma.speed ) {
		S_ReverbSetupRate();
	}
	rvEverUsed = qtrue;

	// comb feedback for an RT60 of `decay`: 60 dB down after decay seconds
	for ( i = 0; i < RV_COMBS; i++ ) {
		float g = (float)pow( 10.0, -3.0 * ( (double)committed.combs[0][i].len / rvRate ) / decay );
		rvFeedback[i] = g > 0.98f ? 0.98f : g;
	}
	rvDamp = Com_Clamp( 0.0f, 0.95f, damp ) * 0.4f;     // Freeverb scaledamp
	rvWet = Com_Clamp( 0.0f, 2.0f, wet ) * 3.0f;        // Freeverb scalewet
	rvOn = qtrue;
	Com_DebugSet( "s_reverb", va( "%s %g %g %g", p->name, decay, damp, wet ) );
}

void S_ReverbReset( void ) {
	rvOn = qfalse;
	rvOffTime = rvCommitTime - RV_MAXRATE * 8;
	if ( rvEverUsed ) {
		S_ReverbSetupRate();
	}
}

qboolean S_ReverbActive( void ) {
	return rvOn || ( rvEverUsed && rvCommitTime - rvOffTime < rvRate * 4 );
}

// one sample frame through the network; returns the stereo wet signal
static ID_INLINE void S_ReverbStep( rvState_t *st, float in, float out[2] ) {
	int   c, i;
	float acc, x, y;
	float damp1 = rvDamp, damp2 = 1.0f - rvDamp;

	for ( c = 0; c < 2; c++ ) {
		acc = 0;
		for ( i = 0; i < RV_COMBS; i++ ) {
			rvComb_t *cb = &st->combs[c][i];
			y = cb->buf[cb->idx];
			cb->store = y * damp2 + cb->store * damp1;
			cb->buf[cb->idx] = in + cb->store * rvFeedback[i];
			if ( ++cb->idx >= cb->len ) {
				cb->idx = 0;
			}
			acc += y;
		}
		for ( i = 0; i < RV_ALLPASSES; i++ ) {
			rvAllpass_t *ap = &st->allpasses[c][i];
			x = ap->buf[ap->idx];
			ap->buf[ap->idx] = acc + x * 0.5f;
			acc = x - acc;
			if ( ++ap->idx >= ap->len ) {
				ap->idx = 0;
			}
		}
		out[c] = acc;
	}
}

/*
=================
S_ReverbProcess

Adds the wet signal to count paint-buffer samples starting at sample time
start, in place.
=================
*/
void S_ReverbProcess( portable_samplepair_t *pb, int start, int count ) {
	int   n, t;
	float in, out[2];

	if ( !S_ReverbActive() ) {
		return;
	}
	if ( !rvTimeValid || start < rvCommitTime || start - rvCommitTime >= RV_HIST ) {
		// first use, or the sound clock jumped: start over from here
		Com_Memcpy( &work, &committed, sizeof( work ) );
		rvCommitTime = start;
		rvTimeValid = qtrue;
	} else if ( start != rvHistEnd ) {
		// a new paint that repaints (or skips past) the last one: advance the
		// committed state over the input that is final now, then run a copy
		for ( t = rvCommitTime; t < start; t++ ) {
			S_ReverbStep( &committed, t < rvHistEnd ? hist[t & ( RV_HIST - 1 )] : 0.0f, out );
		}
		rvCommitTime = start;
		Com_Memcpy( &work, &committed, sizeof( work ) );
	}
	// else: the next chunk of the same paint (S_PaintChannels paints in
	// PAINTBUFFER_SIZE pieces); carry on with the scratch state

	for ( n = 0; n < count; n++ ) {
		in = rvOn ? ( (float)pb[n].left + (float)pb[n].right ) * RV_INGAIN : 0.0f;
		hist[( start + n ) & ( RV_HIST - 1 )] = in;
		S_ReverbStep( &work, in, out );
		pb[n].left += (int)( out[0] * rvWet );
		pb[n].right += (int)( out[1] * rvWet );
	}
	rvHistEnd = start + count;
}

/*
=================
CL_OAXReverbCalls: cgame syscalls in the S block that this file owns
=================
*/
qboolean CL_OAXReverbCalls( intptr_t *args, intptr_t *ret ) {
	switch ( args[0] ) {
	case CG_OAX_S_SETREVERB:
		S_ReverbSet( VMA( 1 ), VMF( 2 ), VMF( 3 ) );
		*ret = 0;
		return qtrue;
	}
	return qfalse;
}

/*
=================
S_MeterPaint

A test meter on the final mix (s_meter 1; setting it again restarts it):
publishes "s_meter <peak> <tail ms> <peak ms>": the loudest sample, how long after
it the mix stays above -40 dB of that peak, in sample time, so the reading
does not depend on how fast the host plays the audio; and when the peak came.
=================
*/
void S_MeterPaint( const portable_samplepair_t *pb, int start, int count ) {
	static cvar_t *s_meter;
	static int     peak, peakTime, lastTime, firstTime;
	int            n, v, l, r;

	if ( !s_meter ) {
		s_meter = Cvar_Get( "s_meter", "0", CVAR_TEMP );
	}
	if ( s_meter->modified ) {
		s_meter->modified = qfalse;
		peak = peakTime = lastTime = 0;
		firstTime = start;
	}
	if ( !s_meter->integer ) {
		return;
	}
	for ( n = 0; n < count; n++ ) {
		l = pb[n].left >> 8;
		r = pb[n].right >> 8;
		v = abs( l ) > abs( r ) ? abs( l ) : abs( r );
		if ( v > peak ) {
			peak = v;
			peakTime = start + n;
		}
		if ( peak > 0 && v * 100 > peak && start + n > lastTime ) {
			lastTime = start + n;
		}
	}
	Com_DebugSet( "s_meter", va( "%i %i %i", peak, peak && dma.speed ? (int)( ( lastTime - peakTime ) * 1000LL / dma.speed ) : 0,
		peak && dma.speed ? (int)( ( peakTime - firstTime ) * 1000LL / dma.speed ) : 0 ) );
}
