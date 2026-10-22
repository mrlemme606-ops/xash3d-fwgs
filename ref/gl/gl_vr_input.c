/*
gl_vr_input.c - SteamVR controller polling (legacy controller state)
Cry of Fear VR project, milestone 2: walk, turn, press buttons.
Copyright (C) 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

The controller state is published into engine cvars (vr_in_*), which the engine reads in
engine/client/input/in_vr.c. The renderer DLL cannot call the engine's input code directly.
*/
#include "gl_vr.h"
#include "xash3d_mathlib.h"

#define OPENVR_API_NODLL
#include <stdint.h>
#include <stdbool.h>
#include "openvr_capi.h"

#define VR_DEVICE_INVALID	0xFFFFFFFFu
#define VR_ROLE_LEFT		1
#define VR_ROLE_RIGHT		2
#define VR_STICK_DEADZONE	0.2f

// button ids as SteamVR reports them for Touch controllers in legacy mode
#define VR_BTN( id )		(1ULL << ( id ))
#define VR_BTN_APPMENU		1	// B / Y
#define VR_BTN_GRIP		2
#define VR_BTN_A		7	// A / X
#define VR_BTN_STICK		32	// thumbstick click (axis 0)
#define VR_BTN_TRIGGER		33

// bit layout of vr_in_buttons (must match engine/client/input/in_vr.c)
enum
{
	VRB_RTRIGGER = 0, VRB_LTRIGGER, VRB_RGRIP, VRB_LGRIP, VRB_A, VRB_B, VRB_X, VRB_Y, VRB_LSTICK, VRB_RSTICK
};

static CVAR_DEFINE_AUTO( vr_input_debug, "0", 0, "print raw controller button masks when they change" );

void VR_InputInitCvars( void )
{
	gEngfuncs.Cvar_RegisterVariable( &vr_input_debug );
}

static void VR_Stick( float x, float y, float *ox, float *oy )
{
	float len = sqrt( x * x + y * y );

	if( len < VR_STICK_DEADZONE )
	{
		*ox = *oy = 0.0f;
		return;
	}

	// rescale so output ramps from 0 at the deadzone edge to 1 at full deflection
	len = Q_min( 1.0f, ( len - VR_STICK_DEADZONE ) / ( 1.0f - VR_STICK_DEADZONE )) / len;
	*ox = x * len;
	*oy = y * len;
}

void VR_InputPoll( float head_yaw_relative, float head_pitch, qboolean physical_crouch )
{
	struct VR_IVRSystem_FnTable *sys = (struct VR_IVRSystem_FnTable *)VR_GetSystemTable();
	VRControllerState_t left, right;
	TrackedDeviceIndex_t li, ri;
	qboolean haveLeft = false, haveRight = false;
	int buttons = 0;
	float mx = 0, my = 0, tx = 0, ty = 0;
	static uint64_t last_l, last_r;

	if( !sys )
		return;

	memset( &left, 0, sizeof( left ));
	memset( &right, 0, sizeof( right ));

	li = sys->GetTrackedDeviceIndexForControllerRole( (ETrackedControllerRole)VR_ROLE_LEFT );
	ri = sys->GetTrackedDeviceIndexForControllerRole( (ETrackedControllerRole)VR_ROLE_RIGHT );

	if( li != VR_DEVICE_INVALID )
		haveLeft = sys->GetControllerState( li, &left, sizeof( left ));

	if( ri != VR_DEVICE_INVALID )
		haveRight = sys->GetControllerState( ri, &right, sizeof( right ));

	if( vr_input_debug.value && ( left.ulButtonPressed != last_l || right.ulButtonPressed != last_r ))
	{
		gEngfuncs.Con_Printf( "VR input: L %s mask %llx stick %.2f %.2f trig %.2f grip %.2f | R %s mask %llx stick %.2f %.2f trig %.2f grip %.2f\n",
			haveLeft ? "ok" : "none", (unsigned long long)left.ulButtonPressed, left.rAxis[0].x, left.rAxis[0].y, left.rAxis[1].x, left.rAxis[2].x,
			haveRight ? "ok" : "none", (unsigned long long)right.ulButtonPressed, right.rAxis[0].x, right.rAxis[0].y, right.rAxis[1].x, right.rAxis[2].x );
		last_l = left.ulButtonPressed;
		last_r = right.ulButtonPressed;
	}

	if( haveRight )
	{
		if(( right.ulButtonPressed & VR_BTN( VR_BTN_TRIGGER )) || right.rAxis[1].x > 0.6f ) buttons |= 1 << VRB_RTRIGGER;
		if(( right.ulButtonPressed & VR_BTN( VR_BTN_GRIP )) || right.rAxis[2].x > 0.6f ) buttons |= 1 << VRB_RGRIP;
		if( right.ulButtonPressed & VR_BTN( VR_BTN_A )) buttons |= 1 << VRB_A;
		if( right.ulButtonPressed & VR_BTN( VR_BTN_APPMENU )) buttons |= 1 << VRB_B;
		if( right.ulButtonPressed & VR_BTN( VR_BTN_STICK )) buttons |= 1 << VRB_RSTICK;
		VR_Stick( right.rAxis[0].x, right.rAxis[0].y, &tx, &ty );
	}

	if( haveLeft )
	{
		if(( left.ulButtonPressed & VR_BTN( VR_BTN_TRIGGER )) || left.rAxis[1].x > 0.6f ) buttons |= 1 << VRB_LTRIGGER;
		if(( left.ulButtonPressed & VR_BTN( VR_BTN_GRIP )) || left.rAxis[2].x > 0.6f ) buttons |= 1 << VRB_LGRIP;
		if( left.ulButtonPressed & VR_BTN( VR_BTN_A )) buttons |= 1 << VRB_X;
		if( left.ulButtonPressed & VR_BTN( VR_BTN_APPMENU )) buttons |= 1 << VRB_Y;
		if( left.ulButtonPressed & VR_BTN( VR_BTN_STICK )) buttons |= 1 << VRB_LSTICK;
		VR_Stick( left.rAxis[0].x, left.rAxis[0].y, &mx, &my );
	}

	// raw sticks. the engine decides how to rotate them (see vr_aim_gaze in in_vr.c)
	gEngfuncs.Cvar_SetValue( "vr_in_move_fwd", my );
	gEngfuncs.Cvar_SetValue( "vr_in_move_side", mx );
	gEngfuncs.Cvar_SetValue( "vr_in_turn", tx );
	gEngfuncs.Cvar_SetValue( "vr_in_stick_y", ty );
	gEngfuncs.Cvar_SetValue( "vr_in_head_yaw", head_yaw_relative );
	gEngfuncs.Cvar_SetValue( "vr_in_head_pitch", head_pitch );
	gEngfuncs.Cvar_SetValue( "vr_in_crouch", physical_crouch ? 1.0f : 0.0f );
	gEngfuncs.Cvar_SetValue( "vr_in_buttons", (float)buttons );
	gEngfuncs.Cvar_SetValue( "vr_in_active", ( haveLeft || haveRight ) ? 1.0f : 0.0f );
}
