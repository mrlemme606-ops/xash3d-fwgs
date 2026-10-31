/*
in_vr.c - VR controller input
Cry of Fear VR project.
Copyright (C) 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

The renderer DLL (ref/gl/gl_vr_input.c, gl_vr.c, gl_vr_ui.c) tracks the headset and controllers and
publishes the result into the vr_in_* cvars. This file turns them into movement, turning, aiming,
key presses and the mouse cursor for menus.
*/
#include "common.h"
#include "client.h"
#include "ref_common.h"
#include "input.h"
#include "keydefs.h"
#include "platform/platform.h"
#include "vgui_draw.h"

// published by the renderer each frame, not meant to be set by hand
static CVAR_DEFINE_AUTO( vr_in_active, "0", 0, "(internal) VR controllers are active" );
static CVAR_DEFINE_AUTO( vr_in_move_fwd, "0", 0, "(internal) left stick forward, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_move_side, "0", 0, "(internal) left stick sideways, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_turn, "0", 0, "(internal) right stick sideways, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_stick_y, "0", 0, "(internal) right stick up/down, -1..1" );
static CVAR_DEFINE_AUTO( vr_in_head_yaw, "0", 0, "(internal) head yaw relative to the body, degrees" );
static CVAR_DEFINE_AUTO( vr_in_head_pitch, "0", 0, "(internal) head pitch, degrees (positive = down)" );
static CVAR_DEFINE_AUTO( vr_in_hand_valid, "0", 0, "(internal) right hand is tracked" );
static CVAR_DEFINE_AUTO( vr_in_hand_yaw, "0", 0, "(internal) aim yaw toward the right hand's target, relative to the body" );
static CVAR_DEFINE_AUTO( vr_in_hand_pitch, "0", 0, "(internal) aim pitch toward the right hand's target" );
static CVAR_DEFINE_AUTO( vr_in_ui_hit, "0", 0, "(internal) the right hand points at the UI panel" );
static CVAR_DEFINE_AUTO( vr_in_ui_x, "0", 0, "(internal) where on the UI panel, 0..1 from the left" );
static CVAR_DEFINE_AUTO( vr_in_ui_y, "0", 0, "(internal) where on the UI panel, 0..1 from the top" );
static CVAR_DEFINE_AUTO( vr_in_hand_speed, "0", 0, "(internal) right hand speed relative to the head, m/s" );
static CVAR_DEFINE_AUTO( vr_in_swing_yaw, "0", 0, "(internal) aim yaw toward where the right hand is swinging" );
static CVAR_DEFINE_AUTO( vr_in_swing_pitch, "0", 0, "(internal) aim pitch toward where the right hand is swinging" );
static CVAR_DEFINE_AUTO( vr_in_melee, "0", 0, "(internal) the weapon in hand is swung by hand" );
static CVAR_DEFINE_AUTO( vr_in_ray_x, "0", 0, "(internal) right hand position in the game world" );
static CVAR_DEFINE_AUTO( vr_in_ray_y, "0", 0, "(internal) right hand position in the game world" );
static CVAR_DEFINE_AUTO( vr_in_ray_z, "0", 0, "(internal) right hand position in the game world" );
static CVAR_DEFINE_AUTO( vr_in_ray_dx, "0", 0, "(internal) direction the right hand points" );
static CVAR_DEFINE_AUTO( vr_in_ray_dy, "0", 0, "(internal) direction the right hand points" );
static CVAR_DEFINE_AUTO( vr_in_ray_dz, "0", 0, "(internal) direction the right hand points" );
static CVAR_DEFINE_AUTO( vr_in_whip, "0", 0, "(internal) pistol whipping right now" );
static CVAR_DEFINE_AUTO( vr_in_menu, "1", 0, "(internal) a menu or the console is open" );
static CVAR_DEFINE_AUTO( vr_shake, "0", FCVAR_ARCHIVE, "1 = the game may shake the camera (explosions, hits). Off is much easier on the stomach in VR" );
static CVAR_DEFINE_AUTO( vr_in_crouch, "0", 0, "(internal) physically crouching" );
static CVAR_DEFINE_AUTO( vr_in_buttons, "0", 0, "(internal) button bitmask" );

// settings
static CVAR_DEFINE_AUTO( vr_turn_mode, "0", FCVAR_ARCHIVE, "0 = snap turn, 1 = smooth turn" );
static CVAR_DEFINE_AUTO( vr_snap_angle, "30", FCVAR_ARCHIVE, "snap turn angle in degrees" );
static CVAR_DEFINE_AUTO( vr_smooth_speed, "120", FCVAR_ARCHIVE, "smooth turn speed in degrees per second" );
static CVAR_DEFINE_AUTO( vr_aim_mode, "2", FCVAR_ARCHIVE, "what the game aims, uses and shoots along: 0 = body (mouse), 1 = head, 2 = right hand while you use/shoot and head otherwise (default), 3 = right hand always" );
static CVAR_DEFINE_AUTO( vr_move_digital, "1", FCVAR_ARCHIVE, "1 = the left stick presses your movement keys (needed on ladders), 0 = analog speed only" );
static CVAR_DEFINE_AUTO( vr_melee_enable, "1", FCVAR_ARCHIVE, "1 = swinging your hand fast with a melee weapon attacks" );
static CVAR_DEFINE_AUTO( vr_melee_speed, "2.0", FCVAR_ARCHIVE, "hand speed in m/s that counts as a swing" );
static CVAR_DEFINE_AUTO( vr_melee_cooldown, "0.1", FCVAR_ARCHIVE, "seconds between two swings (a swing has to slow down before the next one counts anyway)" );
static CVAR_DEFINE_AUTO( vr_key_swing, "mouse1", FCVAR_ARCHIVE, "key pressed when you swing a melee weapon" );
static CVAR_DEFINE_AUTO( vr_key_whip, "mouse2", FCVAR_ARCHIVE, "key pressed when you swing a gun (pistol whip). empty = nothing" );
static CVAR_DEFINE_AUTO( vr_debug_move, "0", 0, "print the player's movement type when it changes (helps find where analog movement fails)" );

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
static CVAR_DEFINE_AUTO( vr_key_forward, "w", FCVAR_ARCHIVE, "key pressed when the left stick goes forward" );
static CVAR_DEFINE_AUTO( vr_key_back, "s", FCVAR_ARCHIVE, "key pressed when the left stick goes back" );
static CVAR_DEFINE_AUTO( vr_key_left, "a", FCVAR_ARCHIVE, "key pressed when the left stick goes left" );
static CVAR_DEFINE_AUTO( vr_key_right, "d", FCVAR_ARCHIVE, "key pressed when the left stick goes right" );
static CVAR_DEFINE_AUTO( vr_key_jump, "space", FCVAR_ARCHIVE, "key for jump (right stick up)" );
static CVAR_DEFINE_AUTO( vr_key_crouch, "ctrl", FCVAR_ARCHIVE, "key for crouch (right stick down toggles)" );

enum
{
	VRB_RTRIGGER = 0, VRB_LTRIGGER, VRB_RGRIP, VRB_LGRIP, VRB_A, VRB_B, VRB_X, VRB_Y, VRB_LSTICK, VRB_RSTICK,
	VRB_JUMP,	// made here from the right stick flick, not published by the renderer
	VRB_CROUCH,	// likewise
	VRB_FORWARD, VRB_BACK, VRB_LEFT, VRB_RIGHT,	// left stick as keys (digital walking)
	VRB_SWING, VRB_WHIP,	// made here from a fast hand swing
	VRB_COUNT
};

static convar_t *vr_key_cvars[VRB_COUNT] =
{
	&vr_key_rtrigger, &vr_key_ltrigger, &vr_key_rgrip, &vr_key_lgrip, &vr_key_a,
	&vr_key_b, &vr_key_x, &vr_key_y, &vr_key_lstick, &vr_key_rstick,
	&vr_key_jump, &vr_key_crouch,
	&vr_key_forward, &vr_key_back, &vr_key_left, &vr_key_right,
	&vr_key_swing, &vr_key_whip
};

static int vr_pressed_key[VRB_COUNT];	// key code sent on press, 0 = not held
static int vr_prev_buttons;
static qboolean vr_snap_latched;
static qboolean vr_up_latched, vr_down_latched;
static qboolean vr_crouch_toggled;
static int vr_jump_frames;
static qboolean vr_dir_down[4];	// forward, back, left, right (with hysteresis)
static int vr_last_movetype = -1;
static qboolean vr_swing_armed = true;	// the hand has to slow down before the next swing counts
static double vr_whip_until;
static double vr_swing_until;	// aim along the swing until this time
static double vr_melee_next;	// no new swing before this time
static float vr_swing_yaw, vr_swing_pitch;
static int vr_swing_frames;
static qboolean vr_swing_whip;
static double vr_hand_aim_until;	// keep aiming with the hand until this time

// mouse buttons must take the same path as a real mouse (menus and the client listen there), keys can go straight in
static void IN_VRSendKey( int key, qboolean down )
{
	if( key >= K_MOUSE1 && key <= K_MOUSE3 )
		IN_MouseEvent( key - K_MOUSE1, down );
	else Key_Event( key, down );
}

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

// where the right hand is and where it points (game world), used to aim the flashlight
qboolean IN_VRGetRay( vec3_t origin, vec3_t dir )
{
	if( vr_in_active.value <= 0.0f || vr_in_hand_valid.value < 0.5f )
		return false;

	VectorSet( origin, vr_in_ray_x.value, vr_in_ray_y.value, vr_in_ray_z.value );
	VectorSet( dir, vr_in_ray_dx.value, vr_in_ray_dy.value, vr_in_ray_dz.value );
	return true;
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
	Cvar_RegisterVariable( &vr_in_hand_valid );
	Cvar_RegisterVariable( &vr_in_hand_yaw );
	Cvar_RegisterVariable( &vr_in_hand_pitch );
	Cvar_RegisterVariable( &vr_in_ui_hit );
	Cvar_RegisterVariable( &vr_in_ui_x );
	Cvar_RegisterVariable( &vr_in_ui_y );
	Cvar_RegisterVariable( &vr_in_hand_speed );
	Cvar_RegisterVariable( &vr_in_swing_yaw );
	Cvar_RegisterVariable( &vr_in_swing_pitch );
	Cvar_RegisterVariable( &vr_in_melee );
	Cvar_RegisterVariable( &vr_melee_enable );
	Cvar_RegisterVariable( &vr_melee_speed );
	Cvar_RegisterVariable( &vr_melee_cooldown );
	Cvar_RegisterVariable( &vr_in_ray_x );
	Cvar_RegisterVariable( &vr_in_ray_y );
	Cvar_RegisterVariable( &vr_in_ray_z );
	Cvar_RegisterVariable( &vr_in_ray_dx );
	Cvar_RegisterVariable( &vr_in_ray_dy );
	Cvar_RegisterVariable( &vr_in_ray_dz );
	Cvar_RegisterVariable( &vr_in_whip );
	Cvar_RegisterVariable( &vr_in_menu );
	Cvar_RegisterVariable( &vr_shake );
	Cvar_RegisterVariable( &vr_in_crouch );
	Cvar_RegisterVariable( &vr_in_buttons );
	Cvar_RegisterVariable( &vr_turn_mode );
	Cvar_RegisterVariable( &vr_snap_angle );
	Cvar_RegisterVariable( &vr_smooth_speed );
	Cvar_RegisterVariable( &vr_aim_mode );
	Cvar_RegisterVariable( &vr_move_digital );
	Cvar_RegisterVariable( &vr_debug_move );

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
			IN_VRSendKey( vr_pressed_key[i], false );
			vr_pressed_key[i] = 0;
		}
	}

	vr_prev_buttons = 0;
	vr_crouch_toggled = false;
	vr_jump_frames = 0;
	memset( vr_dir_down, 0, sizeof( vr_dir_down ));
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
				IN_VRSendKey( key, true );
			}
		}
		else if( vr_pressed_key[i] )
		{
			// release exactly the key we pressed, even if the cvar changed meanwhile
			IN_VRSendKey( vr_pressed_key[i], false );
			vr_pressed_key[i] = 0;
		}
	}

	vr_prev_buttons = buttons;
}

