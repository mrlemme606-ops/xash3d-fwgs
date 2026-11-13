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
#include "cl_tent.h"

// published by the renderer each frame, not meant to be set by hand
static CVAR_DEFINE_AUTO( vr_in_active, "0", 0, "(internal) VR controllers are active" );
static CVAR_DEFINE_AUTO( vr_sound_debug, "0", 0, "1 = print the names of the sounds your own character makes (to mute the extra footstep)" );
static CVAR_DEFINE_AUTO( vr_in_kbd, "0", 0, "(internal) the keyboard should be open" );
static CVAR_DEFINE_AUTO( vr_in_kbd_chars, "", 0, "(internal) typed characters waiting to be sent to the game" );
static CVAR_DEFINE_AUTO( vr_in_kbd_closed, "0", 0, "(internal) the player closed the keyboard" );
static CVAR_DEFINE_AUTO( vr_in_room_quiet, "0", 0, "(internal) the player only moves because he walks around his room" );
static CVAR_DEFINE_AUTO( vr_quiet_steps, "step+pl_dirt+pl_duct+pl_grate+pl_metal+pl_tile+pl_slosh+pl_wade+pl_snow+pl_grass+pl_gravel+pl_wood+pl_concrete", FCVAR_ARCHIVE, "sound names (parts, separated by +) that are not played while you only move by walking around your room" );
static CVAR_DEFINE_AUTO( vr_in_room_x, "0", 0, "(internal) how far you stepped away from your character, game axes" );
static CVAR_DEFINE_AUTO( vr_in_room_y, "0", 0, "(internal) how far you stepped away from your character, game axes" );
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
CVAR_DEFINE_AUTO( vr_melee_instant, "1", FCVAR_ARCHIVE, "1 = a real swing makes the hit of the weapon land at once (the game waits 0.3 to 0.4 s after the attack before the hit)" );
double vr_melee_window_until;	// read by the server (sv_phys.c): until then a melee hit that is waiting is brought forward
static CVAR_DEFINE_AUTO( vr_swing_sound, "1", FCVAR_ARCHIVE, "1 = a real swing plays the weapon's swing sound at once, and the game's own late swing sound is muted" );
static CVAR_DEFINE_AUTO( vr_swing_sounds, "swing+miss+slash+whoosh+woosh+whip", FCVAR_ARCHIVE, "name parts of the game's swing sounds (separated by +)" );
static CVAR_DEFINE_AUTO( vr_melee_hit_sounds, "hitbody+melee_hit", FCVAR_ARCHIVE, "name parts of the sounds of a hit that hurts someone (separated by +). The hand only vibrates when one plays right after a swing. vr_sound_debug 2 lists the sounds after a swing" );
static CVAR_DEFINE_AUTO( vr_melee_hold, "0.12", FCVAR_ARCHIVE, "seconds the attack button is held for a real swing" );
static CVAR_DEFINE_AUTO( vr_in_qs_pick, "0", 0, "(internal) the quick slot the renderer says the hand is over" );
static CVAR_DEFINE_AUTO( vr_in_qs_on, "0", 0, "(internal) the quick slot selector is open" );
static CVAR_DEFINE_AUTO( vr_in_qs_sel, "0", 0, "(internal) the quick slot the hand is over (0 = none)" );
static CVAR_DEFINE_AUTO( vr_in_qs_x, "0", 0, "(internal) where the selector opened, x" );
static CVAR_DEFINE_AUTO( vr_in_qs_y, "0", 0, "(internal) where the selector opened, y" );
static CVAR_DEFINE_AUTO( vr_in_qs_z, "0", 0, "(internal) where the selector opened, z" );
static CVAR_DEFINE_AUTO( vr_in_qs_yaw, "0", 0, "(internal) body yaw while the selector is open" );
static CVAR_DEFINE_AUTO( vr_in_ui_lkbd, "0", 0, "(internal) the left hand points at a key of the computer keyboard" );
static CVAR_DEFINE_AUTO( vr_in_ui_lhit, "0", 0, "(internal) the left hand points at the UI panel" );
static CVAR_DEFINE_AUTO( vr_in_ui_lx, "0", 0, "(internal) left hand: where on the UI panel, 0..1 from the left" );
static CVAR_DEFINE_AUTO( vr_in_ui_ly, "0", 0, "(internal) left hand: where on the UI panel, 0..1 from the top" );
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
static CVAR_DEFINE_AUTO( vr_in_hud_off, "0", 0, "(internal) a full screen UI or a cutscene is shown: the HUD stays on the screen, not on the hand" );
static CVAR_DEFINE_AUTO( vr_in_haptic_len, "1800", 0, "(internal) length of one vibration pulse in microseconds (at most 3999)" );
static CVAR_DEFINE_AUTO( vr_slot_debug, "0", 0, "1 = print the quick slot messages of the game (to find out which items are in the slots)" );
static CVAR_DEFINE_AUTO( vr_in_climb, "0", 0, "(internal) on a ladder or a boat: the hands are hidden" );
static CVAR_DEFINE_AUTO( vr_in_haptic, "0", 0, "(internal) ask the renderer for a vibration: 100 * hand (0 left, 1 right) + number of pulses" );
static CVAR_DEFINE_AUTO( vr_in_lhand_speed, "0", 0, "(internal) left hand speed in m/s" );
static CVAR_DEFINE_AUTO( vr_in_lswing_yaw, "0", 0, "(internal) aim yaw toward where the left hand is swinging" );
static CVAR_DEFINE_AUTO( vr_in_lswing_pitch, "0", 0, "(internal) aim pitch toward where the left hand is swinging" );
static CVAR_DEFINE_AUTO( vr_in_swing_hit, "0", 0, "(internal) the right hand swing would hit something" );
static CVAR_DEFINE_AUTO( vr_in_lswing_hit, "0", 0, "(internal) the left hand swing would hit something" );
static CVAR_DEFINE_AUTO( vr_in_fists, "0", 0, "(internal) nothing in the hand: fists" );
static CVAR_DEFINE_AUTO( vr_in_oneh, "0", 0, "(internal) a one handed item (phone, flare, lantern) in the right hand" );
static CVAR_DEFINE_AUTO( vr_in_lhand_yaw, "0", 0, "(internal) aim yaw of the left hand" );
static CVAR_DEFINE_AUTO( vr_in_lhand_pitch, "0", 0, "(internal) aim pitch of the left hand" );
static CVAR_DEFINE_AUTO( vr_in_dual, "0", 0, "(internal) dual wielding" );
static CVAR_DEFINE_AUTO( vr_in_dual_lmelee, "0", 0, "(internal) the left hand weapon is a melee weapon" );
static CVAR_DEFINE_AUTO( vr_key_fist, "mouse2", FCVAR_ARCHIVE, "key pressed for a punch (fists, and the left hand when it throws punches or swings the left weapon)" );
static CVAR_DEFINE_AUTO( vr_in_menu, "1", 0, "(internal) a menu or the console is open" );
static CVAR_DEFINE_AUTO( vr_shake, "0", FCVAR_ARCHIVE, "1 = the game may shake the camera (explosions, hits). Off is much easier on the stomach in VR" );
static CVAR_DEFINE_AUTO( vr_in_dir_yaw, "0", 0, "(internal) yaw of the direction the right hand points, relative to the body" );
static CVAR_DEFINE_AUTO( vr_in_dir_pitch, "0", 0, "(internal) pitch of the direction the right hand points" );
static CVAR_DEFINE_AUTO( vr_in_crouch, "0", 0, "(internal) physically crouching" );
static CVAR_DEFINE_AUTO( vr_in_buttons, "0", 0, "(internal) button bitmask" );
static CVAR_DEFINE_AUTO( vr_in_hit_x, "0", 0, "(internal) where the hand ray hit the world" );
static CVAR_DEFINE_AUTO( vr_in_hit_y, "0", 0, "(internal)" );
static CVAR_DEFINE_AUTO( vr_in_hit_z, "0", 0, "(internal)" );
static CVAR_DEFINE_AUTO( vr_in_shot, "0", 0, "(internal) counts the shots the weapon in hand has fired" );
static CVAR_DEFINE_AUTO( vr_in_duck_drop, "0", 0, "(internal) how many units the game has lowered the view for the character's own crouch" );
static CVAR_DEFINE_AUTO( vr_in_hit_ok, "0", 0, "(internal) the hand ray hits the world" );
static CVAR_DEFINE_AUTO( vr_bullet_holes, "0", FCVAR_ARCHIVE, "1 = a bullet hole appears where the hand ray hits when you fire a gun (to see where you aim)" );
static CVAR_DEFINE_AUTO( vr_in_mask, "0", 0, "(internal) buttons the reload system keeps away from the game" );
static CVAR_DEFINE_AUTO( vr_in_noswing, "0", 0, "(internal) no melee swings right now (reloading)" );
static CVAR_DEFINE_AUTO( vr_in_reload, "0", 0, "(internal) the reload system finished a manual reload: press the reload key" );

