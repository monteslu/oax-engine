/*
===========================================================================
wasmcart platform backend: input.

Reads the host-written pad, keyboard and pointer state once per frame and
turns changes into engine events. The gamepad goes through the same
IN_GamepadFrame() the SDL backend uses, so identical stick and button state
produces an identical event stream on every build.
===========================================================================
*/

#include <string.h>

#include "../client/client.h"
#include "wc_local.h"

static cvar_t  *in_joystick;
static cvar_t  *in_joystickThreshold;
static cvar_t  *in_joystickUseAnalog;
static cvar_t  *in_mouse;

static uint8_t      oldKeys[32];
static wc_pointer_t oldPointer;

/*
===============
Keyboard: USB HID usage -> engine key
===============
*/
static int IN_KeyForHID( int hid ) {
	if ( hid >= WC_KEY_A && hid <= WC_KEY_Z ) return 'a' + ( hid - WC_KEY_A );
	if ( hid >= WC_KEY_1 && hid <= WC_KEY_9 ) return '1' + ( hid - WC_KEY_1 );
	if ( hid >= WC_KEY_F1 && hid <= WC_KEY_F12 ) return K_F1 + ( hid - WC_KEY_F1 );

	switch ( hid ) {
	case WC_KEY_0:          return '0';
	case WC_KEY_ENTER:      return K_ENTER;
	case WC_KEY_ESCAPE:     return K_ESCAPE;
	case WC_KEY_BACKSPACE:  return K_BACKSPACE;
	case WC_KEY_TAB:        return K_TAB;
	case WC_KEY_SPACE:      return K_SPACE;
	case WC_KEY_MINUS:      return '-';
	case WC_KEY_EQUAL:      return '=';
	case WC_KEY_LBRACKET:   return '[';
	case WC_KEY_RBRACKET:   return ']';
	case WC_KEY_BACKSLASH:  return '\\';
	case WC_KEY_SEMICOLON:  return ';';
	case WC_KEY_QUOTE:      return '\'';
	case WC_KEY_GRAVE:      return K_CONSOLE;
	case WC_KEY_COMMA:      return ',';
	case WC_KEY_PERIOD:     return '.';
	case WC_KEY_SLASH:      return '/';
	case WC_KEY_CAPSLOCK:   return K_CAPSLOCK;
	case WC_KEY_INSERT:     return K_INS;
	case WC_KEY_HOME:       return K_HOME;
	case WC_KEY_PAGEUP:     return K_PGUP;
	case WC_KEY_DELETE:     return K_DEL;
	case WC_KEY_END:        return K_END;
	case WC_KEY_PAGEDOWN:   return K_PGDN;
	case WC_KEY_RIGHT:      return K_RIGHTARROW;
	case WC_KEY_LEFT:       return K_LEFTARROW;
	case WC_KEY_DOWN:       return K_DOWNARROW;
	case WC_KEY_UP:         return K_UPARROW;
	case WC_KEY_KP_ENTER:   return K_KP_ENTER;
	case WC_KEY_KP_MINUS:   return K_KP_MINUS;
	case WC_KEY_KP_PLUS:    return K_KP_PLUS;
	case WC_KEY_LCTRL:
	case WC_KEY_RCTRL:      return K_CTRL;
	case WC_KEY_LSHIFT:
	case WC_KEY_RSHIFT:     return K_SHIFT;
	case WC_KEY_LALT:
	case WC_KEY_RALT:       return K_ALT;
	default:                return 0;
	}
}

// US layout characters for text entry (console, chat, names).
static int IN_CharForHID( int hid, qboolean shift ) {
	static const char *digits = "1234567890", *shiftDigits = "!@#$%^&*()";

	if ( hid >= WC_KEY_A && hid <= WC_KEY_Z ) {
		return ( shift ? 'A' : 'a' ) + ( hid - WC_KEY_A );
	}
	if ( hid >= WC_KEY_1 && hid <= WC_KEY_0 ) {
		return ( shift ? shiftDigits : digits )[hid - WC_KEY_1];
	}
	switch ( hid ) {
	case WC_KEY_SPACE:      return ' ';
	case WC_KEY_MINUS:      return shift ? '_' : '-';
	case WC_KEY_EQUAL:      return shift ? '+' : '=';
	case WC_KEY_LBRACKET:   return shift ? '{' : '[';
	case WC_KEY_RBRACKET:   return shift ? '}' : ']';
	case WC_KEY_BACKSLASH:  return shift ? '|' : '\\';
	case WC_KEY_SEMICOLON:  return shift ? ':' : ';';
	case WC_KEY_QUOTE:      return shift ? '"' : '\'';
	case WC_KEY_COMMA:      return shift ? '<' : ',';
	case WC_KEY_PERIOD:     return shift ? '>' : '.';
	case WC_KEY_SLASH:      return shift ? '?' : '/';
	case WC_KEY_BACKSPACE:  return '\b';
	default:                return 0;
	}
}

