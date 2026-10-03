/*
===========================================================================
oax engine
Copyright (C) 2026 Luis Montes

This file is part of the oax engine, a fork of ioquake3.
It is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version. The combined engine is distributed under GPLv3 (see
COPYING-GPLv3.txt).

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
more details.
===========================================================================
*/

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