// settings
static CVAR_DEFINE_AUTO( vr_turn_mode, "1", FCVAR_ARCHIVE, "0 = snap turn, 1 = smooth turn, 2 = no turning" );
static CVAR_DEFINE_AUTO( vr_move_ref, "0", FCVAR_ARCHIVE, "what the left stick walks along: 0 = where your head looks, 1 = where your left hand points" );
static CVAR_DEFINE_AUTO( vr_menu_debug, "0", 0, "1 = print every command the game sends (helps find the button of the game's own main menu that opens the options)" );
static CVAR_DEFINE_AUTO( vr_snap_angle, "30", FCVAR_ARCHIVE, "snap turn angle in degrees" );
static CVAR_DEFINE_AUTO( vr_smooth_speed, "120", FCVAR_ARCHIVE, "smooth turn speed in degrees per second" );
static CVAR_DEFINE_AUTO( vr_aim_mode, "2", FCVAR_ARCHIVE, "what the game aims, uses and shoots along: 0 = body (mouse), 1 = head, 2 = right hand while you use/shoot and head otherwise (default), 3 = right hand always" );
static CVAR_DEFINE_AUTO( vr_move_digital_ents, "", FCVAR_ARCHIVE, "names of entities (as vr_debug_move shows them, like *16) that always count as a boat or train to stand on" );
static CVAR_DEFINE_AUTO( vr_comp_keyboard, "1", FCVAR_ARCHIVE, "1 = a small keyboard (a-z, 0-9) is drawn over the computer screen and typed with the laser and trigger" );
static CVAR_DEFINE_AUTO( vr_move_cmd, "1000", FCVAR_ARCHIVE, "walking command a fully pushed stick sends (the game clamps it to its own maximum speed)" );
static CVAR_DEFINE_AUTO( vr_move_deadzone, "0.12", FCVAR_ARCHIVE, "left stick dead zone for walking (0..1)" );
static CVAR_DEFINE_AUTO( vr_move_full, "0.95", FCVAR_ARCHIVE, "stick deflection that counts as full speed (0..1)" );
static CVAR_DEFINE_AUTO( vr_move_digital, "0", FCVAR_ARCHIVE, "1 = the left stick presses your movement keys (needed on ladders), 0 = analog speed only" );
static CVAR_DEFINE_AUTO( vr_use_aim, "2", FCVAR_ARCHIVE, "what the use key (right grip) aims along: 0 = your head, 1 = the exact point your hand ray hits, 2 = the direction your hand points" );
static CVAR_DEFINE_AUTO( vr_melee_enable, "1", FCVAR_ARCHIVE, "1 = swinging your hand fast with a melee weapon attacks" );
static CVAR_DEFINE_AUTO( vr_melee_speed, "3.5", FCVAR_ARCHIVE, "hand speed in m/s that counts as a swing" );
static CVAR_DEFINE_AUTO( vr_melee_cooldown, "0.3", FCVAR_ARCHIVE, "seconds between two swings (a swing has to slow down before the next one counts anyway)" );
static CVAR_DEFINE_AUTO( vr_key_swing, "mouse1", FCVAR_ARCHIVE, "key pressed when you swing a melee weapon" );
static CVAR_DEFINE_AUTO( vr_key_whip, "mouse2", FCVAR_ARCHIVE, "key pressed when you swing a gun (pistol whip). empty = nothing" );
static CVAR_DEFINE_AUTO( vr_key_reload, "r", FCVAR_ARCHIVE, "key pressed for you when a manual reload is finished (the game then does the actual reload)" );
static CVAR_DEFINE_AUTO( vr_debug_move, "0", 0, "print the player's movement type when it changes (helps find where analog movement fails)" );

// which key each controller button presses. single character, or mouse1/2/3, space, ctrl, shift, enter, tab, escape. empty = nothing
static CVAR_DEFINE_AUTO( vr_key_rtrigger, "mouse1", FCVAR_ARCHIVE, "key for right trigger" );
static CVAR_DEFINE_AUTO( vr_key_ltrigger, "", FCVAR_ARCHIVE, "key for left trigger (empty = nothing)" );
static CVAR_DEFINE_AUTO( vr_key_rgrip, "e", FCVAR_ARCHIVE, "key for right grip" );
static CVAR_DEFINE_AUTO( vr_key_lgrip, "f", FCVAR_ARCHIVE, "key for left grip" );
static CVAR_DEFINE_AUTO( vr_key_a, "space", FCVAR_ARCHIVE, "key for A (right controller)" );
static CVAR_DEFINE_AUTO( vr_key_b, "r", FCVAR_ARCHIVE, "key for B (right controller)" );
static CVAR_DEFINE_AUTO( vr_key_x, "tab", FCVAR_ARCHIVE, "key for X (left controller)" );
static CVAR_DEFINE_AUTO( vr_key_y, "escape", FCVAR_ARCHIVE, "key for Y (left controller)" );
static CVAR_DEFINE_AUTO( vr_key_lstick, "shift", FCVAR_ARCHIVE, "key for left stick click (sprint)" );
static CVAR_DEFINE_AUTO( vr_key_rstick, "!weapontoggle", FCVAR_ARCHIVE, "right stick click: !special = the weapon's own function (fire mode, phone call, tacklight). Or a key name" );
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
	VRB_SWING, VRB_WHIP, VRB_FIST,	// made here from a fast hand swing
	VRB_RELOAD,	// made here when a manual reload is finished
	VRB_COUNT
};

static convar_t *vr_key_cvars[VRB_COUNT] =
{
	&vr_key_rtrigger, &vr_key_ltrigger, &vr_key_rgrip, &vr_key_lgrip, &vr_key_a,
	&vr_key_b, &vr_key_x, &vr_key_y, &vr_key_lstick, &vr_key_rstick,
	&vr_key_jump, &vr_key_crouch,
	&vr_key_forward, &vr_key_back, &vr_key_left, &vr_key_right,
	&vr_key_swing, &vr_key_whip, &vr_key_fist, &vr_key_reload
};

static int vr_pressed_key[VRB_COUNT];	// key code sent on press, 0 = not held
static int vr_prev_buttons;
static qboolean vr_snap_latched;
static qboolean vr_up_latched, vr_down_latched;
static qboolean vr_crouch_toggled, vr_dodging, vr_qs_active, vr_lclick_down;
static int vr_qs_sel;
static vec3_t vr_qs_start;
extern void S_VRSwingStart( int weapon, int hand );
static int vr_cur_weapon = -1;
static double vr_swing_hold, vr_lswing_hold;
static float vr_qs_yaw;	// the way the hand pointed when the selector opened (radians): 1 2 3 run from its left to its right
static double vr_a_since;
static int vr_jump_frames;
static qboolean vr_dir_down[4];	// forward, back, left, right (with hysteresis)
static int vr_last_movetype = -1;
static qboolean vr_last_boat;
static qboolean vr_swing_armed = true, vr_lswing_armed = true, vr_swing_fist, vr_lmelee_next_dummy;
static double vr_lmelee_next, vr_lswing_until;
static float vr_lswing_yaw, vr_lswing_pitch;
static int vr_lswing_frames;	// the hand has to slow down before the next swing counts
static double vr_whip_until;
static double vr_use_until;	// the use key was pressed: aim it until this time
static double vr_lock_until;	// the body does not turn by itself until this time (melee)
static float vr_lock_yaw;
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


// ---- the keyboard of the computer screen: a plain QWERTY (a-z, 0-9) drawn over the screen, typed with the laser and trigger
static int vr_cursor_px, vr_cursor_py;
static int vr_menu_page;	// our own pause menu: 0 closed, 1 pause, 2 options, 3 load game
static void IN_VRMenuDraw( void );
static void IN_VRMenuClick( void );
static qboolean IN_VRMenuToggle( void );
static void IN_VRMenu_f( void );
static qboolean kbd_manual, kbd_dismissed, kbd_cmd_open, kbd_comp_open;
static void IN_VRTypeChar( int ch );

typedef struct { const char *label; int ch; } vr_key_t;

