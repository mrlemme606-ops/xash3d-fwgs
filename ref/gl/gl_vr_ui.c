/*
gl_vr_ui.c - 2D HUD / menu / subtitles on a floating panel in VR
Cry of Fear VR project, milestone 2b.
Copyright (C) 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

How it works: after the two eye images are captured, the window is cleared to transparent
(see VR_FinishFrame). The engine then draws everything 2D into the window as usual. Just before
the frame is presented, that image is copied into a texture and shown by SteamVR as an overlay.
The panel stays where it is until you look far away from it, then eases over to face you again,
so text is steady and reads comfortably without being glued to your face.
*/
#include "gl_vr.h"
#include "xash3d_mathlib.h"

#define OPENVR_API_NODLL
#include <stdint.h>
#include <stdbool.h>
#include "openvr_capi.h"

static CVAR_DEFINE_AUTO( vr_ui_distance, "1.5", 0, "distance of the UI panel in meters" );
static CVAR_DEFINE_AUTO( vr_ui_width, "1.8", 0, "width of the UI panel in meters" );
static CVAR_DEFINE_AUTO( vr_ui_height, "-0.12", 0, "vertical offset of the UI panel from eye level, meters (negative = lower)" );
static CVAR_DEFINE_AUTO( vr_ui_follow, "1", 0, "1 = panel eases over to face you when you look far away from it, 0 = fixed" );
static CVAR_DEFINE_AUTO( vr_ui_alpha, "1.0", 0, "overall opacity of the UI panel" );

static struct
{
	struct VR_IVROverlay_FnTable *overlay;
	VROverlayHandle_t	handle;
	qboolean		failed;
	qboolean		have_yaw;
	float			panel_yaw;	// radians, around the vertical axis
	qboolean		following;
	GLuint			tex;
	int			tex_w, tex_h;

	// panel placement in tracking space (meters), for the laser pointer
	qboolean		valid;
	float			center[3], right[3], normal[3];
	float			w, h;
} ui;

void VR_UIInitCvars( void )
{
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_distance );
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_width );
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_height );
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_follow );
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_alpha );
}

static float VR_WrapPi( float a )
{
	while( a > M_PI_F ) a -= 2.0f * M_PI_F;
	while( a < -M_PI_F ) a += 2.0f * M_PI_F;
	return a;
}

static qboolean VR_UICreate( void )
{
	EVROverlayError err;
	char name[128];

	Q_snprintf( name, sizeof( name ), "FnTable:%s", IVROverlay_Version );
	ui.overlay = (struct VR_IVROverlay_FnTable *)VR_GetInterface( name );

	if( !ui.overlay )
	{
		gEngfuncs.Con_Printf( S_ERROR "VR: can't get the overlay interface, HUD will only show on the desktop\n" );
		return false;
	}

	err = ui.overlay->CreateOverlay( (char *)"xash3d.cof.ui", (char *)"Cry of Fear UI", &ui.handle );

	if( err != EVROverlayError_VROverlayError_None )
	{
		gEngfuncs.Con_Printf( S_ERROR "VR: CreateOverlay failed (code %d)\n", (int)err );
		return false;
	}

	ui.overlay->SetOverlayWidthInMeters( ui.handle, vr_ui_width.value );
	ui.overlay->ShowOverlay( ui.handle );
	return true;
}

static void VR_UIEnsureTexture( int w, int h )
{
	if( ui.tex && ui.tex_w == w && ui.tex_h == h )
		return;

	if( ui.tex )
		pglDeleteTextures( 1, &ui.tex );

	pglGenTextures( 1, &ui.tex );
	pglBindTexture( GL_TEXTURE_2D, ui.tex );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	ui.tex_w = w;
	ui.tex_h = h;
}

