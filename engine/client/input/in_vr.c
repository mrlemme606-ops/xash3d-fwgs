/*
in_vr.c - VR controller input
Cry of Fear VR project, milestone 2.
Copyright (C) 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

The renderer DLL (ref/gl/gl_vr_input.c) polls the SteamVR controllers and publishes the
result into the vr_in_* cvars. This file turns them into movement, turning, aiming and key presses.
*/
#include "common.h"
#include "client.h"
#include "input.h"
#include "keydefs.h"

// published by the renderer each frame, not meant to be set by hand
static CVAR_DEFINE_AUTO( vr_in_active, "0", 0, "(internal) VR controllers are active" );
static CVAR_DEFINE_AUTO( vr_in_move_fwd, "0", 0, "(internal) left stick forward, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_move_side, "0", 0, "(internal) left stick sideways, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_turn, "0", 0, "(internal) right stick sideways, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_stick_y, "0", 0, "(internal) right stick up/down, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_head_yaw, "0", 0, "(internal) head yaw relative to the body, degrees" );
static CVAR_DEFINE_AUTO( vr_in_head_pitch, "0", 0, "(internal) head pitch, degrees (positive = down)" );
static CVAR_DEFINE_AUTO( vr_in_crouch, "0", 0, "(internal) physically crouching" );
static CVAR_DEFINE_AUTO( vr_in_buttons, "0", 0, "(internal) button bitmask" );

// settings
static CVAR_DEFINE_AUTO( vr_turn_mode, "0", FCVAR_ARCHIVE, "0 = snap turn, 1 = smooth turn" );
static CVAR_DEFINE_AUTO( vr_snap_angle, "30", FCVAR_ARCHIVE, "snap turn angle in degrees" );
static CVAR_DEFINE_AUTO( vr_smooth_speed, "120", FCVAR_ARCHIVE, "smooth turn speed in degrees per second" );
static CVAR_DEFINE_AUTO( vr_aim_gaze, "1", FCVAR_ARCHIVE, "1 = the game's aim, use and walking direction follow your head. Temporary until hand aiming exists" );

// which key each controller button presses. single character, or mouse1/2/3, space, ctrl, shift, enter, tab, escape. empty = nothing
static CVAR_DEFINE_AUTO( vr_key_rtrigger, "mouse1", FCVAR_ARCHIVE, "key for right trigger" );
static CVAR_DEFINE_AUTO( vr_key_ltrigger, "mouse2", FCVAR_ARCHIVE, "key for left trigger" );
static CVAR_DEFINE_AUTO( vr_key_rgrip, "e", FCVAR_ARCHIVE, "key for right grip" );
static CVAR_DEFINE_AUTO( vr_key_lgrip, "f", FCVAR_ARCHIVE, "key for left grip" );
static CVAR_DEFINE_AUTO( vr_key_a, "space", FCVAR_ARCHIVE, "key for A (right controller)" );
static CVAR_DEFINE_AUTO( vr_key_b, "r", FCVAR_ARCHIVE, "key for B (right controller)" );
static CVAR_DEFINE_AUTO( vr_key_x, "tab", FCVAR_ARCHIVE, "key for X (left controller)" );
static CVAR_DEFINE_AUTO( vr_key_y, "i", FCVAR_ARCHIVE, "key for Y (left controller)" );
static CVAR_DEFINE_AUTO( vr_key_lstick, "shift", FCVAR_ARCHIVE, "key for left stick click (sprint)" );
static CVAR_DEFINE_AUTO( vr_key_rstick, "", FCVAR_ARCHIVE, "key for right stick click (left free, reserved for magazine eject)" );
static CVAR_DEFINE_AUTO( vr_key_jump, "space", FCVAR_ARCHIVE, "key for jump (right stick up)" );
static CVAR_DEFINE_AUTO( vr_key_crouch, "ctrl", FCVAR_ARCHIVE, "key for crouch (right stick down toggles, or physically crouch)" );

enum
{
	VRB_RTRIGGER = 0, VRB_LTRIGGER, VRB_RGRIP, VRB_LGRIP, VRB_A, VRB_B, VRB_X, VRB_Y, VRB_LSTICK, VRB_RSTICK,
	VRB_JUMP,	// made here from the right stick flick, not published by the renderer
	VRB_CROUCH,	// likewise
	VRB_COUNT
};

static convar_t *vr_key_cvars[VRB_COUNT] =
{
	&vr_key_rtrigger, &vr_key_ltrigger, &vr_key_rgrip, &vr_key_lgrip, &vr_key_a,
	&vr_key_b, &vr_key_x, &vr_key_y, &vr_key_lstick, &vr_key_rstick,
	&vr_key_jump, &vr_key_crouch
};

