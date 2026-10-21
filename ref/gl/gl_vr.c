/*
gl_vr.c - OpenVR (SteamVR) stereo output for the GL renderer
Cry of Fear VR project, milestone 1: head-tracked stereo view.
Copyright (C) 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Design notes:
- The engine is 32-bit (Cry of Fear's DLLs are 32-bit), so we use OpenVR, loading
  openvr_api.dll at runtime. SteamVR ships a 32-bit copy in SteamVR\bin\win32.
- No FBO redirection: each eye is rendered with the normal scene path, then copied
  from the current framebuffer into a per-eye texture and submitted to the compositor.
- The 2D HUD/menus are NOT in the headset yet (milestone 1). They still show in the window.
*/
#include "gl_vr.h"
#include "xash3d_mathlib.h"

#define OPENVR_API_NODLL
#include <stdint.h>
#include <stdbool.h>
#include "openvr_capi.h"

#define VR_MAX_DEVICES 64

typedef intptr_t (*pfnVR_InitInternal)( EVRInitError *peError, EVRApplicationType eType );
typedef intptr_t (*pfnVR_GetGenericInterface)( const char *pchInterfaceVersion, EVRInitError *peError );
typedef bool (*pfnVR_IsHmdPresent)( void );

static CVAR_DEFINE_AUTO( vr_enable, "0", 0, "enable SteamVR (OpenVR) stereo rendering" );
static CVAR_DEFINE_AUTO( vr_worldscale, "39.37", 0, "game units per meter (GoldSrc: 1 unit = 1 inch)" );
static CVAR_DEFINE_AUTO( vr_flip_roll, "0", 0, "set to 1 if head roll is inverted" );

static struct
{
	qboolean	initialized;
	qboolean	failed;
	qboolean	frame_done;	// one stereo frame per engine frame
	qboolean	have_ref;
	qboolean	in_eye_pass;

	void		*lib;
	struct VR_IVRSystem_FnTable	*system;
	struct VR_IVRCompositor_FnTable	*compositor;

	TrackedDevicePose_t	poses[VR_MAX_DEVICES];

	float		tan_frustum[2][4];	// per eye: left, right, up, down (tangents, left/down negative)
	float		eye_offset[2][3];	// eye position in head space, meters (OpenVR axes)

	vec3_t		ref_pos;	// head position when recentered (game axes, units)
	float		ref_yaw;	// head yaw when recentered (degrees)

	vec3_t		head_pos;	// current head position, OpenVR axes, meters
	vec3_t		axis_fwd, axis_left, axis_up;	// current head basis in game axes (unrotated)

	float		cur_frustum[4];

	GLuint		tex[2];
	int		tex_w, tex_h;
} vr;

/*
=================
OpenVR (x right, y up, -z forward, meters)  ->  game (x forward, y left, z up)
=================
*/
static void VR_ConvDir( const float *v, vec3_t out )
{
	out[0] = -v[2];
	out[1] = -v[0];
	out[2] =  v[1];
}

static void VR_ConvPos( const float *v, vec3_t out )
{
	VR_ConvDir( v, out );
	VectorScale( out, vr_worldscale.value, out );
}

static void VR_RotateZ( const vec3_t in, float degrees, vec3_t out )
{
	float s = sin( DEG2RAD( degrees ));
	float c = cos( DEG2RAD( degrees ));
	float x = in[0] * c - in[1] * s;
	float y = in[0] * s + in[1] * c;

	out[0] = x;
	out[1] = y;
	out[2] = in[2];
}

static void VR_Fail( const char *msg, int code )
{
	vr.failed = true;
	gEngfuncs.Con_Printf( S_ERROR "VR: %s (code %d). Falling back to normal rendering.\n", msg, code );
}

static void VR_Recenter_f( void )
{
	vr.have_ref = false; // taken again from the next pose
	gEngfuncs.Con_Printf( "VR: recentering on next frame\n" );
}

void VR_InitCvars( void )
{
	gEngfuncs.Cvar_RegisterVariable( &vr_enable );
	gEngfuncs.Cvar_RegisterVariable( &vr_worldscale );
	gEngfuncs.Cvar_RegisterVariable( &vr_flip_roll );
	gEngfuncs.Cmd_AddCommand( "vr_recenter", VR_Recenter_f, "recenter the VR view on the current head position and direction" );
}