static const vr_key_t vr_kbd_keys[4][11] =
{
	{ {"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},{"5",'5'},{"6",'6'},{"7",'7'},{"8",'8'},{"9",'9'},{"0",'0'},{"<-",'\b'} },
	{ {"q",'q'},{"w",'w'},{"e",'e'},{"r",'r'},{"t",'t'},{"y",'y'},{"u",'u'},{"i",'i'},{"o",'o'},{"p",'p'},{"OK",'\n'} },
	{ {"a",'a'},{"s",'s'},{"d",'d'},{"f",'f'},{"g",'g'},{"h",'h'},{"j",'j'},{"k",'k'},{"l",'l'},{NULL,0},{NULL,0} },
	{ {"z",'z'},{"x",'x'},{"c",'c'},{"v",'v'},{"b",'b'},{"n",'n'},{"m",'m'},{"_",' '},{NULL,0},{NULL,0},{NULL,0} }
};

static qboolean IN_VRKbdShown( void )
{
	return kbd_comp_open && !kbd_dismissed && cls.state == ca_active && Cvar_VariableInteger( "vr_enable" ) && Cvar_VariableInteger( "vr_comp_keyboard" );
}

static void IN_VRKbdRect( int *x0, int *y0, int *cell )
{
	int w = refState.width, h = refState.height;
	int c = Q_min( w * 76 / 100 / 11, h * 50 / 100 / 4 );

	// on the left, so the X that closes the computer (bottom right of the screen) stays free
	*cell = c;
	*x0 = w * 2 / 100;
	*y0 = h - c * 4 - h / 50;
}

static qboolean IN_VRKbdHit( int px, int py, int *ch )
{
	int x0, y0, c, row, col;

	IN_VRKbdRect( &x0, &y0, &c );

	if( px < x0 || py < y0 || px >= x0 + c * 11 || py >= y0 + c * 4 )
		return false;

	row = ( py - y0 ) / c;
	col = ( px - x0 ) / c;

	if( !vr_kbd_keys[row][col].label )
		return true;	// inside the board, on nothing: swallow the click anyway

	*ch = vr_kbd_keys[row][col].ch;
	return true;
}

static void IN_VRDrawQuickSlots( void )
{
	int w = refState.width, h = refState.height, c = h / 7, i;
	int x0 = ( w - c * 3 - 2 * ( c / 8 )) / 2, y0 = h - c - h / 12;
	rgba_t white = { 255, 255, 255, 255 };

	// the slots are drawn in the world by the renderer now (above the right hand); the flat bar is only for flat play
	if( !vr_qs_active || Cvar_VariableInteger( "vr_enable" ))
		return;

	ref.dllFuncs.R_Set2DMode( true );

	for( i = 0; i < 3; i++ )
	{
		char label[2] = { '1' + i, 0 };
		int x = x0 + i * ( c + c / 8 ), lw = 0, lh = 0, on = ( vr_qs_sel == i + 1 );

		ref.dllFuncs.FillRGBA( kRenderTransTexture, x, y0, c, c, on ? 200 : 20, on ? 160 : 20, on ? 40 : 20, 230 );
		CL_DrawStringLen( &cls.creditsFont, label, &lw, &lh, 0 );
		CL_DrawString( x + ( c - lw ) / 2, y0 + ( c - lh ) / 2, label, white, &cls.creditsFont, FONT_DRAW_NORENDERMODE );
	}
}

void IN_VRDrawKeyboard( void )
{
	IN_VRDrawQuickSlots();
	IN_VRMenuDraw();

	int x0, y0, c, row, col, ch;
	qboolean hit;
	rgba_t white = { 255, 255, 255, 255 };
	static cl_font_t kbd_font;

	int lpx = (int)( vr_in_ui_lx.value * refState.width ), lpy = (int)( vr_in_ui_ly.value * refState.height ), lch = 0;
	qboolean lhit_kbd;

	if( !IN_VRKbdShown( ))
	{
		if( vr_in_ui_lkbd.value > 0.5f )
			Cvar_DirectSetValue( &vr_in_ui_lkbd, 0.0f );
		return;
	}

	// the left hand's cursor only exists over the keyboard
	lhit_kbd = vr_in_ui_lhit.value > 0.5f && IN_VRKbdHit( lpx, lpy, &lch );

	if(( vr_in_ui_lkbd.value > 0.5f ) != lhit_kbd )
		Cvar_DirectSetValue( &vr_in_ui_lkbd, lhit_kbd ? 1.0f : 0.0f );

	// the letters get a font of their own, three times the size
	if( !kbd_font.valid && cls.creditsFont.rendermode )
	{
		if( !Con_LoadVariableWidthFont( "gfx/creditsfont.fnt", &kbd_font, 3.0f, cls.creditsFont.rendermode, TF_FONT ))
			Con_LoadFixedWidthFont( "gfx/conchars", &kbd_font, 4.0f, cls.creditsFont.rendermode, TF_FONT | TF_NEAREST | TF_KEEP_SOURCE );
	}

	IN_VRKbdRect( &x0, &y0, &c );
	ref.dllFuncs.R_Set2DMode( true );
	hit = IN_VRKbdHit( vr_cursor_px, vr_cursor_py, &ch );

	for( row = 0; row < 4; row++ )
	{
		for( col = 0; col < 11; col++ )
		{
			const vr_key_t *k = &vr_kbd_keys[row][col];
			int x = x0 + col * c, y = y0 + row * c;
			int lw = 0, lh = 0;
			int over = hit && k->label && vr_cursor_px >= x && vr_cursor_px < x + c && vr_cursor_py >= y && vr_cursor_py < y + c;

			if( lhit_kbd && k->label && lpx >= x && lpx < x + c && lpy >= y && lpy < y + c )
				over = true;

			if( !k->label )
				continue;

			ref.dllFuncs.FillRGBA( kRenderTransTexture, x + 2, y + 2, c - 4, c - 4, over ? 90 : 20, over ? 90 : 20, over ? 90 : 20, 230 );
			CL_DrawStringLen( kbd_font.valid ? &kbd_font : &cls.creditsFont, k->label, &lw, &lh, 0 );
			CL_DrawString( x + ( c - lw ) / 2, y + ( c - lh ) / 2, k->label, white, kbd_font.valid ? &kbd_font : &cls.creditsFont, FONT_DRAW_NORENDERMODE );
		}
	}
}

// ---- SteamVR keyboard: the characters typed on it become key presses for the game
static qboolean kbd_manual, kbd_dismissed, kbd_cmd_open;
static float room_until;

static void IN_VRKeyboard_f( void )
{
	kbd_manual = !kbd_manual;
	kbd_dismissed = false;
}


static void IN_VRTypeChar( int ch )
{
	static const char shifted[] = "!@#$%^&*()_+:\"<>?{}|~";
	static const char plain[] = "1234567890-=;',./[]\\`";
	const char *sp;
	int key = ch;
	qboolean shift = false;

	if( ch == '\b' ) key = K_BACKSPACE;
	else if( ch == '\r' || ch == '\n' ) key = K_ENTER;
	else if( ch >= 'A' && ch <= 'Z' ) { key = ch + 32; shift = true; }
	else if( ch > 32 && ( sp = Q_strchr( shifted, ch )) != NULL ) { key = plain[sp - shifted]; shift = true; }

	if( cls.key_dest != key_game && ch >= 32 )
	{
		CL_CharEvent( ch );	// the console and the menu take text directly
		return;
	}

	if( shift ) Key_Event( K_SHIFT, true );
	Key_Event( key, true );
	Key_Event( key, false );
	if( shift ) Key_Event( K_SHIFT, false );
}

static qboolean IN_VRNameInList( const char *list, const char *name )
{
	const char *p = list;

	while( *p )
	{
		char tok[48];
		int n = 0;

		while( *p == '+' || *p == ' ' ) p++;
		while( *p && *p != '+' && n < 47 ) tok[n++] = *p++;
		tok[n] = 0;

		if( n && Q_stristr( name, tok ))
			return true;
	}

	return false;
}

// The computer log-on screen is a panel of the game's client.dll. The server opens it with the user message
// "Computer", and closing it makes the game send the command "stopcomputer". That is all we need to know.
static qboolean kbd_comp_open;

qboolean IN_VRActive( void )
{
	return Cvar_VariableInteger( "vr_enable" ) != 0;
}

void IN_VRUserMsg( const char *name, const byte *buf, int size )
{
	static int last_clip = -1, last_weapon = -1;
	static double last_shot;

	if( !name || !Cvar_VariableInteger( "vr_enable" ))
		return;

	// vibrations: damage (both hands), a weapon being taken out, every shot (stronger for slow weapons, short for rapid fire)
	if( !Q_strcmp( name, "Damage" ))
	{
		Cvar_DirectSetValue( &vr_in_haptic_len, 3500.0f );
		Cvar_DirectSetValue( &vr_in_haptic, 203.0f );
	}
	else if( !Q_strcmp( name, "WpnDeploy" ))
	{
		Cvar_DirectSetValue( &vr_in_haptic_len, 2200.0f );
		Cvar_DirectSetValue( &vr_in_haptic, 101.0f );
	}
	else if( !Q_strcmp( name, "CurWeapon" ) && size >= 3 )
	{
		int weapon = buf[1], clip = (signed char)buf[2];

		if( buf[0] )
			vr_cur_weapon = weapon;

		// the clip of every weapon is remembered on its own: the first shot after taking a weapon out used to be
		// missed when the game announced another weapon in between
		static int clip_of[256];
		static qboolean clip_known[256];
		int before = ( clip_known[weapon & 255] ) ? clip_of[weapon & 255] : last_clip;

		if( before >= 0 && clip >= 0 && clip < before && ( weapon == last_weapon || clip_known[weapon & 255] ))
		{
			double gap = host.realtime - last_shot;

			Cvar_DirectSetValue( &vr_in_haptic_len, gap < 0.15 ? 1200.0f : ( gap < 0.5 ? 2600.0f : 3999.0f ));
			// dual wield: the hand whose trigger is held feels the shot (both when both are held)
			{
				float code = 101.0f;

				if( vr_in_dual.value > 0.5f )
				{
					qboolean lt = FBitSet( vr_prev_buttons, BIT( VRB_LTRIGGER )), rt = FBitSet( vr_prev_buttons, BIT( VRB_RTRIGGER ));

					if( lt && !rt ) code = 1.0f;
					else if( lt && rt ) code = 201.0f;
				}

				Cvar_DirectSetValue( &vr_in_haptic, code );
			}
			last_shot = host.realtime;
		}

		if( clip >= 0 )
		{
			clip_of[weapon & 255] = clip;
			clip_known[weapon & 255] = true;
		}

		last_weapon = weapon;
		last_clip = clip;
	}
	else if( !Q_strcmp( name, "QSlotUpdate" ) && Cvar_VariableInteger( "vr_slot_debug" ))
	{
		int i;

		Con_Printf( "VR slots: QSlotUpdate %d bytes:", size );
		for( i = 0; i < size && i < 32; i++ )
			Con_Printf( " %d", buf[i] );
		Con_Printf( "\n" );
	}

	// a new text message on Simon's phone: two short vibrations in the right hand
	if( !Q_strcmp( name, "UnreadSMS" ))
	{
		Cvar_DirectSetValue( &vr_in_haptic_len, 1800.0f );
		Cvar_DirectSetValue( &vr_in_haptic, 102.0f );
	}

	if( !Q_strcmp( name, "Computer" ) && !kbd_comp_open )
	{
		kbd_comp_open = true;
	}
}

// every command the game's client sends (from cl_game.c)
void IN_VRClientCmd( const char *cmd )
{
	if( vr_menu_debug.value )
		Con_Printf( "VR game command: %s\n", cmd );

	// the options button of the game's own main menu opens ours
	if( !vr_menu_page && cls.state == ca_active && Q_stristr( cmd, "option" ) && Q_stristr( clgame.mapname, "c_game_menu" ))
		vr_menu_page = 2;

	if( !Q_strnicmp( cmd, "stopcomputer", 12 ))
		kbd_comp_open = false;
}

void IN_VRInit( void )
{
	int i;

	Cvar_RegisterVariable( &vr_in_active );
	Cvar_RegisterVariable( &vr_sound_debug );
	Cvar_RegisterVariable( &vr_in_kbd );
	Cvar_RegisterVariable( &vr_in_kbd_chars );
	Cvar_RegisterVariable( &vr_in_kbd_closed );
	Cvar_RegisterVariable( &vr_in_room_quiet );
	Cvar_RegisterVariable( &vr_quiet_steps );
	Cmd_AddCommand( "vr_menu", IN_VRMenu_f, "open or close the VR pause menu" );
	Cmd_AddCommand( "vr_keyboard", IN_VRKeyboard_f, "open or close the SteamVR keyboard for the text box you are typing in" );
	Cvar_RegisterVariable( &vr_in_room_x );
	Cvar_RegisterVariable( &vr_in_room_y );
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
	Cvar_RegisterVariable( &vr_in_ui_lhit );
	Cvar_RegisterVariable( &vr_in_ui_lkbd );
	Cvar_RegisterVariable( &vr_melee_instant );
	Cvar_RegisterVariable( &vr_swing_sound );
	Cvar_RegisterVariable( &vr_swing_sounds );
	Cvar_RegisterVariable( &vr_melee_hit_sounds );
	Cvar_RegisterVariable( &vr_melee_hold );
	Cvar_RegisterVariable( &vr_in_qs_pick );
	Cvar_RegisterVariable( &vr_in_qs_on );
	Cvar_RegisterVariable( &vr_in_qs_sel );
	Cvar_RegisterVariable( &vr_in_qs_x );
	Cvar_RegisterVariable( &vr_in_qs_y );
	Cvar_RegisterVariable( &vr_in_qs_z );
	Cvar_RegisterVariable( &vr_in_qs_yaw );
	Cvar_RegisterVariable( &vr_in_ui_lx );
	Cvar_RegisterVariable( &vr_in_ui_ly );
	Cvar_RegisterVariable( &vr_in_ui_x );
	Cvar_RegisterVariable( &vr_in_ui_y );
	Cvar_RegisterVariable( &vr_in_hand_speed );
	Cvar_RegisterVariable( &vr_in_swing_yaw );
	Cvar_RegisterVariable( &vr_in_swing_pitch );
	Cvar_RegisterVariable( &vr_in_melee );
	Cvar_RegisterVariable( &vr_melee_enable );
	Cvar_RegisterVariable( &vr_use_aim );
	Cvar_RegisterVariable( &vr_in_dir_yaw );
	Cvar_RegisterVariable( &vr_in_dir_pitch );
	Cvar_RegisterVariable( &vr_melee_speed );
	Cvar_RegisterVariable( &vr_melee_cooldown );
	Cvar_RegisterVariable( &vr_in_ray_x );
	Cvar_RegisterVariable( &vr_in_ray_y );
	Cvar_RegisterVariable( &vr_in_ray_z );
	Cvar_RegisterVariable( &vr_in_ray_dx );
	Cvar_RegisterVariable( &vr_in_ray_dy );
	Cvar_RegisterVariable( &vr_in_ray_dz );
	Cvar_RegisterVariable( &vr_in_whip );
	Cvar_RegisterVariable( &vr_in_lhand_speed );
	Cvar_RegisterVariable( &vr_in_lswing_yaw );
	Cvar_RegisterVariable( &vr_in_lswing_pitch );
	Cvar_RegisterVariable( &vr_in_swing_hit );
	Cvar_RegisterVariable( &vr_in_lswing_hit );
	Cvar_RegisterVariable( &vr_in_fists );
	Cvar_RegisterVariable( &vr_in_oneh );
	Cvar_RegisterVariable( &vr_in_lhand_yaw );
	Cvar_RegisterVariable( &vr_in_lhand_pitch );
	Cvar_RegisterVariable( &vr_in_dual );
	Cvar_RegisterVariable( &vr_in_dual_lmelee );
	Cvar_RegisterVariable( &vr_key_fist );
	Cvar_RegisterVariable( &vr_in_menu );
	Cvar_RegisterVariable( &vr_in_haptic );
	Cvar_RegisterVariable( &vr_in_climb );
	Cvar_RegisterVariable( &vr_in_haptic_len );
	Cvar_RegisterVariable( &vr_slot_debug );
	Cvar_RegisterVariable( &vr_in_hud_off );
	Cvar_RegisterVariable( &vr_shake );
	Cvar_RegisterVariable( &vr_in_crouch );
	Cvar_RegisterVariable( &vr_in_buttons );
	Cvar_RegisterVariable( &vr_in_mask );
	Cvar_RegisterVariable( &vr_in_hit_x );
	Cvar_RegisterVariable( &vr_in_hit_y );
	Cvar_RegisterVariable( &vr_in_hit_z );
	Cvar_RegisterVariable( &vr_in_hit_ok );
	Cvar_RegisterVariable( &vr_in_shot );
	Cvar_RegisterVariable( &vr_in_duck_drop );
	Cvar_RegisterVariable( &vr_bullet_holes );
	Cvar_RegisterVariable( &vr_in_noswing );
	Cvar_RegisterVariable( &vr_in_reload );
	Cvar_RegisterVariable( &vr_turn_mode );
	Cvar_RegisterVariable( &vr_move_ref );
	Cvar_RegisterVariable( &vr_menu_debug );
	Cvar_RegisterVariable( &vr_snap_angle );
	Cvar_RegisterVariable( &vr_smooth_speed );
	Cvar_RegisterVariable( &vr_aim_mode );
	Cvar_RegisterVariable( &vr_comp_keyboard );
	Cvar_RegisterVariable( &vr_move_cmd );
	Cvar_RegisterVariable( &vr_move_deadzone );
	Cvar_RegisterVariable( &vr_move_full );
	Cvar_RegisterVariable( &vr_move_digital );
	Cvar_RegisterVariable( &vr_move_digital_ents );
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
			const char *ks = vr_key_cvars[i]->string;
			int key;

			// old saved values: the right stick was empty or "!special" (not the right command), Y was "i"
			// dual wield: the left trigger fires the left weapon (the game's secondary attack, mouse 2). A melee weapon there
			// is only swung, never fired from the trigger
			if( i == VRB_LTRIGGER && vr_in_dual.value > 0.5f && cls.key_dest == key_game && vr_in_menu.value < 0.5f )
			{
				if( vr_in_dual_lmelee.value > 0.5f )
					continue;

				// seen in the game: while dual wielding mouse 1 fires the weapon the game draws on the LEFT and mouse 2 the one
				// on the right, so each trigger sends the key of the weapon in its own hand
				ks = "mouse1";
			}
			else if( i == VRB_RTRIGGER && vr_in_dual.value > 0.5f && cls.key_dest == key_game && vr_in_menu.value < 0.5f )
				ks = "mouse2";

			// swings (melee, fists, pistol whip) attack with the weapon of the hand that swung
			if(( i == VRB_SWING || i == VRB_WHIP || i == VRB_FIST ) && vr_in_dual.value > 0.5f && cls.key_dest == key_game )
				ks = ( host.realtime < vr_lswing_until && host.realtime >= vr_swing_until ) ? "mouse1" : "mouse2";

			if( i == VRB_RSTICK && ( !ks[0] || !Q_strcmp( ks, "!special" )))
				ks = "!weapontoggle";
			if( i == VRB_Y && ( !ks[0] || !Q_strcmp( ks, "i" ) || !Q_stricmp( ks, "escape" )))
			{
				// our own pause menu; the Xash menu stays on the keyboard's escape
				if( IN_VRMenuToggle( ))
					continue;

				ks = "escape";
			}

			// sprinting backwards is a dodge (the game's +dodge) instead
			if( i == VRB_LSTICK && vr_in_move_fwd.value < -0.5f && cls.state == ca_active && cls.key_dest == key_game )
			{
				vr_dodging = true;
				Cbuf_AddText( "+back\n+dodge\n" );
				continue;
			}

			key = IN_VRKeyFromName( ks );

			// "!command": run a game command instead of pressing a key, so it works whatever the keys are bound to
			if( ks[0] == '!' )
			{
				Cbuf_AddText( va( "%s\n", ks + 1 ));
				continue;
			}

			// the left hand's own cursor: its trigger clicks where it points (types a key of the computer's keyboard, or clicks)
			if( i == VRB_LTRIGGER && vr_in_menu.value > 0.5f && vr_in_ui_lhit.value > 0.5f )
			{
				int lx = (int)( vr_in_ui_lx.value * refState.width ), ly = (int)( vr_in_ui_ly.value * refState.height ), kch = 0;

				if( IN_VRKbdShown() && IN_VRKbdHit( lx, ly, &kch ))
				{
					if( kch ) IN_VRTypeChar( kch );
					continue;
				}

				// anywhere else the left hand does nothing: the right hand is the cursor of every screen
				continue;
			}

			// a melee weapon only attacks when it is swung, never from the trigger
			// (but never while a screen with a cursor is open: the inventory and the computer need the trigger to click)
			if( i == VRB_RTRIGGER && vr_in_melee.value > 0.5f && cls.key_dest == key_game && vr_in_menu.value < 0.5f )
				continue;

			// our pause menu: the trigger clicks its buttons, and the game never sees that click
			if( key == K_MOUSE1 && vr_menu_page )
			{
				IN_VRMenuClick( );
				continue;
			}

			// the computer's keyboard: a click on a key types it, and the game never sees that click
			if( key == K_MOUSE1 && IN_VRKbdShown())
			{
				int kch = 0;

				if( IN_VRKbdHit( vr_cursor_px, vr_cursor_py, &kch ))
				{
					if( kch ) IN_VRTypeChar( kch );
					continue;
				}
			}

			if( key )
			{
				vr_pressed_key[i] = key;
				IN_VRSendKey( key, true );
			}
		}
		else if( i == VRB_LTRIGGER && vr_lclick_down )
		{
			vr_lclick_down = false;
			IN_VRSendKey( K_MOUSE1, false );
		}
		else if( i == VRB_LSTICK && vr_dodging )
		{
			vr_dodging = false;
			Cbuf_AddText( "-dodge\n-back\n" );
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

	if( cls.key_dest != key_menu && !host.mouse_visible && !vr_menu_page )
		return;

	x = (int)( vr_in_ui_x.value * refState.width );
	y = (int)( vr_in_ui_y.value * refState.height );
	x = bound( 0, x, refState.width - 1 );
	y = bound( 0, y, refState.height - 1 );

	// where the laser points right now (the keyboard of the computer uses this, not the delayed click position)
	vr_cursor_px = x;
	vr_cursor_py = y;

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


// ---- our own pause menu (Y on the left controller) and the options, drawn like the keyboard of the computer and clicked
// with the right laser and trigger. The Xash menu stays on the keyboard's escape key.
#define VM_ID_BACK	99

typedef struct
{
	const char *label;
	const char *cvar;
	float min, max, step;
	int kind;		// 0 number, 1 choice, 2 on/off, 3 percent
	const char *names[3];
} vr_opt_t;

static const vr_opt_t vr_opts[] =
{
	{ "MOVEMENT DIRECTION", "vr_move_ref", 0, 1, 1, 1, { "HEAD", "LEFT HAND", NULL } },
	{ "TURNING", "vr_turn_mode", 0, 2, 1, 1, { "SNAP", "SMOOTH", "OFF" } },
	{ "SMOOTH TURN SPEED", "vr_smooth_speed", 30, 300, 15, 0, { NULL, NULL, NULL } },
	{ "SNAP TURN ANGLE", "vr_snap_angle", 15, 90, 15, 0, { NULL, NULL, NULL } },
	{ "SOUND VOLUME", "volume", 0, 1, 0.1f, 3, { NULL, NULL, NULL } },
	{ "MUSIC VOLUME", "MP3Volume", 0, 1, 0.1f, 3, { NULL, NULL, NULL } },
	{ "BRIGHTNESS", "brightness", 0, 1, 0.1f, 3, { NULL, NULL, NULL } },
};

static const char *vr_menu_saves[] = { "cofsave1", "cofsave2", "cofsave3", "cofsave4", "cofsave5", "autosave", "quick" };

typedef struct
{
	char text[48];
	char value[32];
	int id;
} vr_mitem_t;

static int vr_menu_hover = -1, vr_menu_confirm = -1, vr_menu_bgtex = -2, vr_menu_from;
static qboolean vr_menu_we_paused;

static void IN_VRMenuOpen( int page )
{
	if( !vr_menu_page && cls.state == ca_active && !cl.paused && !Q_stristr( clgame.mapname, "c_game_menu" ))
	{
		Cbuf_AddText( "pause\\n" );
		vr_menu_we_paused = true;
	}

	vr_menu_from = 0;
	vr_menu_page = page;
	vr_menu_confirm = -1;
	vr_menu_hover = -1;
}

static void IN_VRMenuClose( void )
{
	if( vr_menu_we_paused && cls.state == ca_active )
		Cbuf_AddText( "pause\\n" );

	vr_menu_we_paused = false;
	vr_menu_page = 0;
	vr_menu_confirm = -1;
	vr_menu_hover = -1;
}

static qboolean IN_VRMenuToggle( void )
{
	if( !Cvar_VariableInteger( "vr_enable" ) || cls.state != ca_active )
		return false;

	if( vr_menu_page )
	{
		IN_VRMenuClose( );
		return true;
	}

	// an inventory or the computer is open: Y still closes that one
	if( cls.key_dest != key_game || host.mouse_visible || Q_stristr( clgame.mapname, "c_game_menu" ))
		return false;

	IN_VRMenuOpen( 1 );
	return true;
}

static void IN_VRMenu_f( void )
{
	if( vr_menu_page )
		IN_VRMenuClose( );
	else if( cls.state == ca_active )
		IN_VRMenuOpen( 1 );
}

static void IN_VRMenuOptionValue( const vr_opt_t *o, char *out, size_t size )
{
	float v = Cvar_VariableValue( o->cvar );

	if( o->kind == 1 )
	{
		int n = bound( 0, (int)( v - o->min + 0.5f ), 2 );

		Q_strncpy( out, o->names[n] ? o->names[n] : "?", size );
	}
	else if( o->kind == 2 )
		Q_strncpy( out, v > 0.5f ? "ON" : "OFF", size );
	else if( o->kind == 3 )
		Q_snprintf( out, size, "%d%%", (int)( v * 100.0f + 0.5f ));
	else
		Q_snprintf( out, size, "%d", (int)( v + 0.5f ));
}

static int IN_VRMenuItems( vr_mitem_t *items )
{
	int n = 0, i;

	if( vr_menu_page == 1 )
	{
		static const char *names[] = { "RESUME", "LOAD GAME", "OPTIONS", "QUIT TO MAIN MENU", "QUIT TO DESKTOP" };

		for( i = 0; i < 5; i++ )
		{
			Q_strncpy( items[n].text, names[i], sizeof( items[n].text ));
			items[n].value[0] = 0;
			items[n].id = i + 1;

			// the two quit buttons ask once more
			if( vr_menu_confirm == i + 1 )
				Q_strncpy( items[n].text, "CLICK AGAIN TO CONFIRM", sizeof( items[n].text ));

			n++;
		}
	}
	else if( vr_menu_page == 2 )
	{
		for( i = 0; i < (int)ARRAYSIZE( vr_opts ); i++ )
		{
			Q_strncpy( items[n].text, vr_opts[i].label, sizeof( items[n].text ));
			IN_VRMenuOptionValue( &vr_opts[i], items[n].value, sizeof( items[n].value ));
			items[n].id = 100 + i;
			n++;
		}

		Q_strncpy( items[n].text, "BACK", sizeof( items[n].text ));
		items[n].value[0] = 0;
		items[n].id = VM_ID_BACK;
		n++;
	}
	else if( vr_menu_page == 3 )
	{
		for( i = 0; i < (int)ARRAYSIZE( vr_menu_saves ); i++ )
		{
			if( !FS_FileExists( va( "save/%s.sav", vr_menu_saves[i] ), true ))
				continue;

			if( i < 5 )
				Q_snprintf( items[n].text, sizeof( items[n].text ), "SAVE %d", i + 1 );
			else
				Q_strncpy( items[n].text, i == 5 ? "AUTOSAVE" : "QUICKSAVE", sizeof( items[n].text ));

			items[n].value[0] = 0;
			items[n].id = 200 + i;
			n++;
		}

		if( !n )
		{
			Q_strncpy( items[n].text, "NO SAVED GAMES", sizeof( items[n].text ));
			items[n].value[0] = 0;
			items[n].id = 0;
			n++;
		}

		Q_strncpy( items[n].text, "BACK", sizeof( items[n].text ));
		items[n].value[0] = 0;
		items[n].id = VM_ID_BACK;
		n++;
	}

	return n;
}

static void IN_VRMenuLayout( int n, int *x0, int *y0, int *rw, int *rh, int *gap )
{
	int w = refState.width, h = refState.height;

	*rw = w * 62 / 100;
	*rh = Q_min( h * 8 / 100, (int)( h * 0.70f / Q_max( n, 1 ) * 0.84f ));
	*gap = Q_max( *rh / 5, 2 );
	*x0 = ( w - *rw ) / 2;
	*y0 = h * 25 / 100;
}

// the - and + boxes at the right end of an option row: x of the minus box, of the plus box and their size
static void IN_VRMenuButtons( int x0, int y, int rw, int rh, int *minus_x, int *plus_x, int *bs, int *by )
{
	*bs = rh * 80 / 100;
	*by = y + ( rh - *bs ) / 2;
	*plus_x = x0 + rw - rw / 40 - *bs;
	*minus_x = *plus_x - *bs - rw * 22 / 100;
}

static int IN_VRMenuHit( int px, int py, const vr_mitem_t *items, int n, int *part )
{
	int x0, y0, rw, rh, gap, i;

	IN_VRMenuLayout( n, &x0, &y0, &rw, &rh, &gap );

	for( i = 0; i < n; i++ )
	{
		int y = y0 + i * ( rh + gap );

		if( px >= x0 && px < x0 + rw && py >= y && py < y + rh )
		{
			int mx, pxx, bs, by;

			// an option row: only its - and + boxes act (0 = minus, 100 = plus, -1 = elsewhere on the row)
			IN_VRMenuButtons( x0, y, rw, rh, &mx, &pxx, &bs, &by );
			*part = -1;

			if( items[i].value[0] )
			{
				if( px >= mx - bs / 4 && px < mx + bs + bs / 4 )
					*part = 0;
				else if( px >= pxx - bs / 4 && px < pxx + bs + bs / 4 )
					*part = 100;
			}

			return i;
		}
	}

	return -1;
}

static void IN_VRMenuCursorPos( int *px, int *py )
{
	// the position the trigger was aimed at (not where the pull moved the hand to)
	*px = vr_cur_locked ? vr_cur_lock_x : vr_cursor_px;
	*py = vr_cur_locked ? vr_cur_lock_y : vr_cursor_py;
}

static void IN_VRMenuChangeOption( const vr_opt_t *o, int part )
{
	float v = Cvar_VariableValue( o->cvar );

	if( o->kind == 1 )
	{
		int count = o->names[2] ? 3 : 2;
		int cur = bound( 0, (int)( v - o->min + 0.5f ), count - 1 );

		cur = ( part < 50 ) ? ( cur + count - 1 ) % count : ( cur + 1 ) % count;
		v = o->min + cur;
	}
	else if( o->kind == 2 )
		v = ( v > 0.5f ) ? 0.0f : 1.0f;
	else
		v = bound( o->min, v + (( part < 50 ) ? -o->step : o->step ), o->max );

	Cvar_SetValue( o->cvar, v );
}

static void IN_VRMenuClick( void )
{
	vr_mitem_t items[16];
	int n = IN_VRMenuItems( items ), px, py, part = 0, hit, id;

	IN_VRMenuCursorPos( &px, &py );
	hit = IN_VRMenuHit( px, py, items, n, &part );

	if( hit < 0 )
	{
		vr_menu_confirm = -1;
		return;
	}

	id = items[hit].id;

	if( !id )
		return;

	if( id < 100 || id >= 200 )
		S_StartLocalSound( "UI/3d_click.wav", VOL_NORM, false );

	if( id >= 100 && id < 200 )
	{
		if( part >= 0 )
		{
			S_StartLocalSound( "UI/3d_click.wav", VOL_NORM, false );
			IN_VRMenuChangeOption( &vr_opts[id - 100], part );
		}

		return;
	}

	if( id >= 200 )
	{
		Cbuf_AddText( va( "load %s\\n", vr_menu_saves[id - 200] ));
		IN_VRMenuClose( );
		return;
	}

	if(( id == 4 || id == 5 ) && vr_menu_confirm != id )
	{
		vr_menu_confirm = id;
		return;
	}

	vr_menu_confirm = -1;

	switch( id )
	{
	case 1:
		IN_VRMenuClose( );
		break;
	case 2:
		vr_menu_page = 3;
		break;
	case 3:
		vr_menu_page = 2;
		break;
	case 4:
		IN_VRMenuClose( );
		Cbuf_AddText( "map c_game_menu1\\n" );
		break;
	case 5:
		Cbuf_AddText( "quit\\n" );
		break;
	case VM_ID_BACK:
		// the options of the game's own main menu go straight away, the rest goes back to the pause menu
		if( Q_stristr( clgame.mapname, "c_game_menu" ))
			IN_VRMenuClose( );
		else vr_menu_page = 1;
		break;
	}

	vr_menu_hover = -1;
}

static void IN_VRMenuDraw( void )
{
	vr_mitem_t items[16];
	int w = refState.width, h = refState.height, n, i, k, x0, y0, rw, rh, gap, px, py, part = 0, hit = -1;
	static cl_font_t font;
	rgba_t white = { 235, 235, 235, 255 }, orange = { 255, 190, 60, 255 };
	const char *title;
	cl_font_t *f;
	int lw, lh;

	if( !vr_menu_page )
		return;

	// a map change closes the pause menu (the options of the game's own main menu stay)
	if( cls.state != ca_active )
	{
		IN_VRMenuClose( );
		return;
	}

	if( !font.valid && cls.creditsFont.rendermode )
	{
		// gfx/vr/menufont.fnt (made by tools/make_menu_font.py from any TrueType font), else the game's credits font
		if( !Con_LoadVariableWidthFont( "gfx/vr/menufont.fnt", &font, 1.35f, cls.creditsFont.rendermode, TF_FONT ))
		{
			if( !Con_LoadVariableWidthFont( "gfx/creditsfont.fnt", &font, 2.0f, cls.creditsFont.rendermode, TF_FONT ))
				Con_LoadFixedWidthFont( "gfx/conchars", &font, 3.0f, cls.creditsFont.rendermode, TF_FONT | TF_NEAREST | TF_KEEP_SOURCE );
		}
	}

	f = font.valid ? &font : &cls.creditsFont;

	n = IN_VRMenuItems( items );
	IN_VRMenuLayout( n, &x0, &y0, &rw, &rh, &gap );

	if( vr_in_ui_hit.value > 0.5f )
	{
		IN_VRMenuCursorPos( &px, &py );
		hit = IN_VRMenuHit( vr_cursor_px, vr_cursor_py, items, n, &part );
		(void)px; (void)py;
	}

	if( hit >= 0 && !items[hit].id )
		hit = -1;

	if( hit != vr_menu_hover )
	{
		if( hit >= 0 )
			S_StartLocalSound( "UI/3d_rollover.wav", VOL_NORM, false );

		vr_menu_hover = hit;
	}

	ref.dllFuncs.R_Set2DMode( true );

	// the picture of the menu (gfx/vr/menu_bg.tga), a plain dark panel if it is not there
	if( vr_menu_bgtex == -2 )
		vr_menu_bgtex = FS_FileExists( "gfx/vr/menu_bg.tga", false ) ? ref.dllFuncs.GL_LoadTexture( "gfx/vr/menu_bg.tga", NULL, 0, TF_IMAGE ) : 0;

	if( vr_menu_bgtex > 0 )
	{
		ref.dllFuncs.GL_SetRenderMode( kRenderNormal );
		ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
		ref.dllFuncs.R_DrawStretchPic( 0, 0, w, h, 0, 0, 1, 1, vr_menu_bgtex );
	}
	else ref.dllFuncs.FillRGBA( kRenderTransTexture, 0, 0, w, h, 8, 8, 8, 255 );

	ref.dllFuncs.FillRGBA( kRenderTransTexture, 0, 0, w, h, 0, 0, 0, 110 );

	title = ( vr_menu_page == 2 ) ? "OPTIONS" : ( vr_menu_page == 3 ? "LOAD GAME" : "PAUSED" );
	CL_DrawStringLen( f, title, &lw, &lh, 0 );
	CL_DrawString(( w - lw ) / 2, h * 13 / 100, title, white, f, 0 );

	for( i = 0; i < n; i++ )
	{
		int y = y0 + i * ( rh + gap ), over = ( i == hit ), tw = 0, th = 0, vw = 0, vh = 0;
		int pad = rw / 25;
		byte *col = over ? orange : white;
		char value[48];

		ref.dllFuncs.FillRGBA( kRenderTransTexture, x0, y, rw, rh, over ? 90 : 15, over ? 60 : 15, over ? 15 : 15, over ? 225 : 190 );

		CL_DrawStringLen( f, items[i].text, &tw, &th, 0 );
		CL_DrawString( x0 + pad, y + ( rh - th ) / 2, items[i].text, col, f, 0 );

		if( items[i].value[0] )
		{
			int mx, pxx, bs, by, bar;

			IN_VRMenuButtons( x0, y, rw, rh, &mx, &pxx, &bs, &by );
			bar = Q_max( bs / 8, 3 );

			// the value, centred between the two boxes
			CL_DrawStringLen( f, items[i].value, &vw, &vh, 0 );
			CL_DrawString( mx + bs + (( pxx - mx - bs ) - vw ) / 2, y + ( rh - vh ) / 2, items[i].value, col, f, 0 );

			// - and + : boxes with a bar (and a second one for the plus) so they are big and easy to hit
			for( k = 0; k < 2; k++ )
			{
				int bx = ( k == 0 ) ? mx : pxx;
				int lit = over && (( k == 0 && part == 0 ) || ( k == 1 && part == 100 ));

				ref.dllFuncs.FillRGBA( kRenderTransTexture, bx, by, bs, bs, lit ? 150 : 60, lit ? 100 : 60, lit ? 25 : 60, 235 );
				ref.dllFuncs.FillRGBA( kRenderTransTexture, bx + bs / 5, by + bs / 2 - bar / 2, bs * 3 / 5, bar, 245, 245, 245, 255 );

				if( k == 1 )
					ref.dllFuncs.FillRGBA( kRenderTransTexture, bx + bs / 2 - bar / 2, by + bs / 5, bar, bs * 3 / 5, 245, 245, 245, 255 );
			}
		}
	}

	// where the right hand points (the panel's own dot is drawn by the renderer)
	(void)part;
}

// every rendered frame, also when no game is running (the Xash menu over the main menu map, or no map at all): says that a
// menu wants the cursor and moves it where the right hand points
void IN_VRMenuFrame( void )
{
	float menu = ( cls.key_dest != key_game || host.mouse_visible || vr_menu_page ) ? 1.0f : 0.0f;

	if( vr_in_menu.value != menu )
		Cvar_DirectSetValue( &vr_in_menu, menu );

	IN_VRMoveCursor();
}

void IN_VRAppendMove( float frametime, usercmd_t *cmd, qboolean active )
{
	float yaw = 0.0f;
	float x, y, fwd, side;
	float head_rel, aim_rel, aim_pitch, rot;
	int buttons, mode;
	qboolean in_game, on_ladder, on_boat, digital, stuck_forced;

	{
		// the game's own inventory / save screens keep key_game but show a mouse cursor
		float menu = ( cls.key_dest != key_game || host.mouse_visible || vr_menu_page ) ? 1.0f : 0.0f;

		if( vr_in_menu.value != menu )
			Cvar_DirectSetValue( &vr_in_menu, menu );

		// inventory, computer, letters and cutscenes (the camera is not the player) show the HUD normally
		{
			float off = ( menu > 0.5f || ( cls.state == ca_active && cl.viewentity != cl.playernum + 1 )) ? 1.0f : 0.0f;

			if( vr_in_hud_off.value != off )
				Cvar_DirectSetValue( &vr_in_hud_off, off );
		}
	}

	{
		// the SteamVR keyboard for text boxes in the game (the computer): open while such a screen is open
		qboolean ui_open = cls.state == ca_active && cls.key_dest == key_game && host.mouse_visible;
		qboolean want;

		if( !ui_open && cls.key_dest == key_game )
			kbd_manual = false;

		{
			// a new map: nothing of the old one stays open
			static char kbd_map[64];

			if( Q_stricmp( kbd_map, clgame.mapname ))
			{
				Q_strncpy( kbd_map, clgame.mapname, sizeof( kbd_map ));
				kbd_comp_open = false;
				kbd_cmd_open = false;
			}
		}

		if( vr_in_kbd_closed.value )
		{
			kbd_dismissed = true;
			kbd_manual = false;
			kbd_cmd_open = false;
			Cvar_DirectSetValue( &vr_in_kbd_closed, 0.0f );
		}

		if( !ui_open && !kbd_manual )
			kbd_dismissed = false;

		want = !kbd_dismissed && ( kbd_manual || kbd_cmd_open );

		if(( vr_in_kbd.value >= 0.5f ) != want )
			Cvar_DirectSetValue( &vr_in_kbd, want ? 1.0f : 0.0f );

		if( vr_in_kbd_chars.string[0] )
		{
			const char *c = vr_in_kbd_chars.string;
			char copy[256];

			Q_strncpy( copy, c, sizeof( copy ));
			Cvar_DirectSet( &vr_in_kbd_chars, "" );

			for( c = copy; *c; c++ )
				IN_VRTypeChar(( unsigned char )*c );
		}
	}

	if( vr_in_active.value > 0.0f && cls.state == ca_active )
	{
		// things that make a VR picture uncomfortable are switched off on every map
		static char forced_map[64];

		if( Q_strcmp( forced_map, clgame.mapname ))
		{
			convar_t *cv;

			Q_strncpy( forced_map, clgame.mapname, sizeof( forced_map ));

			if(( cv = Cvar_FindVar( "cl_noiseeffect" )) != NULL ) Cvar_DirectSetValue( cv, 0.0f );
			if(( cv = Cvar_FindVar( "gl_screenblur" )) != NULL ) Cvar_DirectSetValue( cv, 0.0f );
		}
	}

	if( vr_in_active.value <= 0.0f )
	{
		IN_VRReleaseAll();
		return;
	}

	// melee and swings must not turn you around: hold the body direction for a moment
	if( host.realtime < vr_lock_until )
		cl.viewangles[YAW] = vr_lock_yaw;

	{
		float whip = host.realtime < vr_whip_until ? 1.0f : 0.0f;

		if( vr_in_whip.value != whip )
			Cvar_DirectSetValue( &vr_in_whip, whip );
	}

	IN_VRMoveCursor();

	in_game = active && cls.key_dest == key_game && !cl.paused && !cl.intermission;
	buttons = (int)vr_in_buttons.value;

	// the reload system keeps some buttons away from the game while it is using them, and presses reload when it is done
	buttons &= ~(int)vr_in_mask.value;

	if( in_game && vr_in_reload.value > 0.5f )
		buttons |= BIT( VRB_RELOAD );

	// where does the game aim? 0 = body (mouse), 1 = head, 2 = hand while acting, 3 = hand always
	head_rel = vr_in_head_yaw.value;
	mode = (int)vr_aim_mode.value;

	// using something or shooting: aim with the hand for a moment (walking keeps following the head otherwise)
	if( buttons & ( BIT( VRB_RTRIGGER ) | BIT( VRB_B )))
		vr_hand_aim_until = host.realtime + 0.35;

	// swinging by hand: a fast hand movement attacks at once, aimed where the tip of the weapon is going.
	// Right hand: the weapon in it (melee weapon, a gun for a pistol whip, fists). Left hand: punches, when the right hand holds
	// a one handed item or nothing, or the left weapon of a dual wield
	{
		qboolean right_ok = vr_in_melee.value > 0.5f || ( !COM_StringEmptyOrNULL( vr_key_whip.string ) && vr_in_dual.value < 0.5f );
		// dual wield: only a left MELEE weapon swings. (A left gun with a phone or flare on the right used to count the phone as
		// a free hand, so swinging the left hand sent the attack key of the left gun and it fired)
		qboolean left_ok = ( vr_in_dual.value > 0.5f ) ? ( vr_in_dual_lmelee.value > 0.5f ) : ( vr_in_fists.value > 0.5f || vr_in_oneh.value > 0.5f );
		float thr = vr_melee_speed.value;

		if( vr_in_hand_speed.value < thr * 0.45f )
			vr_swing_armed = true;
		if( vr_in_lhand_speed.value < thr * 0.45f )
			vr_lswing_armed = true;

		if( in_game && vr_melee_enable.value && vr_in_noswing.value < 0.5f && vr_in_hand_valid.value > 0.5f && vr_swing_armed
			&& right_ok && vr_in_hand_speed.value > thr && host.realtime >= vr_melee_next )
		{
			vr_melee_next = host.realtime + vr_melee_cooldown.value;
			vr_swing_until = host.realtime + 0.3;
			vr_swing_yaw = vr_in_swing_yaw.value;
			vr_swing_pitch = vr_in_swing_pitch.value;
			vr_swing_frames = 3;
			vr_swing_whip = vr_in_melee.value < 0.5f;
			vr_swing_fist = vr_in_fists.value > 0.5f;
			vr_swing_armed = false;
			vr_lock_yaw = cl.viewangles[YAW];
			vr_lock_until = host.realtime + 0.8;

			vr_swing_hold = host.realtime + vr_melee_hold.value;
			vr_melee_window_until = host.realtime + 0.45;
			S_VRSwingStart( vr_cur_weapon, 1 );

			// a gun being whipped holds still on screen for a moment
			if( vr_swing_whip && !vr_swing_fist )
				vr_whip_until = host.realtime + 0.7;
		}

		if( in_game && vr_melee_enable.value && vr_in_noswing.value < 0.5f && left_ok && vr_lswing_armed
			&& vr_in_lhand_speed.value > thr && host.realtime >= vr_lmelee_next )
		{
			vr_lmelee_next = host.realtime + vr_melee_cooldown.value;
			vr_lswing_until = host.realtime + 0.3;
			vr_lswing_yaw = vr_in_lswing_yaw.value;
			vr_lswing_pitch = vr_in_lswing_pitch.value;
			vr_lswing_frames = 3;
			vr_lswing_armed = false;
			vr_lock_yaw = cl.viewangles[YAW];
			vr_lock_until = host.realtime + 0.8;

			vr_lswing_hold = host.realtime + vr_melee_hold.value;
			vr_melee_window_until = host.realtime + 0.45;
			S_VRSwingStart( vr_cur_weapon, 0 );
		}
	}

	if( in_game && vr_in_melee.value > 0.5f && FBitSet( buttons & ~vr_prev_buttons, BIT( VRB_RTRIGGER )))
	{
		vr_lock_yaw = cl.viewangles[YAW];
		vr_lock_until = host.realtime + 0.8;
	}

	if( buttons & BIT( VRB_RGRIP ))
		vr_use_until = host.realtime + 0.35;

	if( !in_game )
		vr_swing_frames = 0;

	if( vr_swing_frames > 0 || host.realtime < vr_swing_hold )
	{
		buttons |= BIT( vr_swing_fist ? VRB_FIST : ( vr_swing_whip ? VRB_WHIP : VRB_SWING ));
		if( vr_swing_frames > 0 ) vr_swing_frames--;
	}

	if( vr_lswing_frames > 0 || host.realtime < vr_lswing_hold )
	{
		buttons |= BIT( VRB_FIST );
		if( vr_lswing_frames > 0 ) vr_lswing_frames--;
	}

	if( vr_in_dual.value > 0.5f && vr_in_dual_lmelee.value < 0.5f && FBitSet( buttons, BIT( VRB_LTRIGGER )) && !FBitSet( buttons, BIT( VRB_RTRIGGER )))
	{
		// the left weapon of a dual wield shoots where the left hand points
		mode = 2;
		aim_rel = vr_in_lhand_yaw.value;
		aim_pitch = vr_in_lhand_pitch.value;
	}
	else if( host.realtime < vr_lswing_until && host.realtime >= vr_swing_until )
	{
		mode = 2;
		aim_rel = vr_in_lswing_yaw.value;
		aim_pitch = vr_in_lswing_pitch.value;
	}
	else if( host.realtime < vr_swing_until )
	{
		// the aim follows the swing while it goes on: it ends up on what the tip is moving at
		mode = 2;
		aim_rel = vr_in_swing_yaw.value;
		aim_pitch = vr_in_swing_pitch.value;
	}
	else if( vr_in_hand_valid.value > 0.5f && vr_use_aim.value >= 1.0f && host.realtime < vr_use_until && host.realtime >= vr_hand_aim_until )
	{
		// the use key (doors, items, the save cassette) aims where the hand points
		mode = 2;

		if( vr_use_aim.value >= 2.0f )
		{
			aim_rel = vr_in_dir_yaw.value;
			aim_pitch = vr_in_dir_pitch.value;
		}
		else
		{
			aim_rel = vr_in_hand_yaw.value;
			aim_pitch = vr_in_hand_pitch.value;
		}
	}
	else if( vr_in_hand_valid.value > 0.5f && ( mode >= 3 || host.realtime < vr_hand_aim_until ))
	{
		// shooting always aims with the hand, whatever vr_aim_mode says (0 = body used to send every shot straight ahead)
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

	// a bullet hole where the hand ray hits the world, so you can see where you are aiming
	// (only when the weapon really fired: the renderer counts the shooting animations)
	{
		static int last_shot;
		int shot = (int)vr_in_shot.value;

		if( shot != last_shot )
		{
			last_shot = shot;

			if( in_game && vr_bullet_holes.value && vr_in_hit_ok.value > 0.5f && vr_in_melee.value < 0.5f )
			{
				char name[16];
				vec3_t pos = { vr_in_hit_x.value, vr_in_hit_y.value, vr_in_hit_z.value };
				int id;

				Q_snprintf( name, sizeof( name ), "{shot%d", COM_RandomLong( 1, 5 ));
				id = CL_DecalIndexFromName( name );

				if( id )
					CL_DecalShoot( CL_DecalIndex( id ), 0, 0, pos, 0 );
			}
		}
	}

	if( Cvar_VariableValue( "vr_aim_debug" ) > 0.0f && (( buttons & ~vr_prev_buttons ) & ( BIT( VRB_RTRIGGER ) | BIT( VRB_RGRIP ))))
	{
		Con_Printf( "VR trigger/grip pressed: sending yaw %.1f (body %.1f) pitch %.1f, aim from %s (hand valid %d, hand yaw %.1f pitch %.1f)\n",
			cl.viewangles[YAW] + aim_rel, cl.viewangles[YAW], aim_pitch, mode == 2 ? "hand" : ( mode == 1 ? "head" : "body" ),
			(int)vr_in_hand_valid.value, vr_in_hand_yaw.value, vr_in_hand_pitch.value );
	}

	// the stick means "walk where I look". the game walks along its aim, so rotate the stick by the difference
	rot = (( vr_move_ref.value >= 0.5f ) ? vr_in_lhand_yaw.value : head_rel ) - aim_rel;
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

	// boats, trains and lifts also need real key presses. ordinary floors can be entities too, so only count:
	// a vehicle you are controlling, something under your feet that is really moving, or a name you listed
	on_boat = false;

	if( clgame.pmove )
	{
		static int plat_ent = -1;
		static vec3_t plat_org;
		static double plat_time, plat_moving_until, dbg_next;
		static float plat_speed;
		int g = clgame.pmove->onground;
		const char *gname = "-";

		if( FBitSet( clgame.pmove->flags, FL_ONTRAIN ))
			on_boat = true;

		if( g > 0 && g < clgame.pmove->numphysent && !clgame.pmove->physents[g].player )
		{
			physent_t *pe = &clgame.pmove->physents[g];

			gname = pe->name;

			if( vr_move_digital_ents.string[0] && Q_stristr( vr_move_digital_ents.string, pe->name ))
				on_boat = true;

			if( g != plat_ent )
			{
				plat_ent = g;
				VectorCopy( pe->origin, plat_org );
				plat_time = host.realtime;
				plat_speed = 0.0f;
			}
			else if( host.realtime - plat_time > 0.25 )
			{
				vec3_t d;

				VectorSubtract( pe->origin, plat_org, d );
				plat_speed = VectorLength( d ) / (float)( host.realtime - plat_time );
				VectorCopy( pe->origin, plat_org );
				plat_time = host.realtime;

				if( plat_speed > 15.0f )
					plat_moving_until = host.realtime + 1.5;
			}
		}
		else
		{
			plat_ent = -1;
			plat_speed = 0.0f;
		}

		if( host.realtime < plat_moving_until )
			on_boat = true;

		if( vr_debug_move.value && host.realtime > dbg_next )
		{
			dbg_next = host.realtime + 1.0;
			Con_Printf( "VR move: movetype %d, flags 0x%x, ground entity %d (%s), it moves at %.0f units/s -> %s%s\n",
				clgame.pmove->movetype, clgame.pmove->flags, g, gname, plat_speed,
				on_boat ? "boat/train (keys)" : "normal walking", on_ladder ? ", ladder" : "" );
		}
	}

	// anything we did not recognise (a ladder or boat of the game's own, a lift): you push the stick and nothing moves,
	// so for a while the movement keys are pressed instead
	stuck_forced = false;

	if( clgame.pmove && in_game )
	{
		static double stuck_since, force_until;
		float push = sqrt( fwd * fwd + side * side );
		float speed = VectorLength( clgame.pmove->velocity );

		if( push > 0.6f && speed < 15.0f && !on_ladder && !on_boat )
		{
			if( stuck_since == 0.0 )
				stuck_since = host.realtime;
			else if( host.realtime - stuck_since > 0.45 )
				force_until = host.realtime + 2.0;
		}
		else
		{
			stuck_since = 0.0;
		}

		if( host.realtime < force_until )
		{
			on_boat = true;
			stuck_forced = true;
		}
	}

	{
		float climb = ( on_ladder || ( on_boat && !stuck_forced )) ? 1.0f : 0.0f;

		if( vr_in_climb.value != climb )
			Cvar_DirectSetValue( &vr_in_climb, climb );
	}

	// vr_move_digital 0 is the normal analog walking, but ladders and boats need the keys: switch for as long as you are on one
	digital = ( vr_move_digital.value || on_ladder || on_boat ) && ( on_ladder || on_boat || fabs( rot ) < 15.0f );

	// how much the game has lowered the view for its own crouch (the renderer gives it back, so only your real head counts)
	if( clgame.pmove )
	{
		static float stand_height;
		float h = clgame.pmove->view_ofs[2] - clgame.pmove->player_mins[clgame.pmove->usehull][2];
		qboolean ducking = FBitSet( clgame.pmove->flags, FL_DUCKING ) || clgame.pmove->bInDuck;

		// the standing height is only learned while the character is not ducking at all
		if( !ducking )
			stand_height = h;

		Cvar_DirectSetValue( &vr_in_duck_drop, ducking ? bound( 0.0f, stand_height - h, 60.0f ) : 0.0f );
	}

	// right stick: up and hold = the quick slot selector (move your hand to slot 1, 2 or 3 and let go), down = toggle crouch.
	// Jumping is the A button: a tap jumps, holding it also crouches, for jumps that need a crouch in the air
	y = vr_in_stick_y.value;

	if( in_game && y > 0.7f && !vr_up_latched )
	{
		vec3_t o, d;

		vr_up_latched = true;
		vr_qs_active = IN_VRGetRay( o, d );

		if( vr_qs_active )
		{
			VectorCopy( o, vr_qs_start );
			vr_qs_yaw = ( d[0] * d[0] + d[1] * d[1] > 0.09f ) ? atan2( d[1], d[0] ) : DEG2RAD( cl.viewangles[YAW] );
		}
	}
	else if( y < 0.3f )
	{
		if( vr_up_latched && vr_qs_active && vr_qs_sel > 0 )
		{
			int key = '0' + vr_qs_sel;

			IN_VRSendKey( key, true );
			IN_VRSendKey( key, false );
		}

		vr_up_latched = false;
		vr_qs_active = false;
		vr_qs_sel = 0;
	}

	if( vr_qs_active )
	{
		vec3_t o, d, right;
		float yaw = vr_qs_yaw, side, up;

		vr_qs_sel = 0;

		if( IN_VRGetRay( o, d ))
		{
			right[0] = sin( yaw );
			right[1] = -cos( yaw );
			right[2] = 0.0f;
			side = ( o[0] - vr_qs_start[0] ) * right[0] + ( o[1] - vr_qs_start[1] ) * right[1];
			up = o[2] - vr_qs_start[2];

			if( side < -4.0f ) vr_qs_sel = 1;
			else if( side > 4.0f ) vr_qs_sel = 3;
			else if( up > 5.5f ) vr_qs_sel = 2;
		}
	}

	// the renderer draws the three slots in the world (attached to the head) and says which one the hand is over
	if( vr_qs_active )
		vr_qs_sel = Cvar_VariableInteger( "vr_in_qs_pick" );

	// the renderer draws the three slots in the world, above the hand, where the stick was pushed forward
	Cvar_DirectSetValue( &vr_in_qs_on, vr_qs_active ? 1.0f : 0.0f );
	Cvar_DirectSetValue( &vr_in_qs_sel, vr_qs_active ? (float)vr_qs_sel : 0.0f );

	if( vr_qs_active )
	{
		Cvar_DirectSetValue( &vr_in_qs_x, vr_qs_start[0] );
		Cvar_DirectSetValue( &vr_in_qs_y, vr_qs_start[1] );
		Cvar_DirectSetValue( &vr_in_qs_z, vr_qs_start[2] );
		Cvar_DirectSetValue( &vr_in_qs_yaw, RAD2DEG( vr_qs_yaw ));
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

	// hold A: a crouch jump
	if( FBitSet( buttons, BIT( VRB_A )))
	{
		if( vr_a_since == 0.0 )
			vr_a_since = host.realtime;
		else if( host.realtime - vr_a_since > 0.18 )
			buttons |= BIT( VRB_CROUCH );
	}
	else
	{
		vr_a_since = 0.0;
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

	// the quick slot selector is open (right stick held up): the stick does not turn you, whatever it does sideways
	if( vr_qs_active && vr_up_latched )
		x = 0.0f;

	if( vr_turn_mode.value >= 2.0f )
	{
		// no turning at all
	}
	else if( vr_turn_mode.value )
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
		vr_lock_yaw = cl.viewangles[YAW];
	}

	// analog walking, when the keys are not doing it
	if( !digital )
	{
		// 100% stick is 100% speed, and a gentle push is a gentle walk: a small dead zone, then a straight line up to
		// vr_move_full (the stick rarely reaches 1.0)
		{
			float m = sqrt( fwd * fwd + side * side );
			float dz = vr_move_deadzone.value, full = Q_max( vr_move_full.value, dz + 0.1f );

			if( m < dz )
			{
				fwd = side = 0.0f;
			}
			else
			{
				float k = bound( 0.0f, ( m - dz ) / ( full - dz ), 1.0f ) / m;

				fwd *= k;
				side *= k;
			}
		}

		// the game limits the speed itself (its own maximum), the keys send more than cl_forwardspeed too:
		// send enough that a pushed stick always reaches that maximum, like the keyboard does
		{
			// A full push sends a huge command, so the game's own maximum is reached (what the keys give). The speed that
			// shows up then is remembered, separately for forward/back and for sideways, and a smaller push is that fraction
			// of it: a gentle push is a gentle walk, a full push is a full walk
			// one speed for every direction: the stick's direction is kept, only its length is scaled. (Before, forward and
			// sideways had their own numbers and the diagonals fell between them: forward-left was slow)
			static float top = 250.0f;
			float speed = clgame.pmove ? sqrt( clgame.pmove->velocity[0] * clgame.pmove->velocity[0] + clgame.pmove->velocity[1] * clgame.pmove->velocity[1] ) : 0.0f;
			qboolean sprint = FBitSet( buttons, BIT( VRB_LSTICK ));
			float len = sqrt( fwd * fwd + side * side );

			if( len > 0.001f )
			{
				// at a full push the speed that comes out is the game's own maximum: remember it, any direction counts
				if( clgame.pmove && clgame.pmove->onground != -1 && !sprint && speed > 40.0f && len > 0.97f )
					top = Q_max( top * 0.9995f, speed );

				{
					float cmdlen = ( len > 0.97f ) ? vr_move_cmd.value : len * top;

					cmd->forwardmove += fwd / len * cmdlen;
					cmd->sidemove += side / len * cmdlen;
				}
			}
		}

		if( fwd > 0.3f ) SetBits( cmd->buttons, IN_FORWARD );
		if( fwd < -0.3f ) SetBits( cmd->buttons, IN_BACK );
		if( side > 0.3f ) SetBits( cmd->buttons, IN_MOVERIGHT );
		if( side < -0.3f ) SetBits( cmd->buttons, IN_MOVELEFT );
	}

	// roomscale: walk to where you really stand. the renderer measures how far that is, this turns it into walking
	{
		float ox = vr_in_room_x.value, oy = vr_in_room_y.value;

		if(( ox != 0.0f || oy != 0.0f ) && !on_ladder && Cvar_VariableValue( "vr_in_menu" ) < 0.5f )
		{
			vec3_t ang = { 0.0f, mode > 0 ? cl.viewangles[YAW] + aim_rel : cmd->viewangles[YAW], 0.0f }, f, r;
			float vf, vs, len;

			AngleVectors( ang, f, r, NULL );
			vf = ( ox * f[0] + oy * f[1] ) * 10.0f;
			vs = ( ox * r[0] + oy * r[1] ) * 10.0f;
			len = sqrt( vf * vf + vs * vs );

			if( len > 250.0f )
			{
				vf *= 250.0f / len;
				vs *= 250.0f / len;
			}

			cmd->forwardmove += vf;
			cmd->sidemove += vs;
			room_until = host.realtime + 0.6f;
		}

		Cvar_SetValue( "vr_in_room_x", 0.0f );
		Cvar_SetValue( "vr_in_room_y", 0.0f );

		// footsteps from walking around your room are muted (s_main.c): a step of a millimeter is not a walk
		{
			float quiet = ( host.realtime < room_until && fabs( vr_in_move_fwd.value ) < 0.25f && fabs( vr_in_move_side.value ) < 0.25f ) ? 1.0f : 0.0f;

			if( vr_in_room_quiet.value != quiet )
				Cvar_DirectSetValue( &vr_in_room_quiet, quiet );
		}
	}

	// what the game aims, uses and shoots along. cl.viewangles stays the body direction, so the
	// headset view does not feed back into itself.
	if( mode > 0 )
	{
		cmd->viewangles[YAW] = cl.viewangles[YAW] + aim_rel;
		cmd->viewangles[PITCH] = bound( -89.0f, aim_pitch, 89.0f );
	}
}
