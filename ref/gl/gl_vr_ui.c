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

#ifndef GL_COLOR_CLEAR_VALUE
#define GL_COLOR_CLEAR_VALUE	0x0C22
#endif

#ifndef GL_SCISSOR_BOX
#define GL_SCISSOR_BOX		0x0C10
#endif

#define OPENVR_API_NODLL
#include <stdint.h>
#include <stdbool.h>
#include "openvr_capi.h"

static CVAR_DEFINE_AUTO( vr_ui_distance, "1.5", 0, "distance of the UI panel in meters" );
static CVAR_DEFINE_AUTO( vr_ui_size, "1.3", 0, "height of the UI panel in meters. The width follows the window's aspect ratio, so the menus are the same physical size at every resolution" );
static CVAR_DEFINE_AUTO( vr_ui_width, "0", 0, "force the UI panel width in meters (0 = automatic from vr_ui_size)" );
static CVAR_DEFINE_AUTO( vr_ui_maxwidth, "2.6", 0, "widest the UI panel may get, in meters (very wide windows are scaled down to this)" );
static CVAR_DEFINE_AUTO( vr_hud_hand, "1", FCVAR_ARCHIVE, "1 = two strips of the HUD (health/stamina and ammo, see vr_hud_band, vr_hud_x1, vr_hud_x2) are shown on the back of your right hand instead of the floating panel. Everything else stays on the panel" );
static CVAR_DEFINE_AUTO( vr_hud_width, "0.22", FCVAR_ARCHIVE, "width in meters of the HUD on your hand" );
static CVAR_DEFINE_AUTO( vr_hud_band, "0 0.14", FCVAR_ARCHIVE, "height of the strips taken from the HUD, as: bottom top (0 to 1 of the screen height, from the bottom). Needs quotes" );
static CVAR_DEFINE_AUTO( vr_hud_x1, "0 0.30", FCVAR_ARCHIVE, "first strip (health, stamina), as: left right (0 to 1 of the screen width). Needs quotes" );
static CVAR_DEFINE_AUTO( vr_hud_x2, "0.78 1", FCVAR_ARCHIVE, "second strip (ammo), as: left right. Set to \"0 0\" for no second strip" );
static CVAR_DEFINE_AUTO( vr_hud_pos, "0 0.045 0.06", FCVAR_ARCHIVE, "where the HUD sits on the controller, as: x y z in meters (y = up, z = toward your wrist). Needs quotes" );
static CVAR_DEFINE_AUTO( vr_hud_side, "0", FCVAR_ARCHIVE, "which hand wears the HUD: 0 = left (like a watch), 1 = right" );
static CVAR_DEFINE_AUTO( vr_hud_rot, "0 0", FCVAR_ARCHIVE, "extra turn of the hand HUD, as: yaw roll in degrees (vr_hud_tilt is the third direction). Needs quotes" );
static CVAR_DEFINE_AUTO( vr_hud_tilt, "0", FCVAR_ARCHIVE, "tilt of the hand HUD around the controller's side axis, in degrees" );
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
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_side );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_rot );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_width );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_band );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_x1 );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_x2 );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_pos );
	gEngfuncs.Cvar_RegisterVariable( &vr_hud_tilt );
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

// the two strips of the screen that go to the hand, in pixels: x, y, width, height (width 0 = not used)
static void VR_HUDRects( int w, int h, int rect[2][4] )
{
	float band[2] = { 0.0f, 0.14f }, xs[2][2] = { { 0.0f, 0.30f }, { 0.78f, 1.0f } };
	int i;

	VR_ParseFloats( vr_hud_band.string, band, 2 );
	VR_ParseFloats( vr_hud_x1.string, xs[0], 2 );
	VR_ParseFloats( vr_hud_x2.string, xs[1], 2 );

	band[0] = bound( 0.0f, band[0], 0.95f );
	band[1] = bound( band[0] + 0.02f, band[1], 1.0f );

	for( i = 0; i < 2; i++ )
	{
		float l = bound( 0.0f, xs[i][0], 1.0f ), r = bound( l, xs[i][1], 1.0f );

		rect[i][0] = (int)( l * w );
		rect[i][1] = (int)( band[0] * h );
		rect[i][2] = (int)(( r - l ) * w );
		rect[i][3] = (int)(( band[1] - band[0] ) * h );

		if( rect[i][2] < 4 )
			rect[i][2] = 0;
	}
}