static qboolean IN_VRHysteresis( qboolean was_down, float value )
{
	if( value > 0.55f ) return true;
	if( value < 0.35f ) return false;
	return was_down;
}

// laser pointer: when the right hand points at the floating UI panel and a menu wants the cursor, move it there
// where the laser pointed over the last moments, to click where you aimed and not where the trigger pull moved you
static float vr_cur_x[16], vr_cur_y[16];
static double vr_cur_t[16];
static int vr_cur_n;
static qboolean vr_cur_locked;
static int vr_cur_lock_x, vr_cur_lock_y;

static void IN_VRMoveCursor( void )
{
	int x, y;
	qboolean trigger = FBitSet( (int)vr_in_buttons.value, BIT( VRB_RTRIGGER ));

	if( !trigger )
		vr_cur_locked = false;

	if( vr_in_ui_hit.value < 0.5f )
		return;

	if( cls.key_dest != key_menu && !host.mouse_visible )
		return;

	x = (int)( vr_in_ui_x.value * refState.width );
	y = (int)( vr_in_ui_y.value * refState.height );
	x = bound( 0, x, refState.width - 1 );
	y = bound( 0, y, refState.height - 1 );

	vr_cur_x[vr_cur_n & 15] = x;
	vr_cur_y[vr_cur_n & 15] = y;
	vr_cur_t[vr_cur_n & 15] = host.realtime;
	vr_cur_n++;

	if( trigger && !vr_cur_locked )
	{
		// pulling the trigger moves the controller a little: click where you pointed a moment ago
		int idx = vr_cur_n - 1, k;

		for( k = 0; k < 15 && idx > 0 && vr_cur_t[idx & 15] > host.realtime - 0.10; k++ )
			idx--;

		vr_cur_lock_x = (int)vr_cur_x[idx & 15];
		vr_cur_lock_y = (int)vr_cur_y[idx & 15];
		vr_cur_locked = true;
	}

	// held still while the trigger is down, so a click is never lost to a small movement
	if( vr_cur_locked )
	{
		x = vr_cur_lock_x;
		y = vr_cur_lock_y;
	}

	// the real cursor first (client code that reads the OS cursor), then the exact position for the UI,
	// so a window larger than the desktop can't clamp where the laser points
	Platform_SetMousePos( x, y );
	VGui_MouseMove( x, y );
	UI_MouseMove( x, y );
}

