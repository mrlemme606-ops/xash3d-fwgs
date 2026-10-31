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
static CVAR_DEFINE_AUTO( vr_ui_size, "1.3", 0, "height of the UI panel in meters. The width follows the window's aspect ratio, so the menus are the same physical size at every resolution" );
static CVAR_DEFINE_AUTO( vr_ui_width, "0", 0, "force the UI panel width in meters (0 = automatic from vr_ui_size)" );
static CVAR_DEFINE_AUTO( vr_ui_maxwidth, "2.6", 0, "widest the UI panel may get, in meters (very wide windows are scaled down to this)" );
static CVAR_DEFINE_AUTO( vr_hud_hand, "1", FCVAR_ARCHIVE, "1 = the HUD (health, ammo...) is shown on the back of your right hand instead of the floating panel" );
static CVAR_DEFINE_AUTO( vr_hud_width, "0.30", FCVAR_ARCHIVE, "width in meters of the HUD on your hand" );
static CVAR_DEFINE_AUTO( vr_hud_crop, "0 0 1 0.25", FCVAR_ARCHIVE, "part of the screen the hand HUD shows, as: left bottom right top (0 to 1, from the bottom left). Needs quotes. Default is the bottom quarter" );
static CVAR_DEFINE_AUTO( vr_hud_pos, "0 0.045 0.06", FCVAR_ARCHIVE, "where the HUD sits on the controller, as: x y z in meters (y = up, z = toward your wrist). Needs quotes" );
static CVAR_DEFINE_AUTO( vr_hud_tilt, "0", FCVAR_ARCHIVE, "tilt of the hand HUD around the controller's side axis, in degrees" );
static CVAR_DEFINE_AUTO( vr_hud_flipv, "0", FCVAR_ARCHIVE, "set to 1 if the hand HUD shows the top of the screen instead of the bottom" );
static CVAR_DEFINE_AUTO( vr_hud_alpha, "1.0", FCVAR_ARCHIVE, "opacity of the hand HUD when you look at it" );
static CVAR_DEFINE_AUTO( vr_ui_pointer, "0.035", 0, "size in meters of the dot that shows where your hand points on the UI, drawn on top of everything (0 = off)" );
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

// the dot that shows where the hand points, a second overlay above the panel
static struct
{
	VROverlayHandle_t	handle;
	qboolean		created, failed, shown;
	qboolean		want;
	float			u, v;
	GLuint			tex;
} dot;

// the HUD (health, ammo...) shown on the back of the right hand
static struct
{
	VROverlayHandle_t	handle;
	qboolean		created, failed, shown, have;
	GLuint			tex;
	int			tex_w, tex_h;
} hud;

static void VR_UIUpdateHUD( void );

static void VR_UIUpdatePointer( const HmdMatrix34_t *panel );

void VR_UIInitCvars( void )
{
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_distance );
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_size );
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_width );
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_maxwidth );
	gEngfuncs.Cvar_RegisterVariable( &vr_ui_pointer );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_hand );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_width );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_crop );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_pos );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_tilt );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_flipv );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_alpha );
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

	ui.overlay->SetOverlayWidthInMeters( ui.handle, 1.8f );
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

/*
=================
Called by the renderer right after the HUD is drawn into the window and before the menus.
The window is copied (it holds only the HUD, the world went to the eye images) and then cleared,
so the menus that follow get a clean panel.
=================
*/
void VR_HUDCapture( void )
{
	int w = gpGlobals->width, h = gpGlobals->height;
	GLfloat clear[4];
	GLboolean scissor;

	hud.have = false;

	if( !vr_hud_hand.value || ui.failed || hud.failed || !VR_IsActive( ) || !VR_FrameDone( ))
		return;

	if( !hud.tex || hud.tex_w != w || hud.tex_h != h )
	{
		if( hud.tex )
			pglDeleteTextures( 1, &hud.tex );

		pglGenTextures( 1, &hud.tex );
		pglBindTexture( GL_TEXTURE_2D, hud.tex );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		hud.tex_w = w;
		hud.tex_h = h;
	}

	pglBindTexture( GL_TEXTURE_2D, hud.tex );
	pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h );
	glState.currentTextures[glState.activeTMU] = -1;
	glState.currentTexturesIndex[glState.activeTMU] = -1;

	// the HUD is on its way to the hand, wipe it from the window
	scissor = pglIsEnabled( GL_SCISSOR_TEST );
	pglGetFloatv( GL_COLOR_CLEAR_VALUE, clear );

	if( scissor )
		pglDisable( GL_SCISSOR_TEST );

	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	pglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	pglClear( GL_COLOR_BUFFER_BIT );
	pglClearColor( clear[0], clear[1], clear[2], clear[3] );

	if( scissor )
		pglEnable( GL_SCISSOR_TEST );

	hud.have = true;
}