/*
=================
Called by the renderer right after the in-game HUD is drawn and before the menus.
Cry of Fear draws its inventory, menus and subtitles through the same path as the HUD, so only
the strips that hold health, stamina and ammo are taken out of the window (copied side by side
into one small texture, then wiped from the window). Everything else stays on the panel.
=================
*/
void VR_HUDCapture( void )
{
	int w = gpGlobals->width, h = gpGlobals->height, rect[2][4], i, xoff = 0, tw, th;
	GLint scissor_box[4];
	GLfloat clear[4];
	GLboolean scissor;

	hud.have = false;

	if( !vr_hud_hand.value || ui.failed || hud.failed || !VR_IsActive( ) || !VR_FrameDone( ))
		return;

	// inventory, computer, letters, cutscenes: the HUD stays in the picture (nothing is cut out of it, nothing goes on the hand)
	{
		static cvar_t *off;

		if( !off ) off = gEngfuncs.pfnGetCvarPointer( "vr_in_hud_off" );
		if( off && off->value > 0.5f )
			return;
	}

	VR_HUDRects( w, h, rect );
	th = rect[0][3];
	tw = rect[0][2] + rect[1][2];

	if( tw < 8 || th < 4 )
		return;

	if( !hud.tex || hud.tex_w != tw || hud.tex_h != th )
	{
		if( hud.tex )
			pglDeleteTextures( 1, &hud.tex );

		pglGenTextures( 1, &hud.tex );
		pglBindTexture( GL_TEXTURE_2D, hud.tex );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		hud.tex_w = tw;
		hud.tex_h = th;
	}

	pglBindTexture( GL_TEXTURE_2D, hud.tex );

	for( i = 0; i < 2; i++ )
	{
		if( !rect[i][2] )
			continue;

		pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, xoff, 0, rect[i][0], rect[i][1], rect[i][2], rect[i][3] );
		xoff += rect[i][2];
	}

	glState.currentTextures[glState.activeTMU] = -1;
	glState.currentTexturesIndex[glState.activeTMU] = -1;

	// wipe just those strips from the window
	scissor = pglIsEnabled( GL_SCISSOR_TEST );
	pglGetIntegerv( GL_SCISSOR_BOX, scissor_box );
	pglGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	pglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	pglEnable( GL_SCISSOR_TEST );

	for( i = 0; i < 2; i++ )
	{
		if( !rect[i][2] )
			continue;

		pglScissor( rect[i][0], rect[i][1], rect[i][2], rect[i][3] );
		pglClear( GL_COLOR_BUFFER_BIT );
	}

	pglScissor( scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3] );

	if( !scissor )
		pglDisable( GL_SCISSOR_TEST );

	pglClearColor( clear[0], clear[1], clear[2], clear[3] );
	hud.have = true;
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
	float pos[3] = { 0.0f, 0.045f, 0.06f };
	float cm[3][4], hm[3][4], n[3], p[3], to[3], len, facing, alpha, cs, sn;
	HmdMatrix34_t rel;
	Texture_t tex;
	uint32_t dev = 0;
	int i;

	if( hud.failed || !ui.overlay )
		return;

	if( !vr_hud_hand.value || !hud.have || !VR_GetHandPose( vr_hud_side.value ? 1 : 0, cm, &dev ))
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

	VR_ParseFloats( vr_hud_pos.string, pos, 3 );

	// the HUD lies on the back of the hand, facing up and toward you. Controller axes: x right, y up, z toward the wrist
	cs = cos( DEG2RAD( vr_hud_tilt.value ));
	sn = sin( DEG2RAD( vr_hud_tilt.value ));

	rel.m[0][0] = 1.0f;	rel.m[0][1] = 0.0f;	rel.m[0][2] = 0.0f;	rel.m[0][3] = pos[0];
	rel.m[1][0] = 0.0f;	rel.m[1][1] = sn;	rel.m[1][2] = cs;	rel.m[1][3] = pos[1];
	rel.m[2][0] = 0.0f;	rel.m[2][1] = -cs;	rel.m[2][2] = sn;	rel.m[2][3] = pos[2];

	// the extra turns: yaw around the panel's up axis, roll around its normal (done in the panel's own space)
	{
		float rot[2] = { 0.0f, 0.0f }, cy, sy, cr, sr, b[3][3], e[3][3];
		int r, c, k;

		VR_ParseFloats( vr_hud_rot.string, rot, 2 );
		cy = cos( DEG2RAD( rot[0] )); sy = sin( DEG2RAD( rot[0] ));
		cr = cos( DEG2RAD( rot[1] )); sr = sin( DEG2RAD( rot[1] ));

		// e = Ry( yaw ) * Rz( roll ), in the panel's own axes (x right, y up, z out of the panel)
		e[0][0] = cy * cr;	e[0][1] = -cy * sr;	e[0][2] = sy;
		e[1][0] = sr;		e[1][1] = cr;		e[1][2] = 0.0f;
		e[2][0] = -sy * cr;	e[2][1] = sy * sr;	e[2][2] = cy;

		for( r = 0; r < 3; r++ )
			for( c = 0; c < 3; c++ )
				b[r][c] = rel.m[r][c];

		for( r = 0; r < 3; r++ )
			for( c = 0; c < 3; c++ )
			{
				float v = 0.0f;

				for( k = 0; k < 3; k++ )
					v += b[r][k] * e[k][c];

				rel.m[r][c] = v;
			}
	}

	// is the hand turned toward your eyes? fade in when it is
	VR_GetHeadMatrix( hm );

	for( i = 0; i < 3; i++ )
	{
		n[i] = cm[i][0] * rel.m[0][2] + cm[i][1] * rel.m[1][2] + cm[i][2] * rel.m[2][2];
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

	tex.handle = (void *)(uintptr_t)hud.tex;
	tex.eType = (ETextureType)1;
	tex.eColorSpace = (EColorSpace)1;

	ui.overlay->SetOverlayWidthInMeters( hud.handle, vr_hud_width.value );
	ui.overlay->SetOverlayAlpha( hud.handle, alpha );
	ui.overlay->SetOverlayTransformTrackedDeviceRelative( hud.handle, dev, &rel );
	ui.overlay->SetOverlayTexture( hud.handle, &tex );

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

/*
=================
SteamVR keyboard. The engine (in_vr.c) says when a text box is open (vr_in_kbd). The characters typed
on the keyboard come back as overlay events and are handed to the engine in vr_in_kbd_chars, which
turns them into key presses for the game.
=================
*/
static void VR_UIKeyboard( void )
{
	static cvar_t *want_cv, *chars_cv;
	static qboolean kbd_open, kbd_blocked;
	struct VREvent_t ev;
	qboolean want;

	if( !ui.overlay )
		return;

	if( !want_cv ) want_cv = gEngfuncs.pfnGetCvarPointer( "vr_in_kbd" );
	if( !chars_cv ) chars_cv = gEngfuncs.pfnGetCvarPointer( "vr_in_kbd_chars" );

	want = want_cv && want_cv->value >= 0.5f;

	if( !want )
		kbd_blocked = false;

	if( want && !kbd_open && !kbd_blocked )
	{
		if( ui.overlay->ShowKeyboardForOverlay( ui.handle, EGamepadTextInputMode_k_EGamepadTextInputModeNormal,
			EGamepadTextInputLineMode_k_EGamepadTextInputLineModeSingleLine, (char *)"Type here", 64, (char *)"", true, 0 ) == EVROverlayError_VROverlayError_None )
			kbd_open = true;
		else kbd_blocked = true;	// could not open it, do not try every frame
	}
	else if( !want && kbd_open )
	{
		ui.overlay->HideKeyboard();
		kbd_open = false;
	}

	while( ui.overlay->PollNextOverlayEvent( ui.handle, &ev, sizeof( ev )))
	{
		char buf[8], all[256];

		if( ev.eventType == EVREventType_VREvent_KeyboardCharInput )
		{
			// the header declares the typed text as an array of pointers, it really is 8 plain characters
			memcpy( buf, &ev.data.keyboard, 7 );
			buf[7] = 0;
		}
		else if( ev.eventType == EVREventType_VREvent_KeyboardDone )
		{
			buf[0] = '\r';
			buf[1] = 0;
		}
		else if( ev.eventType == EVREventType_VREvent_KeyboardClosed )
		{
			kbd_open = false;
			kbd_blocked = true;
			gEngfuncs.Cvar_SetValue( "vr_in_kbd_closed", 1.0f );
			continue;
		}
		else continue;

		Q_snprintf( all, sizeof( all ), "%s%s", chars_cv ? chars_cv->string : "", buf );
		gEngfuncs.Cvar_Set( "vr_in_kbd_chars", all );
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

	VR_UIKeyboard();

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