void IN_VRAppendMove( float frametime, usercmd_t *cmd, qboolean active )
{
	float yaw = 0.0f;
	float x, y, fwd, side;
	float head_rel, aim_rel, aim_pitch, rot;
	int buttons, mode;
	qboolean in_game, on_ladder, digital;

	{
		float menu = cls.key_dest != key_game ? 1.0f : 0.0f;

		if( vr_in_menu.value != menu )
			Cvar_DirectSetValue( &vr_in_menu, menu );
	}

	if( vr_in_active.value <= 0.0f )
	{
		IN_VRReleaseAll();
		return;
	}

	{
		float whip = host.realtime < vr_whip_until ? 1.0f : 0.0f;

		if( vr_in_whip.value != whip )
			Cvar_DirectSetValue( &vr_in_whip, whip );
	}

	IN_VRMoveCursor();

	in_game = active && cls.key_dest == key_game && !cl.paused && !cl.intermission;
	buttons = (int)vr_in_buttons.value;

	// where does the game aim? 0 = body (mouse), 1 = head, 2 = hand while acting, 3 = hand always
	head_rel = vr_in_head_yaw.value;
	mode = (int)vr_aim_mode.value;

	// using something or shooting: aim with the hand for a moment (walking keeps following the head otherwise)
	if( buttons & ( BIT( VRB_RTRIGGER ) | BIT( VRB_LTRIGGER ) | BIT( VRB_RGRIP ) | BIT( VRB_B )))
		vr_hand_aim_until = host.realtime + 0.35;

	// swinging a weapon by hand: a fast hand movement attacks, aimed where the hand is going
	if( vr_in_hand_speed.value < vr_melee_speed.value * 0.5f )
		vr_swing_armed = true;

	if( in_game && vr_melee_enable.value && vr_in_hand_valid.value > 0.5f && vr_swing_armed
		&& vr_in_hand_speed.value > vr_melee_speed.value && host.realtime >= vr_melee_next
		&& ( vr_in_melee.value > 0.5f || !COM_StringEmptyOrNULL( vr_key_whip.string )))
	{
		vr_melee_next = host.realtime + vr_melee_cooldown.value;
		vr_swing_until = host.realtime + 0.3;
		vr_swing_yaw = vr_in_swing_yaw.value;
		vr_swing_pitch = vr_in_swing_pitch.value;
		vr_swing_frames = 3;
		vr_swing_whip = vr_in_melee.value < 0.5f;
		vr_swing_armed = false;

		// a gun being whipped holds still on screen for a moment
		if( vr_swing_whip )
			vr_whip_until = host.realtime + 0.7;
	}

	if( !in_game )
		vr_swing_frames = 0;

	if( vr_swing_frames > 0 )
	{
		buttons |= BIT( vr_swing_whip ? VRB_WHIP : VRB_SWING );
		vr_swing_frames--;
	}

	if( host.realtime < vr_swing_until )
	{
		mode = 2;
		aim_rel = vr_swing_yaw;
		aim_pitch = vr_swing_pitch;
	}
	else if( vr_in_hand_valid.value > 0.5f && ( mode >= 3 || ( mode == 2 && host.realtime < vr_hand_aim_until )))
	{
		mode = 2;
		aim_rel = vr_in_hand_yaw.value;
		aim_pitch = vr_in_hand_pitch.value;
	}
	else if( mode >= 1 )
	{
		mode = 1;
		aim_rel = head_rel;
		aim_pitch = vr_in_head_pitch.value;
	}
	else
	{
		mode = 0;
		aim_rel = 0.0f;
		aim_pitch = 0.0f;
	}

	if( Cvar_VariableValue( "vr_aim_debug" ) > 0.0f && (( buttons & ~vr_prev_buttons ) & ( BIT( VRB_RTRIGGER ) | BIT( VRB_RGRIP ))))
	{
		Con_Printf( "VR trigger/grip pressed: sending yaw %.1f (body %.1f) pitch %.1f, aim from %s (hand valid %d, hand yaw %.1f pitch %.1f)\n",
			cl.viewangles[YAW] + aim_rel, cl.viewangles[YAW], aim_pitch, mode == 2 ? "hand" : ( mode == 1 ? "head" : "body" ),
			(int)vr_in_hand_valid.value, vr_in_hand_yaw.value, vr_in_hand_pitch.value );
	}

	// the stick means "walk where I look". the game walks along its aim, so rotate the stick by the difference
	rot = head_rel - aim_rel;
	{
		float a = DEG2RAD( rot );
		float c = cos( a ), s = sin( a );

		fwd = vr_in_move_fwd.value * c + vr_in_move_side.value * s;
		side = -vr_in_move_fwd.value * s + vr_in_move_side.value * c;
	}

	// ladders only react to real key presses, so use the keys there
	on_ladder = clgame.pmove && clgame.pmove->movetype == MOVETYPE_FLY;

	if( vr_debug_move.value && clgame.pmove && clgame.pmove->movetype != vr_last_movetype )
	{
		vr_last_movetype = clgame.pmove->movetype;
		Con_Printf( "VR: movetype is now %d\n", vr_last_movetype );
	}

	digital = vr_move_digital.value && ( on_ladder || fabs( rot ) < 15.0f );

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

	// digital walking: press the player's own movement keys, so everything the keyboard can do works
	if( digital && in_game )
	{
		vr_dir_down[0] = IN_VRHysteresis( vr_dir_down[0], fwd );
		vr_dir_down[1] = IN_VRHysteresis( vr_dir_down[1], -fwd );
		vr_dir_down[2] = IN_VRHysteresis( vr_dir_down[2], -side );
		vr_dir_down[3] = IN_VRHysteresis( vr_dir_down[3], side );

		if( vr_dir_down[0] ) buttons |= BIT( VRB_FORWARD );
		if( vr_dir_down[1] ) buttons |= BIT( VRB_BACK );
		if( vr_dir_down[2] ) buttons |= BIT( VRB_LEFT );
		if( vr_dir_down[3] ) buttons |= BIT( VRB_RIGHT );
	}
	else
	{
		memset( vr_dir_down, 0, sizeof( vr_dir_down ));
	}

	IN_VRUpdateButtons( buttons );

	if( !in_game )
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

	// analog walking, when the keys are not doing it
	if( !digital )
	{
		cmd->forwardmove += fwd * Cvar_VariableValue( "cl_forwardspeed" );
		cmd->sidemove += side * Cvar_VariableValue( "cl_sidespeed" );

		if( fwd > 0.3f ) SetBits( cmd->buttons, IN_FORWARD );
		if( fwd < -0.3f ) SetBits( cmd->buttons, IN_BACK );
		if( side > 0.3f ) SetBits( cmd->buttons, IN_MOVERIGHT );
		if( side < -0.3f ) SetBits( cmd->buttons, IN_MOVELEFT );
	}

	// what the game aims, uses and shoots along. cl.viewangles stays the body direction, so the
	// headset view does not feed back into itself.
	if( mode > 0 )
	{
		cmd->viewangles[YAW] = cl.viewangles[YAW] + aim_rel;
		cmd->viewangles[PITCH] = bound( -89.0f, aim_pitch, 89.0f );
	}
}