// "0.5 1 2" -> 0.5, 1, 2 (what is missing keeps its default)
static void VR_ParseFloats( const char *str, float *out, int count )
{
	int i;

	for( i = 0; i < count; i++ )
	{
		while( *str == ' ' || *str == '\t' ) str++;

		if( !*str )
			break;

		out[i] = Q_atof( str );

		while( *str && *str != ' ' && *str != '\t' ) str++;
	}
}

static void VR_UIHideHUD( void )
{
	if( hud.shown )
	{
		ui.overlay->HideOverlay( hud.handle );
		hud.shown = false;
	}
}

static void VR_UIUpdateHUD( void )
{
	float c[4] = { 0.0f, 0.0f, 1.0f, 0.25f }, pos[3] = { 0.0f, 0.045f, 0.06f };
	float cm[3][4], hm[3][4], n[3], p[3], to[3], len, facing, alpha, cs, sn, width, height;
	HmdMatrix34_t rel;
	VRTextureBounds_t bounds;
	Texture_t tex;
	uint32_t dev = 0;
	int i;

	if( hud.failed || !ui.overlay )
		return;

	if( !vr_hud_hand.value || !hud.have || !VR_GetHandPose( 1, cm, &dev ))
	{
		VR_UIHideHUD();
		return;
	}

	if( !hud.created )
	{
		if( ui.overlay->CreateOverlay( (char *)"xash3d.cof.hud", (char *)"Cry of Fear HUD", &hud.handle ) != EVROverlayError_VROverlayError_None )
		{
			hud.failed = true;
			return;
		}

		hud.created = true;
	}

	VR_ParseFloats( vr_hud_crop.string, c, 4 );
	VR_ParseFloats( vr_hud_pos.string, pos, 3 );
	c[0] = bound( 0.0f, c[0], 0.99f );
	c[1] = bound( 0.0f, c[1], 0.99f );
	c[2] = bound( c[0] + 0.01f, c[2], 1.0f );
	c[3] = bound( c[1] + 0.01f, c[3], 1.0f );

	bounds.uMin = c[0];
	bounds.uMax = c[2];
	bounds.vMin = vr_hud_flipv.value ? 1.0f - c[3] : c[1];
	bounds.vMax = vr_hud_flipv.value ? 1.0f - c[1] : c[3];

	// the HUD lies on the back of the hand, facing up and toward you. Controller axes: x right, y up, z toward the wrist
	cs = cos( DEG2RAD( vr_hud_tilt.value ));
	sn = sin( DEG2RAD( vr_hud_tilt.value ));

	rel.m[0][0] = 1.0f;	rel.m[0][1] = 0.0f;	rel.m[0][2] = 0.0f;	rel.m[0][3] = pos[0];
	rel.m[1][0] = 0.0f;	rel.m[1][1] = sn;	rel.m[1][2] = cs;	rel.m[1][3] = pos[1];
	rel.m[2][0] = 0.0f;	rel.m[2][1] = -cs;	rel.m[2][2] = sn;	rel.m[2][3] = pos[2];

	// is the hand turned toward your eyes? fade in when it is
	VR_GetHeadMatrix( hm );

	for( i = 0; i < 3; i++ )
	{
		n[i] = cm[i][1] * cs + cm[i][2] * sn;
		p[i] = cm[i][3] + cm[i][0] * pos[0] + cm[i][1] * pos[1] + cm[i][2] * pos[2];
		to[i] = hm[i][3] - p[i];
	}

	len = sqrt( to[0] * to[0] + to[1] * to[1] + to[2] * to[2] );
	facing = len > 0.01f ? ( n[0] * to[0] + n[1] * to[1] + n[2] * to[2] ) / len : 0.0f;
	alpha = bound( 0.0f, ( facing - 0.25f ) / 0.4f, 1.0f ) * vr_hud_alpha.value;

	if( alpha < 0.02f )
	{
		VR_UIHideHUD();
		return;
	}

	width = vr_hud_width.value;
	height = width * (( c[3] - c[1] ) * (float)hud.tex_h ) / (( c[2] - c[0] ) * (float)hud.tex_w );

	tex.handle = (void *)(uintptr_t)hud.tex;
	tex.eType = (ETextureType)1;
	tex.eColorSpace = (EColorSpace)1;

	ui.overlay->SetOverlayWidthInMeters( hud.handle, width );
	ui.overlay->SetOverlayTextureBounds( hud.handle, &bounds );
	ui.overlay->SetOverlayAlpha( hud.handle, alpha );
	ui.overlay->SetOverlayTransformTrackedDeviceRelative( hud.handle, dev, &rel );
	ui.overlay->SetOverlayTexture( hud.handle, &tex );
	(void)height;

	if( !hud.shown )
	{
		ui.overlay->ShowOverlay( hud.handle );
		hud.shown = true;
	}
}

