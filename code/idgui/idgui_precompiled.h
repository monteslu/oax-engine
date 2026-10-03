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
idgui_precompiled.h: every header of the GUI module, in dependency order.

Plays the part of DOOM-3's neo/idlib/precompiled.h for code/idgui/. System
headers come first; then everything (the idlib-lite classes and the ports
of neo/ui) is declared inside namespace idgui, so this module links next to
the script VM's own idlib_lite without a clash. Each .cpp includes this one
header and wraps its body in namespace idgui.
===========================================================================
*/
#ifndef __IDGUI_PRECOMPILED_H__
#define __IDGUI_PRECOMPILED_H__

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <time.h>
#include <new>

#include "idgui_public.h"

// engine C functions the module calls directly (qcommon/q_detmath.c)
extern "C" float Q_detSinf( float x );
extern "C" float Q_detCosf( float x );

#ifndef NDEBUG
#define NDEBUG
#endif
#include <assert.h>

namespace idgui {

#include "idlib/Lib.h"
#include "idlib/Math.h"
#include "idlib/List.h"
#include "idlib/Str.h"
#include "idlib/HashIndex.h"
#include "idlib/HashTable.h"
#include "idlib/Token.h"
#include "idlib/Lexer.h"
#include "idlib/Parser.h"
#include "idlib/Extrapolate.h"
#include "idlib/Interpolate.h"
#include "idlib/Dict.h"
#include "idgui_sys.h"

#include "Rectangle.h"
#include "DeviceContext.h"
#include "RegExp.h"
#include "Winvar.h"
#include "GuiScript.h"
#include "SimpleWindow.h"
#include "Window.h"
#include "UserInterface.h"
#include "UserInterfaceLocal.h"
#include "ChoiceWindow.h"
#include "SliderWindow.h"
#include "ListWindow.h"
#include "EditWindow.h"

// the module's engine imports (idgui_main.cpp)
extern idguiImport_t		gImport;

// the error path: Error() longjmps to the innermost guard
struct idLibGuard {
	jmp_buf				jb;
	idLibGuard *		prev;
};
void		idLib_PushGuard( idLibGuard *g );
void		idLib_PopGuard( idLibGuard *g );

} // namespace idgui

#endif
