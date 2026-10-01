/*
===========================================================================
wasmcart platform backend: shared state between the backend's files.

The engine core talks to its platform through ioquake3's existing platform
interface (Sys_*, GLimp_*, SNDDMA_*, IN_*). This directory is the wasmcart
implementation of that interface; code/sys + code/sdl is the native one.
Nothing outside code/wasmcart includes this header.
===========================================================================
*/

#ifndef WC_LOCAL_H
#define WC_LOCAL_H

#include <stdint.h>
#include "wasmcart.h"

#define WC_DEFAULT_WIDTH   1280
#define WC_DEFAULT_HEIGHT  720
#define WC_MAX_WIDTH       3840
#define WC_MAX_HEIGHT      2160

#define WC_AUDIO_RATE      48000
#define WC_AUDIO_CAP       16384            // stereo frames in the host ring

#define WC_SAVE_SIZE       ( 4 * 1024 * 1024 )  // SRAM: configs, demos, screenshots

extern wc_info_t       wc_info;
extern wc_host_info_t  wc_host_info;
extern wc_pad_t        wc_pads[4];
extern wc_time_t       wc_time;
extern uint8_t         wc_keys[32];
extern wc_pointer_t    wc_pointers[10];
extern wc_wheel_t      wc_wheel;

extern int             wc_width;
extern int             wc_height;
extern int             wc_deterministic;   // WC_HOST_FLAG_DETERMINISTIC seen at init

// wc_vfs.c
void     WC_VFS_Init( void );
void     WC_VFS_Flush( void );              // write dirty files into the save region
uint8_t *WC_VFS_SaveRegion( void );

// wc_snd.c
void     WC_SND_Frame( void );              // hand mixed samples to the host ring

// wc_sys.c
uint32_t WC_Random( void );                 // the cart's only entropy source

#endif