// called at the very start of R_EndFrame, before the frame is presented
void VR_UIUpdate( void )
{
	float m[3][4];
	HmdMatrix34_t panel;
	Texture_t tex;
	float fx, fz, len, head_yaw, diff, dist, zx, zz;
	int w = gpGlobals->width, h = gpGlobals->height;

	if( ui.failed || !VR_IsActive( ))
		return;

	if( !ui.overlay && !VR_UICreate( ))
	{
		ui.failed = true;
		return;
	}

	// copy the transparent-background 2D layer
	VR_UIEnsureTexture( w, h );
	pglBindTexture( GL_TEXTURE_2D, ui.tex );
	pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h );
	glState.currentTextures[glState.activeTMU] = -1;
	glState.currentTexturesIndex[glState.activeTMU] = -1;

	// where is the head looking (horizontally)?
	VR_GetHeadMatrix( m );
	fx = -m[0][2];
	fz = -m[2][2];
	len = sqrt( fx * fx + fz * fz );

	if( len > 0.2f )
		head_yaw = atan2( fx, -fz );	// 0 = looking along -Z, positive toward +X
	else
		head_yaw = ui.have_yaw ? ui.panel_yaw : 0.0f;

	if( !ui.have_yaw )
	{
		ui.panel_yaw = head_yaw;
		ui.have_yaw = true;
	}

	diff = VR_WrapPi( head_yaw - ui.panel_yaw );

	if( vr_ui_follow.value )
	{
		// start easing when you look well away from the panel, stop when it is nearly centered again
		if( fabs( diff ) > DEG2RAD( 40.0f )) ui.following = true;
		else if( fabs( diff ) < DEG2RAD( 8.0f )) ui.following = false;

		if( ui.following )
			ui.panel_yaw = VR_WrapPi( ui.panel_yaw + diff * 0.06f );
	}

	dist = vr_ui_distance.value;

	// panel faces the viewer: its +Z axis points back at the head
	zx = -sin( ui.panel_yaw );
	zz = cos( ui.panel_yaw );

	panel.m[0][0] = zz;	panel.m[0][1] = 0.0f;	panel.m[0][2] = zx;	panel.m[0][3] = m[0][3] + sin( ui.panel_yaw ) * dist;
	panel.m[1][0] = 0.0f;	panel.m[1][1] = 1.0f;	panel.m[1][2] = 0.0f;	panel.m[1][3] = m[1][3] + vr_ui_height.value;
	panel.m[2][0] = -zx;	panel.m[2][1] = 0.0f;	panel.m[2][2] = zz;	panel.m[2][3] = m[2][3] - cos( ui.panel_yaw ) * dist;

	ui.center[0] = panel.m[0][3]; ui.center[1] = panel.m[1][3]; ui.center[2] = panel.m[2][3];
	ui.right[0] = panel.m[0][0]; ui.right[1] = panel.m[1][0]; ui.right[2] = panel.m[2][0];
	ui.normal[0] = panel.m[0][2]; ui.normal[1] = panel.m[1][2]; ui.normal[2] = panel.m[2][2];
	ui.w = vr_ui_width.value;
	ui.h = vr_ui_width.value * (float)h / (float)w;
	ui.valid = true;

	tex.handle = (void *)(uintptr_t)ui.tex;
	tex.eType = (ETextureType)1;		// OpenGL
	tex.eColorSpace = (EColorSpace)1;	// gamma

	ui.overlay->SetOverlayWidthInMeters( ui.handle, vr_ui_width.value );
	ui.overlay->SetOverlayAlpha( ui.handle, vr_ui_alpha.value );
	ui.overlay->SetOverlayTransformAbsolute( ui.handle, (ETrackingUniverseOrigin)1, &panel );
	ui.overlay->SetOverlayTexture( ui.handle, &tex );
}

/*
=================
Where does a ray (tracking space, meters) hit the UI panel?
u: 0..1 from the left, v: 0..1 from the bottom, dist in meters.
=================
*/
qboolean VR_UIRayHit( const float origin[3], const float dir[3], float *u, float *v, float *dist )
{
	float denom, t, rel[3], x, y;
	int i;

	if( !ui.valid )
		return false;

	denom = dir[0] * ui.normal[0] + dir[1] * ui.normal[1] + dir[2] * ui.normal[2];

	if( fabs( denom ) < 0.0001f )
		return false;

	t = (( ui.center[0] - origin[0] ) * ui.normal[0] + ( ui.center[1] - origin[1] ) * ui.normal[1] + ( ui.center[2] - origin[2] ) * ui.normal[2] ) / denom;

	if( t <= 0.0f || t > 10.0f )
		return false;

	for( i = 0; i < 3; i++ )
		rel[i] = origin[i] + dir[i] * t - ui.center[i];

	x = ( rel[0] * ui.right[0] + rel[1] * ui.right[1] + rel[2] * ui.right[2] ) / ui.w + 0.5f;
	y = rel[1] / ui.h + 0.5f;	// the panel is upright, so "up" is the vertical axis

	if( x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f )
		return false;

	*u = x;
	*v = y;
	*dist = t;
	return true;
}
