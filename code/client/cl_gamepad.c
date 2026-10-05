/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// cl_gamepad.c -- turns a polled gamepad state into key and axis events
//
// Every platform backend reads its controller its own way and hands the
// result here, so the same stick and button state produces the same event
// stream on every build. Moved out of sdl/sdl_input.c unchanged in behavior.

#include "client.h"

typedef struct {
	qboolean buttons[IN_GAMEPAD_BUTTONS];
	keyNum_t sentKeys[IN_GAMEPAD_BUTTONS];	// the key a press produced, so its release matches
	int      axes[IN_GAMEPAD_AXES];
} in_gamepadEdge_t;

static in_gamepadEdge_t gamepadOld;
static keyNum_t stickSentPos[IN_GAMEPAD_AXES], stickSentNeg[IN_GAMEPAD_AXES];	// the key a stick press produced

static const keyNum_t gamepadButtonKeys[IN_GAMEPAD_BUTTONS] = {
	K_PAD0_A, K_PAD0_B, K_PAD0_X, K_PAD0_Y,
	K_PAD0_BACK, K_PAD0_GUIDE, K_PAD0_START,
	K_PAD0_LEFTSTICK_CLICK, K_PAD0_RIGHTSTICK_CLICK,
	K_PAD0_LEFTSHOULDER, K_PAD0_RIGHTSHOULDER,
	K_PAD0_DPAD_UP, K_PAD0_DPAD_DOWN, K_PAD0_DPAD_LEFT, K_PAD0_DPAD_RIGHT,
	K_PAD0_MISC1, K_PAD0_PADDLE1, K_PAD0_PADDLE2, K_PAD0_PADDLE3, K_PAD0_PADDLE4,
	K_PAD0_TOUCHPAD
};

/*
===============
GamepadKeyFor

Menus and the console only understand keyboard navigation, so while one of
them has the keys a pad press is delivered as the key it stands for. Start
is Escape everywhere, which is what opens the in-game menu.
===============
*/
static keyNum_t GamepadKeyFor( int button )
{
	keyNum_t key = gamepadButtonKeys[button];

	if (key == K_PAD0_START)
		return K_ESCAPE;

	if (Key_GetCatcher() & (KEYCATCH_UI | KEYCATCH_CONSOLE | KEYCATCH_MESSAGE))
	{
		switch (key)
		{
			case K_PAD0_A:          return K_ENTER;
			case K_PAD0_B:          return K_ESCAPE;
			case K_PAD0_DPAD_UP:    return K_UPARROW;
			case K_PAD0_DPAD_DOWN:  return K_DOWNARROW;
			case K_PAD0_DPAD_LEFT:  return K_LEFTARROW;
			case K_PAD0_DPAD_RIGHT: return K_RIGHTARROW;
			default:                break;
		}
	}

	return key;
}

/*
===============
StickKeyFor

The left stick steers menus like the d-pad.
===============
*/
static keyNum_t StickKeyFor( int key )
{
	if (Key_GetCatcher() & (KEYCATCH_UI | KEYCATCH_CONSOLE | KEYCATCH_MESSAGE))
	{
		switch (key)
		{
			case K_PAD0_LEFTSTICK_UP:    return K_UPARROW;
			case K_PAD0_LEFTSTICK_DOWN:  return K_DOWNARROW;
			case K_PAD0_LEFTSTICK_LEFT:  return K_LEFTARROW;
			case K_PAD0_LEFTSTICK_RIGHT: return K_RIGHTARROW;
			default:                     break;
		}
	}
	return key;
}

/*
===============
CL_GamepadDefaults

After the config files: modern twin-stick controls (Xbox 360 layout) on
every pad button the player has not bound, and, once per config, modern
stick tuning. in_gamepadBinds 0 leaves the pad unbound. On the keyboard,
E is use and V toggles the view when nothing else is bound to them.

  left stick   move                  right stick  look
  A            jump                  B            crouch
  X            use (vehicles, items) Y            next weapon
  LB / RB      previous / next weapon
  LT           zoom                  RT           fire
  left click   walk                  right click  first / third person
  d-pad        up / right next weapon, down / left previous
  Back         scores                Start        menu

In a vehicle the game reads the same buttons its own way (Halo and
Battlefield's conventions): A switches seats, B is the handbrake, X held
gets out (a tap gets in), RT fires the mounted gun. Q3 weapons have no
alternate fire. Version 2 put the view toggle on Y; version 3 moves it to
the right stick click, as Battlefield has it, and gives Y back to next
weapon.
===============
*/
#define GAMEPAD_DEFAULTS_VERSION 3