static void VR_Init( void )
{
	pfnVR_InitInternal initInternal;
	pfnVR_GetGenericInterface getInterface;
	pfnVR_IsHmdPresent isHmdPresent;
	EVRInitError err = EVRInitError_VRInitError_None;
	char name[128];
	int eye;

	vr.lib = gEngfuncs.COM_LoadLibrary( "openvr_api.dll", false, true );

	if( !vr.lib )
	{
		VR_Fail( "can't load openvr_api.dll (copy the 32-bit one from SteamVR\\bin\\win32 next to xash3d.exe)", 0 );
		return;
	}

	initInternal = (pfnVR_InitInternal)gEngfuncs.COM_GetProcAddress( vr.lib, "VR_InitInternal" );
	getInterface = (pfnVR_GetGenericInterface)gEngfuncs.COM_GetProcAddress( vr.lib, "VR_GetGenericInterface" );
	isHmdPresent = (pfnVR_IsHmdPresent)gEngfuncs.COM_GetProcAddress( vr.lib, "VR_IsHmdPresent" );

	if( !initInternal || !getInterface )
	{
		VR_Fail( "openvr_api.dll is missing expected exports", 0 );
		return;
	}

	if( isHmdPresent && !isHmdPresent( ))
	{
		VR_Fail( "no headset detected (start SteamVR and connect your headset first)", 0 );
		return;
	}

	initInternal( &err, EVRApplicationType_VRApplication_Scene );

	if( err != EVRInitError_VRInitError_None )
	{
		VR_Fail( "VR_InitInternal failed", (int)err );
		return;
	}

	Q_snprintf( name, sizeof( name ), "FnTable:%s", IVRSystem_Version );
	vr.system = (struct VR_IVRSystem_FnTable *)getInterface( name, &err );

	if( !vr.system || err != EVRInitError_VRInitError_None )
	{
		VR_Fail( "can't get IVRSystem", (int)err );
		return;
	}

	Q_snprintf( name, sizeof( name ), "FnTable:%s", IVRCompositor_Version );
	vr.compositor = (struct VR_IVRCompositor_FnTable *)getInterface( name, &err );

	if( !vr.compositor || err != EVRInitError_VRInitError_None )
	{
		VR_Fail( "can't get IVRCompositor", (int)err );
		return;
	}

	vr.compositor->SetTrackingSpace( ETrackingUniverseOrigin_TrackingUniverseStanding );

	for( eye = 0; eye < 2; eye++ )
	{
		float l, r, t, b;
		HmdMatrix34_t e;

		// raw projection: left/top are negative. top is negative = UP in OpenVR, so flip it
		vr.system->GetProjectionRaw( (EVREye)eye, &l, &r, &t, &b );
		vr.tan_frustum[eye][0] = l;
		vr.tan_frustum[eye][1] = r;
		vr.tan_frustum[eye][2] = -t;
		vr.tan_frustum[eye][3] = -b;

		e = vr.system->GetEyeToHeadTransform( (EVREye)eye );
		vr.eye_offset[eye][0] = e.m[0][3];
		vr.eye_offset[eye][1] = e.m[1][3];
		vr.eye_offset[eye][2] = e.m[2][3];
	}

	vr.initialized = true;
	gEngfuncs.Con_Printf( "VR: SteamVR initialized. Eye FOV tangents L: %.2f %.2f %.2f %.2f\n",
		vr.tan_frustum[0][0], vr.tan_frustum[0][1], vr.tan_frustum[0][2], vr.tan_frustum[0][3] );
}

void VR_NewFrame( void )
{
	vr.frame_done = false;
}

qboolean VR_BeginMainPass( const ref_viewpass_t *game )
{
	const TrackedDevicePose_t *hmd;
	const HmdMatrix34_t *m;
	float p[3], back[3];
	vec3_t fwd;

	if( !vr_enable.value || vr.frame_done )
		return false;

	if( !FBitSet( game->flags, RF_DRAW_WORLD ) || FBitSet( game->flags, RF_DRAW_OVERVIEW | RF_ONLY_CLIENTDRAW | RF_DRAW_CUBEMAP ))
		return false;

	if( !vr.initialized && !vr.failed )
		VR_Init();

	if( !vr.initialized )
		return false;

	// blocks until it is time to start rendering the next frame
	vr.compositor->WaitGetPoses( vr.poses, VR_MAX_DEVICES, NULL, 0 );
	hmd = &vr.poses[k_unTrackedDeviceIndex_Hmd];

	if( !hmd->bPoseIsValid )
		return false;

	m = &hmd->mDeviceToAbsoluteTracking;
	p[0] = m->m[0][3]; p[1] = m->m[1][3]; p[2] = m->m[2][3];
	VectorCopy( p, vr.head_pos );

	// head basis. OpenVR matrix columns are right, up, back
	{
		float right[3] = { m->m[0][0], m->m[1][0], m->m[2][0] };
		float up[3] = { m->m[0][1], m->m[1][1], m->m[2][1] };
		float negright[3];

		back[0] = m->m[0][2]; back[1] = m->m[1][2]; back[2] = m->m[2][2];
		negright[0] = -right[0]; negright[1] = -right[1]; negright[2] = -right[2];

		{
			float forward[3] = { -back[0], -back[1], -back[2] };
			VR_ConvDir( forward, vr.axis_fwd );
		}
		VR_ConvDir( negright, vr.axis_left );
		VR_ConvDir( up, vr.axis_up );
	}

	if( !vr.have_ref )
	{
		VR_ConvPos( p, vr.ref_pos );
		VectorCopy( vr.axis_fwd, fwd );
		vr.ref_yaw = RAD2DEG( atan2( fwd[1], fwd[0] ));
		vr.have_ref = true;
	}

	return true;
}

