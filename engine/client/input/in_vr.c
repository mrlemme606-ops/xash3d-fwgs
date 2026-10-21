/*
in_vr.c - VR controller input (milestone 2a)
Cry of Fear VR project.
Copyright (C) 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

The renderer DLL (ref/gl/gl_vr_input.c) polls the SteamVR controllers and publishes the
result into the vr_in_* cvars. This file turns them into movement, turning and key presses.
*/
#include "common.h"
#include "client.h"
#include "input.h"
#include "keydefs.h"

// published by the renderer each frame, not meant to be set by hand
static CVAR_DEFINE_AUTO( vr_in_active, "0", 0, "(internal) VR controllers are active" );
static CVAR_DEFINE_AUTO( vr_in_move_fwd, "0", 0, "(internal) forward movement, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_move_side, "0", 0, "(internal) strafe movement, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_turn, "0", 0, "(internal) turn stick, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_buttons, "0", 0, "(internal) button bitmask" );

// settings
static CVAR_DEFINE_AUTO( vr_turn_mode, "0", FCVAR_ARCHIVE, "0 = snap turn, 1 = smooth turn" );
static CVAR_DEFINE_AUTO( vr_snap_angle, "30", FCVAR_ARCHIVE, "snap turn angle in degrees" );
static CVAR_DEFINE_AUTO( vr_smooth_speed, "120", FCVAR_ARCHIVE, "smooth turn speed in degrees per second" );

// which key each controller button presses. single character, or mouse1/2/3, space, ctrl, shift, enter, tab, escape
static CVAR_DEFINE_AUTO( vr_key_rtrigger, "mouse1", FCVAR_ARCHIVE, "key for right trigger" );
static CVAR_DEFINE_AUTO( vr_key_ltrigger, "mouse2", FCVAR_ARCHIVE, "key for left trigger" );
static CVAR_DEFINE_AUTO( vr_key_rgrip, "e", FCVAR_ARCHIVE, "key for right grip" );
static CVAR_DEFINE_AUTO( vr_key_lgrip, "f", FCVAR_ARCHIVE, "key for left grip" );
static CVAR_DEFINE_AUTO( vr_key_a, "space", FCVAR_ARCHIVE, "key for A (right controller)" );
static CVAR_DEFINE_AUTO( vr_key_b, "r", FCVAR_ARCHIVE, "key for B (right controller)" );
static CVAR_DEFINE_AUTO( vr_key_x, "tab", FCVAR_ARCHIVE, "key for X (left controller)" );
static CVAR_DEFINE_AUTO( vr_key_y, "i", FCVAR_ARCHIVE, "key for Y (left controller)" );
static CVAR_DEFINE_AUTO( vr_key_lstick, "shift", FCVAR_ARCHIVE, "key for left stick click" );
static CVAR_DEFINE_AUTO( vr_key_rstick, "ctrl", FCVAR_ARCHIVE, "key for right stick click" );

#define VR_BUTTON_COUNT	10

static convar_t *vr_key_cvars[VR_BUTTON_COUNT] =
{
	&vr_key_rtrigger, &vr_key_ltrigger, &vr_key_rgrip, &vr_key_lgrip, &vr_key_a,
	&vr_key_b, &vr_key_x, &vr_key_y, &vr_key_lstick, &vr_key_rstick
};

static int vr_pressed_key[VR_BUTTON_COUNT];	// key code sent on press, 0 = not held
static int vr_prev_buttons;
static qboolean vr_snap_latched;

static int IN_VRKeyFromName( const char *name )
{
	if( COM_StringEmptyOrNULL( name ))
		return 0;

	if( !name[1] )
	{
		int c = (unsigned char)name[0];

		if( c >= 'A' && c <= 'Z' )
			c += 'a' - 'A';

		return c;
	}

	if( !Q_stricmp( name, "mouse1" )) return K_MOUSE1;
	if( !Q_stricmp( name, "mouse2" )) return K_MOUSE2;
	if( !Q_stricmp( name, "mouse3" )) return K_MOUSE3;
	if( !Q_stricmp( name, "space" )) return K_SPACE;
	if( !Q_stricmp( name, "ctrl" )) return K_CTRL;
	if( !Q_stricmp( name, "shift" )) return K_SHIFT;
	if( !Q_stricmp( name, "enter" )) return K_ENTER;
	if( !Q_stricmp( name, "tab" )) return K_TAB;
	if( !Q_stricmp( name, "escape" )) return K_ESCAPE;

	return 0;
}

void IN_VRInit( void )
{
	int i;

	Cvar_RegisterVariable( &vr_in_active );
	Cvar_RegisterVariable( &vr_in_move_fwd );
	Cvar_RegisterVariable( &vr_in_move_side );
	Cvar_RegisterVariable( &vr_in_turn );
	Cvar_RegisterVariable( &vr_in_buttons );
	Cvar_RegisterVariable( &vr_turn_mode );
	Cvar_RegisterVariable( &vr_snap_angle );
	Cvar_RegisterVariable( &vr_smooth_speed );

	for( i = 0; i < VR_BUTTON_COUNT; i++ )
		Cvar_RegisterVariable( vr_key_cvars[i] );
}

static void IN_VRReleaseAll( void )
{
	int i;

	for( i = 0; i < VR_BUTTON_COUNT; i++ )
	{
		if( vr_pressed_key[i] )
		{
			Key_Event( vr_pressed_key[i], false );
			vr_pressed_key[i] = 0;
		}
	}

	vr_prev_buttons = 0;
}

static void IN_VRUpdateButtons( int buttons )
{
	int changed = buttons ^ vr_prev_buttons;
	int i;

	if( !changed )
		return;

	for( i = 0; i < VR_BUTTON_COUNT; i++ )
	{
		if( !FBitSet( changed, BIT( i )))
			continue;

		if( FBitSet( buttons, BIT( i )))
		{
			int key = IN_VRKeyFromName( vr_key_cvars[i]->string );

			if( key )
			{
				vr_pressed_key[i] = key;
				Key_Event( key, true );
			}
		}
		else if( vr_pressed_key[i] )
		{
			// release exactly the key we pressed, even if the cvar changed meanwhile
			Key_Event( vr_pressed_key[i], false );
			vr_pressed_key[i] = 0;
		}
	}

	vr_prev_buttons = buttons;
}

void IN_VRAppendMove( float frametime, usercmd_t *cmd, qboolean active )
{
	float yaw = 0.0f;
	float x;

	if( vr_in_active.value <= 0.0f )
	{
		IN_VRReleaseAll();
		return;
	}

	IN_VRUpdateButtons( (int)vr_in_buttons.value );

	if( !active || cls.key_dest != key_game || cl.paused || cl.intermission )
		return;

	// movement. the renderer already rotated the stick into the body frame
	cmd->forwardmove += vr_in_move_fwd.value * Cvar_VariableValue( "cl_forwardspeed" );
	cmd->sidemove += vr_in_move_side.value * Cvar_VariableValue( "cl_sidespeed" );

	// turning
	x = vr_in_turn.value;

	if( vr_turn_mode.value )
	{
		if( fabs( x ) > 0.05f )
			yaw = -x * vr_smooth_speed.value * frametime;
	}
	else if( !vr_snap_latched && fabs( x ) > 0.7f )
	{
		yaw = ( x > 0.0f ? -1.0f : 1.0f ) * vr_snap_angle.value;
		vr_snap_latched = true;
	}
	else if( fabs( x ) < 0.3f )
	{
		vr_snap_latched = false;
	}

	if( yaw != 0.0f )
	{
		cmd->viewangles[YAW] += yaw;
		VectorCopy( cmd->viewangles, cl.viewangles );
	}
}