void CL_GamepadDefaults( void )
{
	static const struct { int key; const char *bind; } binds[] = {
		{ K_PAD0_LEFTSTICK_UP, "+forward" }, { K_PAD0_LEFTSTICK_DOWN, "+back" },
		{ K_PAD0_LEFTSTICK_LEFT, "+moveleft" }, { K_PAD0_LEFTSTICK_RIGHT, "+moveright" },
		{ K_PAD0_RIGHTSTICK_UP, "+lookup" }, { K_PAD0_RIGHTSTICK_DOWN, "+lookdown" },
		{ K_PAD0_RIGHTSTICK_LEFT, "+left" }, { K_PAD0_RIGHTSTICK_RIGHT, "+right" },
		{ K_PAD0_A, "+moveup" }, { K_PAD0_B, "+movedown" },
		{ K_PAD0_X, "+button2" }, { K_PAD0_Y, "weapnext" },
		{ K_PAD0_LEFTSHOULDER, "weapprev" }, { K_PAD0_RIGHTSHOULDER, "weapnext" },
		{ K_PAD0_LEFTTRIGGER, "+zoom" }, { K_PAD0_RIGHTTRIGGER, "+attack" },
		{ K_PAD0_LEFTSTICK_CLICK, "+speed" }, { K_PAD0_RIGHTSTICK_CLICK, "toggleview" },
		{ K_PAD0_DPAD_UP, "weapnext" }, { K_PAD0_DPAD_DOWN, "weapprev" },
		{ K_PAD0_DPAD_LEFT, "weapprev" }, { K_PAD0_DPAD_RIGHT, "weapnext" },
		{ K_PAD0_BACK, "+scores" },
	};
	cvar_t *on = Cvar_Get( "in_gamepadBinds", "1", CVAR_ARCHIVE );
	cvar_t *version = Cvar_Get( "in_gamepadVersion", "0", CVAR_ARCHIVE );
	int i;

	Cvar_SetDescription( on, "Bind modern twin-stick gamepad controls (Xbox 360 layout) to unbound pad buttons at startup." );
	if ( !on->integer )
		return;
	for ( i = 0; i < ARRAY_LEN( binds ); i++ ) {
		const char *b = Key_GetBinding( binds[i].key );

		if ( !b || !b[0] )
			Key_SetBinding( binds[i].key, binds[i].bind );
	}
	{
		const char *b = Key_GetBinding( 'v' );

		if ( !b || !b[0] )
			Key_SetBinding( 'v', "toggleview" );
		b = Key_GetBinding( 'e' );
		if ( !b || !b[0] )
			Key_SetBinding( 'e', "+button2" );
	}
	// earlier defaults: the view toggle on Y (version 2), centre view on
	// the right stick click (before version 3)
	if ( version->integer < 3 ) {
		const char *b = Key_GetBinding( K_PAD0_Y );

		if ( b && !Q_stricmp( b, "toggleview" ) )
			Key_SetBinding( K_PAD0_Y, "weapnext" );
		b = Key_GetBinding( K_PAD0_RIGHTSTICK_CLICK );
		if ( b && !Q_stricmp( b, "centerview" ) )
			Key_SetBinding( K_PAD0_RIGHTSTICK_CLICK, "toggleview" );
	}
	// configs written before these defaults hold the old stick values
	// (digital-feeling movement, 720 degrees a second of look)
	if ( version->integer < 1 ) {
		Cvar_Set( "in_joystick", "1" );
		Cvar_Set( "in_joystickUseAnalog", "1" );
		Cvar_Set( "j_forward", "-0.0045" );
		Cvar_Set( "j_side", "0.0045" );
		Cvar_Set( "j_yaw", "-0.008" );
		Cvar_Set( "j_pitch", "0.0055" );
		Cvar_Set( "j_lookCurve", "2" );
	}
	if ( version->integer < GAMEPAD_DEFAULTS_VERSION )
		Cvar_SetValue( "in_gamepadVersion", GAMEPAD_DEFAULTS_VERSION );
}