void VR_SetupEyeView( int eye, const ref_viewpass_t *game, ref_viewpass_t *out )
{
	const HmdMatrix34_t *m = &vr.poses[k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
	const float *t = vr.eye_offset[eye];
	float ep[3];
	vec3_t eyepos, offset, fwd, left, up;
	float d, xy, tan_x, tan_y;
	int i;

	*out = *game;

	// eye position in tracking space: head position + head rotation * eye offset
	for( i = 0; i < 3; i++ )
		ep[i] = m->m[i][3] + m->m[i][0] * t[0] + m->m[i][1] * t[1] + m->m[i][2] * t[2];

	VR_ConvPos( ep, eyepos );
	VectorSubtract( eyepos, vr.ref_pos, offset );

	// the mouse/keyboard yaw turns the whole rig; the head only adds to it
	d = game->viewangles[YAW] - vr.ref_yaw;

	VR_RotateZ( offset, d, offset );
	VR_RotateZ( vr.axis_fwd, d, fwd );
	VR_RotateZ( vr.axis_left, d, left );
	VR_RotateZ( vr.axis_up, d, up );

	VectorAdd( game->vieworigin, offset, out->vieworigin );

	xy = sqrt( fwd[0] * fwd[0] + fwd[1] * fwd[1] );
	out->viewangles[PITCH] = RAD2DEG( atan2( -fwd[2], xy ));
	out->viewangles[YAW] = RAD2DEG( atan2( fwd[1], fwd[0] ));
	out->viewangles[ROLL] = RAD2DEG( atan2( left[2], up[2] ));

	if( vr_flip_roll.value )
		out->viewangles[ROLL] = -out->viewangles[ROLL];

	// asymmetric frustum from the headset; the culling frustum gets a symmetric superset
	for( i = 0; i < 4; i++ )
		vr.cur_frustum[i] = vr.tan_frustum[eye][i];

	tan_x = Q_max( -vr.tan_frustum[eye][0], vr.tan_frustum[eye][1] );
	tan_y = Q_max( vr.tan_frustum[eye][2], -vr.tan_frustum[eye][3] );
	out->fov_x = 2.0f * RAD2DEG( atan( tan_x ));
	out->fov_y = 2.0f * RAD2DEG( atan( tan_y ));

	vr.in_eye_pass = true;
}

qboolean VR_GetEyeFrustum( float *left, float *right, float *up, float *down )
{
	if( !vr.in_eye_pass )
		return false;

	*left = vr.cur_frustum[0];
	*right = vr.cur_frustum[1];
	*up = vr.cur_frustum[2];
	*down = vr.cur_frustum[3];
	return true;
}

static void VR_EnsureTextures( int w, int h )
{
	int i;

	if( vr.tex[0] && vr.tex_w == w && vr.tex_h == h )
		return;

	if( vr.tex[0] )
		pglDeleteTextures( 2, vr.tex );

	pglGenTextures( 2, vr.tex );

	for( i = 0; i < 2; i++ )
	{
		pglBindTexture( GL_TEXTURE_2D, vr.tex[i] );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	}

	vr.tex_w = w;
	vr.tex_h = h;
}

static void VR_ForgetTextureBinding( void )
{
	// we bound textures behind the renderer's back, make it rebind on next use
	glState.currentTextures[glState.activeTMU] = -1;
	glState.currentTexturesIndex[glState.activeTMU] = -1;
}

void VR_EndEyePass( int eye )
{
	int w, h;

	vr.in_eye_pass = false;

	// the scene target (or the window) is still bound from R_SetupGL
	GL_GetSceneTargetSize( &w, &h );
	VR_EnsureTextures( w, h );

	pglBindTexture( GL_TEXTURE_2D, vr.tex[eye] );
	pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h );
	VR_ForgetTextureBinding();
}

void VR_FinishFrame( void )
{
	int eye;

	pglFlush();

	for( eye = 0; eye < 2; eye++ )
	{
		Texture_t tex;
		VRTextureBounds_t bounds;
		EVRCompositorError err;

		tex.handle = (void *)(uintptr_t)vr.tex[eye];
		tex.eType = ETextureType_TextureType_OpenGL;
		tex.eColorSpace = EColorSpace_ColorSpace_Gamma;

		bounds.uMin = 0.0f;
		bounds.uMax = 1.0f;
		bounds.vMin = 0.0f;
		bounds.vMax = 1.0f;

		err = vr.compositor->Submit( (EVREye)eye, &tex, &bounds, EVRSubmitFlags_Submit_Default );

		if( err != EVRCompositorError_VRCompositorError_None )
			gEngfuncs.Con_DPrintf( S_ERROR "VR: Submit failed for eye %d (code %d)\n", eye, (int)err );
	}

	pglFlush();
	vr.frame_done = true;
}