void VR_UISetPointer( qboolean visible, float u, float v )
{
	dot.want = visible;
	dot.u = u;
	dot.v = v;
}

// a small round marker: bright centre, dark outline, see-through outside
static void VR_UIDotTexture( void )
{
	byte pix[64 * 64 * 4];
	int x, y;

	for( y = 0; y < 64; y++ )
	{
		for( x = 0; x < 64; x++ )
		{
			float dx = x - 31.5f, dy = y - 31.5f, r = sqrt( dx * dx + dy * dy );
			byte *p = &pix[( y * 64 + x ) * 4];

			if( r < 7.0f ) { p[0] = 60; p[1] = 230; p[2] = 255; p[3] = 255; }
			else if( r < 11.0f ) { p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 230; }
			else { p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; }
		}
	}

	pglGenTextures( 1, &dot.tex );
	pglBindTexture( GL_TEXTURE_2D, dot.tex );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix );
	glState.currentTextures[glState.activeTMU] = -1;
	glState.currentTexturesIndex[glState.activeTMU] = -1;
}

static void VR_UIUpdatePointer( const HmdMatrix34_t *panel )
{
	HmdMatrix34_t m;
	Texture_t tex;
	float ox, oy;
	int i;

	if( dot.failed )
		return;

	if( vr_ui_pointer.value <= 0.0f || !dot.want )
	{
		if( dot.shown )
		{
			ui.overlay->HideOverlay( dot.handle );
			dot.shown = false;
		}

		return;
	}

	if( !dot.created )
	{
		if( ui.overlay->CreateOverlay( (char *)"xash3d.cof.pointer", (char *)"Cry of Fear UI pointer", &dot.handle ) != EVROverlayError_VROverlayError_None )
		{
			dot.failed = true;
			return;
		}

		VR_UIDotTexture();
		ui.overlay->SetOverlaySortOrder( dot.handle, 10 );	// above the panel
		tex.handle = (void *)(uintptr_t)dot.tex;
		tex.eType = (ETextureType)1;
		tex.eColorSpace = (EColorSpace)1;
		ui.overlay->SetOverlayTexture( dot.handle, &tex );
		dot.created = true;
	}

	// on the panel where the ray hits, a little toward the viewer so it never sinks into the panel
	ox = ( dot.u - 0.5f ) * ui.w;
	oy = ( dot.v - 0.5f ) * ui.h;
	m = *panel;

	for( i = 0; i < 3; i++ )
		m.m[i][3] = panel->m[i][3] + panel->m[i][0] * ox + panel->m[i][1] * oy + panel->m[i][2] * 0.012f;

	ui.overlay->SetOverlayWidthInMeters( dot.handle, vr_ui_pointer.value );
	ui.overlay->SetOverlayTransformAbsolute( dot.handle, (ETrackingUniverseOrigin)1, &m );

	if( !dot.shown )
	{
		ui.overlay->ShowOverlay( dot.handle );
		dot.shown = true;
	}
}

// called at the very start of R_EndFrame, before the frame is presented
void VR_UIUpdate( void )
{
	float m[3][4];
	HmdMatrix34_t panel;
	Texture_t tex;
	float fx, fz, len, head_yaw, diff, dist, zx, zz, pw, ph;
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

	// the menus are laid out relative to the window height, so the panel height is what stays fixed
	ph = vr_ui_size.value;

	if( vr_ui_width.value > 0.0f )
	{
		pw = vr_ui_width.value;
		ph = pw * (float)h / (float)w;
	}
	else
	{
		pw = ph * (float)w / (float)h;
	}

	if( pw > vr_ui_maxwidth.value )
	{
		ph *= vr_ui_maxwidth.value / pw;
		pw = vr_ui_maxwidth.value;
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
	ui.w = pw;
	ui.h = ph;
	ui.valid = true;

	tex.handle = (void *)(uintptr_t)ui.tex;
	tex.eType = (ETextureType)1;		// OpenGL
	tex.eColorSpace = (EColorSpace)1;	// gamma

	ui.overlay->SetOverlayWidthInMeters( ui.handle, pw );
	ui.overlay->SetOverlayAlpha( ui.handle, vr_ui_alpha.value );
	ui.overlay->SetOverlayTransformAbsolute( ui.handle, (ETrackingUniverseOrigin)1, &panel );
	ui.overlay->SetOverlayTexture( ui.handle, &tex );

	VR_UIUpdatePointer( &panel );
	VR_UIUpdateHUD();
	VR_DrainGLErrors();
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