/*
===============
IN_GamepadReset
===============
*/
void IN_GamepadReset( void )
{
	Com_Memset( &gamepadOld, 0, sizeof( gamepadOld ) );
	Com_Memset( stickSentPos, 0, sizeof( stickSentPos ) );
	Com_Memset( stickSentNeg, 0, sizeof( stickSentNeg ) );
}

/*
===============
KeyToAxisAndSign
===============
*/
static qboolean KeyToAxisAndSign(int keynum, int *outAxis, int *outSign)
{
	char *bind;

	if (!keynum)
		return qfalse;

	bind = Key_GetBinding(keynum);

	if (!bind || *bind != '+')
		return qfalse;

	*outSign = 0;

	if (Q_stricmp(bind, "+forward") == 0)
	{
		*outAxis = j_forward_axis->integer;
		*outSign = j_forward->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+back") == 0)
	{
		*outAxis = j_forward_axis->integer;
		*outSign = j_forward->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+moveleft") == 0)
	{
		*outAxis = j_side_axis->integer;
		*outSign = j_side->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+moveright") == 0)
	{
		*outAxis = j_side_axis->integer;
		*outSign = j_side->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+lookup") == 0)
	{
		*outAxis = j_pitch_axis->integer;
		*outSign = j_pitch->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+lookdown") == 0)
	{
		*outAxis = j_pitch_axis->integer;
		*outSign = j_pitch->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+left") == 0)
	{
		*outAxis = j_yaw_axis->integer;
		*outSign = j_yaw->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+right") == 0)
	{
		*outAxis = j_yaw_axis->integer;
		*outSign = j_yaw->value > 0.0f ? -1 : 1;
	}
	else if (Q_stricmp(bind, "+moveup") == 0)
	{
		*outAxis = j_up_axis->integer;
		*outSign = j_up->value > 0.0f ? 1 : -1;
	}
	else if (Q_stricmp(bind, "+movedown") == 0)
	{
		*outAxis = j_up_axis->integer;
		*outSign = j_up->value > 0.0f ? -1 : 1;
	}

	return *outSign != 0;
}

/*
===============
IN_GamepadFrame

pad->axes are raw values: sticks -32768..32767, triggers 0..32767.
===============
*/
void IN_GamepadFrame( const in_gamepad_t *pad, int eventTime, float threshold, qboolean useAnalog )
{
	int i;
	int translatedAxes[MAX_JOYSTICK_AXIS];
	qboolean translatedAxesSet[MAX_JOYSTICK_AXIS];

	// check buttons
	for (i = 0; i < IN_GAMEPAD_BUTTONS; i++)
	{
		qboolean pressed = pad->buttons[i] ? qtrue : qfalse;
		if (pressed != gamepadOld.buttons[i])
		{
			if (pressed)
				gamepadOld.sentKeys[i] = GamepadKeyFor(i);
			Com_QueueEvent(eventTime, SE_KEY, gamepadOld.sentKeys[i], pressed, 0, NULL);
			gamepadOld.buttons[i] = pressed;
		}
	}

	// must defer translated axes until all real axes are processed
	// must be done this way to prevent a later mapped axis from zeroing out a previous one
	if (useAnalog)
	{
		for (i = 0; i < MAX_JOYSTICK_AXIS; i++)
		{
			translatedAxes[i] = 0;
			translatedAxesSet[i] = qfalse;
		}
	}

	// check axes
	for (i = 0; i < IN_GAMEPAD_AXES; i++)
	{
		int axis = pad->axes[i];
		int oldAxis = gamepadOld.axes[i];

		// Smoothly ramp from dead zone to maximum value
		float f = ((float)abs(axis) / 32767.0f - threshold) / (1.0f - threshold);

		if (f < 0.0f)
			f = 0.0f;

		axis = (int)(32767 * ((axis < 0) ? -f : f));

		if (axis != oldAxis)
		{
			const int negMap[IN_GAMEPAD_AXES] = { K_PAD0_LEFTSTICK_LEFT,  K_PAD0_LEFTSTICK_UP,   K_PAD0_RIGHTSTICK_LEFT,  K_PAD0_RIGHTSTICK_UP, 0, 0 };
			const int posMap[IN_GAMEPAD_AXES] = { K_PAD0_LEFTSTICK_RIGHT, K_PAD0_LEFTSTICK_DOWN, K_PAD0_RIGHTSTICK_RIGHT, K_PAD0_RIGHTSTICK_DOWN, K_PAD0_LEFTTRIGGER, K_PAD0_RIGHTTRIGGER };

			qboolean posAnalog = qfalse, negAnalog = qfalse;
			int negKey = negMap[i];
			int posKey = posMap[i];

			if (useAnalog)
			{
				int posAxis = 0, posSign = 0, negAxis = 0, negSign = 0;

				// get axes and axes signs for keys if available
				posAnalog = KeyToAxisAndSign(posKey, &posAxis, &posSign);
				negAnalog = KeyToAxisAndSign(negKey, &negAxis, &negSign);

				// positive to negative/neutral -> keyup if axis hasn't yet been set
				if (posAnalog && !translatedAxesSet[posAxis] && oldAxis > 0 && axis <= 0)
				{
					translatedAxes[posAxis] = 0;
					translatedAxesSet[posAxis] = qtrue;
				}

				// negative to positive/neutral -> keyup if axis hasn't yet been set
				if (negAnalog && !translatedAxesSet[negAxis] && oldAxis < 0 && axis >= 0)
				{
					translatedAxes[negAxis] = 0;
					translatedAxesSet[negAxis] = qtrue;
				}

				// negative/neutral to positive -> keydown
				if (posAnalog && axis > 0)
				{
					translatedAxes[posAxis] = axis * posSign;
					translatedAxesSet[posAxis] = qtrue;
				}

				// positive/neutral to negative -> keydown
				if (negAnalog && axis < 0)
				{
					translatedAxes[negAxis] = -axis * negSign;
					translatedAxesSet[negAxis] = qtrue;
				}
			}

			// keyups first so they get overridden by keydowns later

			// positive to negative/neutral -> keyup
			if (stickSentPos[i] && oldAxis > 0 && axis <= 0)
			{
				Com_QueueEvent(eventTime, SE_KEY, stickSentPos[i], qfalse, 0, NULL);
				stickSentPos[i] = 0;
			}

			// negative to positive/neutral -> keyup
			if (stickSentNeg[i] && oldAxis < 0 && axis >= 0)
			{
				Com_QueueEvent(eventTime, SE_KEY, stickSentNeg[i], qfalse, 0, NULL);
				stickSentNeg[i] = 0;
			}

			// negative/neutral to positive -> keydown (in menus the left
			// stick is the arrow keys, even when it moves analog in game)
			if (posKey && oldAxis <= 0 && axis > 0)
			{
				keyNum_t k = StickKeyFor(posKey);
				if (!posAnalog || k != posKey)
				{
					stickSentPos[i] = k;
					Com_QueueEvent(eventTime, SE_KEY, k, qtrue, 0, NULL);
				}
			}

			// positive/neutral to negative -> keydown
			if (negKey && oldAxis >= 0 && axis < 0)
			{
				keyNum_t k = StickKeyFor(negKey);
				if (!negAnalog || k != negKey)
				{
					stickSentNeg[i] = k;
					Com_QueueEvent(eventTime, SE_KEY, k, qtrue, 0, NULL);
				}
			}

			gamepadOld.axes[i] = axis;
		}
	}

	// set translated axes
	if (useAnalog)
	{
		for (i = 0; i < MAX_JOYSTICK_AXIS; i++)
		{
			if (translatedAxesSet[i])
				Com_QueueEvent(eventTime, SE_JOYSTICK_AXIS, i, translatedAxes[i], 0, NULL);
		}
	}
}
