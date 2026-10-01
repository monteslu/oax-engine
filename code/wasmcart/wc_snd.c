/*
===========================================================================
wasmcart platform backend: sound output.

The engine's DMA mixer paints ahead of a play cursor into dma.buffer. On a
real device a callback advances that cursor; here the cart's own clock does:
each frame the cursor moves by delta_ms worth of samples, and the samples it
passed over are copied into the host's audio ring.
===========================================================================
*/

#include <stdlib.h>
#include <string.h>

#include "../qcommon/q_shared.h"
#include "../client/snd_local.h"
#include "wc_local.h"

#define WC_DMA_FRAMES 32768   // mixer ring, stereo frames (~0.7 s at 48 kHz)

extern int16_t  wc_audio_ring[];
extern uint32_t wc_audio_write_cursor;

static qboolean snd_inited;
static int      dmapos;            // in samples (frames * channels)
static double   pendingFrames;

qboolean SNDDMA_Init( void ) {
	if ( snd_inited ) {
		return qtrue;
	}

	dma.samplebits = 16;
	dma.isfloat = qfalse;
	dma.channels = 2;
	dma.speed = WC_AUDIO_RATE;
	dma.samples = WC_DMA_FRAMES * dma.channels;
	dma.fullsamples = WC_DMA_FRAMES;
	dma.submission_chunk = 1;
	dma.buffer = calloc( 1, dma.samples * ( dma.samplebits / 8 ) );

	dmapos = 0;
	pendingFrames = 0.0;
	snd_inited = qtrue;

	Com_Printf( "wasmcart audio: %d Hz, %d channels, 16-bit\n", dma.speed, dma.channels );
	return qtrue;
}

int SNDDMA_GetDMAPos( void ) {
	return dmapos;
}

void SNDDMA_Shutdown( void ) {
	free( dma.buffer );
	dma.buffer = NULL;
	snd_inited = qfalse;
}

void SNDDMA_BeginPainting( void ) { }
void SNDDMA_Submit( void ) { }

void SNDDMA_StartCapture( void ) { }
int SNDDMA_AvailableCaptureSamples( void ) { return 0; }
void SNDDMA_Capture( int samples, byte *data ) { (void)samples; (void)data; }
void SNDDMA_StopCapture( void ) { }
void SNDDMA_MasterGain( float val ) { (void)val; }

void WC_SND_Frame( void ) {
	const int16_t *src;
	int            frames, i;
	uint32_t       w;

	if ( !snd_inited || !dma.buffer ) {
		return;
	}

	pendingFrames += wc_time.delta_ms * ( WC_AUDIO_RATE / 1000.0 );
	frames = (int)pendingFrames;
	pendingFrames -= frames;
	if ( frames > WC_DMA_FRAMES / 2 ) {
		frames = WC_DMA_FRAMES / 2;
	}

	src = (const int16_t *)dma.buffer;
	w = wc_audio_write_cursor;
	for ( i = 0; i < frames; i++ ) {
		int s = dmapos + i * 2;
		if ( s >= dma.samples ) {
			s -= dma.samples;
		}
		wc_audio_ring[w * 2] = src[s];
		wc_audio_ring[w * 2 + 1] = src[s + 1];
		w = ( w + 1 ) % WC_AUDIO_CAP;
	}
	wc_audio_write_cursor = w;

	dmapos = ( dmapos + frames * 2 ) % dma.samples;
}