static void IN_KeyboardFrame( int time ) {
	qboolean shift = WC_KEY_IS_DOWN( wc_keys, WC_KEY_LSHIFT ) || WC_KEY_IS_DOWN( wc_keys, WC_KEY_RSHIFT );
	int      hid;

	for ( hid = 0; hid < 256; hid++ ) {
		qboolean down = WC_KEY_IS_DOWN( wc_keys, hid ) ? qtrue : qfalse;
		qboolean was = WC_KEY_IS_DOWN( oldKeys, hid ) ? qtrue : qfalse;
		int      key;

		if ( down == was ) {
			continue;
		}
		key = IN_KeyForHID( hid );
		if ( key ) {
			Com_QueueEvent( time, SE_KEY, key, down, 0, NULL );
		}
		if ( down && hid != WC_KEY_GRAVE ) {
			int ch = IN_CharForHID( hid, shift );
			if ( ch ) {
				Com_QueueEvent( time, SE_CHAR, ch, 0, 0, NULL );
			}
		}
	}
	memcpy( oldKeys, wc_keys, sizeof( oldKeys ) );
}

/*
===============
Pointer: relative motion, buttons, wheel
===============
*/
static void IN_PointerFrame( int time ) {
	const wc_pointer_t *p = &wc_pointers[0];
	static const int    buttonKeys[3] = { K_MOUSE1, K_MOUSE2, K_MOUSE3 };
	int                 i;

	if ( p->active && oldPointer.active && in_mouse->integer ) {
		int dx = p->x - oldPointer.x;
		int dy = p->y - oldPointer.y;
		if ( dx || dy ) {
			Com_QueueEvent( time, SE_MOUSE, dx, dy, 0, NULL );
		}
	}

	for ( i = 0; i < 3; i++ ) {
		int down = p->active && ( p->buttons & ( 1 << i ) );
		int was = oldPointer.active && ( oldPointer.buttons & ( 1 << i ) );
		if ( down != was ) {
			Com_QueueEvent( time, SE_KEY, buttonKeys[i], down ? qtrue : qfalse, 0, NULL );
		}
	}

	if ( wc_wheel.dy ) {
		int key = wc_wheel.dy > 0 ? K_MWHEELUP : K_MWHEELDOWN;
		Com_QueueEvent( time, SE_KEY, key, qtrue, 0, NULL );
		Com_QueueEvent( time, SE_KEY, key, qfalse, 0, NULL );
	}

	oldPointer = *p;
}

/*
===============
Gamepad
===============
*/
static void IN_PadFrame( int time ) {
	const wc_pad_t *pad = &wc_pads[0];
	in_gamepad_t    gp;

	memset( &gp, 0, sizeof( gp ) );

	if ( pad->connected ) {
		gp.buttons[0] = ( pad->buttons & WC_BTN_A ) != 0;
		gp.buttons[1] = ( pad->buttons & WC_BTN_B ) != 0;
		gp.buttons[2] = ( pad->buttons & WC_BTN_X ) != 0;
		gp.buttons[3] = ( pad->buttons & WC_BTN_Y ) != 0;
		gp.buttons[4] = ( pad->buttons & WC_BTN_SELECT ) != 0;
		gp.buttons[6] = ( pad->buttons & WC_BTN_START ) != 0;
		gp.buttons[7] = ( pad->buttons & WC_BTN_L3 ) != 0;
		gp.buttons[8] = ( pad->buttons & WC_BTN_R3 ) != 0;
		gp.buttons[9] = ( pad->buttons & WC_BTN_L ) != 0;
		gp.buttons[10] = ( pad->buttons & WC_BTN_R ) != 0;
		gp.buttons[11] = ( pad->buttons & WC_BTN_UP ) != 0;
		gp.buttons[12] = ( pad->buttons & WC_BTN_DOWN ) != 0;
		gp.buttons[13] = ( pad->buttons & WC_BTN_LEFT ) != 0;
		gp.buttons[14] = ( pad->buttons & WC_BTN_RIGHT ) != 0;

		gp.axes[0] = pad->left_x;
		gp.axes[1] = pad->left_y;
		gp.axes[2] = pad->right_x;
		gp.axes[3] = pad->right_y;
		gp.axes[4] = pad->left_trigger * 32767 / 255;
		gp.axes[5] = pad->right_trigger * 32767 / 255;
	}

	IN_GamepadFrame( &gp, time, in_joystickThreshold->value, in_joystickUseAnalog->integer ? qtrue : qfalse );
}

/*
===============
Engine interface
===============
*/
void IN_Init( void *windowData ) {
	(void)windowData;

	in_joystick = Cvar_Get( "in_joystick", "1", CVAR_ARCHIVE | CVAR_LATCH );
	in_joystickThreshold = Cvar_Get( "joy_threshold", "0.15", CVAR_ARCHIVE );
	in_joystickUseAnalog = Cvar_Get( "in_joystickUseAnalog", "1", CVAR_ARCHIVE );
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );

	memset( oldKeys, 0, sizeof( oldKeys ) );
	memset( &oldPointer, 0, sizeof( oldPointer ) );
	IN_GamepadReset();
}

// Called by Com_Frame just before it drains the event queue, at the same
// point in the frame as the SDL backend's IN_Frame.
void IN_Frame( void ) {
	int time;

	if ( !in_joystick ) {
		return;
	}
	time = Sys_Milliseconds();

	IN_KeyboardFrame( time );
	IN_PointerFrame( time );

	// a pad script (cl_testscript.c) supplies the gamepad while it plays
	if ( !CL_PadScriptFrame( time ) && in_joystick->integer ) {
		IN_PadFrame( time );
	}
}

void IN_Shutdown( void ) {
	in_joystick = NULL;
}

void IN_Restart( void ) {
	IN_Shutdown();
	IN_Init( NULL );
}
