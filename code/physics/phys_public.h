/*
===========================================================================
phys_public.h: what the rest of the engine sees of the physics module
(code/physics). No Box3D types here.
===========================================================================
*/
#ifndef PHYS_PUBLIC_H
#define PHYS_PUBLIC_H

void		Phys_Init( void );
// syscall handlers for the game and the cgame (block 1200-1249)
qboolean	Phys_GameCalls( intptr_t *args, intptr_t *ret );
qboolean	Phys_CgameCalls( intptr_t *args, intptr_t *ret );
// drop every world a VM made (it is restarting or going away)
void		Phys_FreeGameWorlds( void );
void		Phys_FreeCgameWorlds( void );

#endif