static int vr_pressed_key[VRB_COUNT];	// key code sent on press, 0 = not held
static int vr_prev_buttons;
static qboolean vr_snap_latched;
static qboolean vr_up_latched, vr_down_latched;
static qboolean vr_crouch_toggled;
static int vr_jump_frames;

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
	Cvar_RegisterVariable( &vr_in_stick_y );
	Cvar_RegisterVariable( &vr_in_head_yaw );
	Cvar_RegisterVariable( &vr_in_head_pitch );
	Cvar_RegisterVariable( &vr_in_crouch );
	Cvar_RegisterVariable( &vr_in_buttons );
	Cvar_RegisterVariable( &vr_turn_mode );
	Cvar_RegisterVariable( &vr_snap_angle );
	Cvar_RegisterVariable( &vr_smooth_speed );
	Cvar_RegisterVariable( &vr_aim_gaze );

	for( i = 0; i < VRB_COUNT; i++ )
		Cvar_RegisterVariable( vr_key_cvars[i] );
}

static void IN_VRReleaseAll( void )
{
	int i;

	for( i = 0; i < VRB_COUNT; i++ )
	{
		if( vr_pressed_key[i] )
		{
			Key_Event( vr_pressed_key[i], false );
			vr_pressed_key[i] = 0;
		}
	}

	vr_prev_buttons = 0;
	vr_crouch_toggled = false;
	vr_jump_frames = 0;
}

static void IN_VRUpdateButtons( int buttons )
{
	int changed = buttons ^ vr_prev_buttons;
	int i;

	if( !changed )
		return;

	for( i = 0; i < VRB_COUNT; i++ )
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
	float x, y, fwd, side;
	int buttons;

	if( vr_in_active.value <= 0.0f )
	{
		IN_VRReleaseAll();
		return;
	}

	buttons = (int)vr_in_buttons.value;

	// right stick flicks: up = jump (a short key press), down = toggle crouch
	y = vr_in_stick_y.value;

	if( y > 0.7f && !vr_up_latched )
	{
		vr_up_latched = true;
		vr_jump_frames = 3;
	}
	else if( y < 0.3f )
	{
		vr_up_latched = false;
	}

	if( y < -0.7f && !vr_down_latched )
	{
		vr_down_latched = true;
		vr_crouch_toggled = !vr_crouch_toggled;
	}
	else if( y > -0.3f )
	{
		vr_down_latched = false;
	}

	if( vr_jump_frames > 0 )
	{
		buttons |= BIT( VRB_JUMP );
		vr_jump_frames--;
	}

	if( vr_crouch_toggled || vr_in_crouch.value > 0.0f )
		buttons |= BIT( VRB_CROUCH );

	IN_VRUpdateButtons( buttons );

	if( !active || cls.key_dest != key_game || cl.paused || cl.intermission )
		return;

	// turning (snap or smooth) changes the body direction, the same way the mouse does
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

	// walking. with gaze aim the game already walks where the head points, otherwise rotate the stick
	fwd = vr_in_move_fwd.value;
	side = vr_in_move_side.value;

	if( !vr_aim_gaze.value )
	{
		float a = DEG2RAD( vr_in_head_yaw.value );
		float c = cos( a ), s = sin( a );
		float f2 = fwd * c + side * s;

		side = -fwd * s + side * c;
		fwd = f2;
	}

	cmd->forwardmove += fwd * Cvar_VariableValue( "cl_forwardspeed" );
	cmd->sidemove += side * Cvar_VariableValue( "cl_sidespeed" );

	// Cry of Fear's ladders and boat read these buttons, not the move values
	if( fwd > 0.3f ) SetBits( cmd->buttons, IN_FORWARD );
	if( fwd < -0.3f ) SetBits( cmd->buttons, IN_BACK );
	if( side > 0.3f ) SetBits( cmd->buttons, IN_MOVERIGHT );
	if( side < -0.3f ) SetBits( cmd->buttons, IN_MOVELEFT );

	// gaze aim: what the game aims, uses and walks along is where the head looks.
	// cl.viewangles stays the body direction, so the headset view does not feed back into itself.
	if( vr_aim_gaze.value )
	{
		cmd->viewangles[YAW] = cl.viewangles[YAW] + vr_in_head_yaw.value;
		cmd->viewangles[PITCH] = bound( -89.0f, vr_in_head_pitch.value, 89.0f );
	}
}
