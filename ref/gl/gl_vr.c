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
#include <ctype.h>

#ifndef PM_STUDIO_BOX
#define PM_STUDIO_BOX	0x00000002	// trace against boxes of studio models (from pm_defs.h)
#endif

#include <stdint.h>
#include <stdbool.h>

/*
=================
Minimal OpenVR C interface definitions.

Valve's openvr_capi.h does not compile as C with MSVC, so only the pieces used here are declared.
The function tables must match the layout of the "FnTable:" interfaces exactly (members are
listed in Valve's order, anything we do not call is a placeholder pointer).
Interface versions: IVRSystem_019, IVRCompositor_022 (both still served by current SteamVR).
=================
*/
#if defined( _WIN32 )
#define VR_FN __stdcall
#else
#define VR_FN
#endif

#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER		0x8D41
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8		0x88F0
#endif

#ifndef GL_VIEWPORT
#define GL_VIEWPORT		0x0BA2
#endif

#ifndef GL_COLOR_CLEAR_VALUE
#define GL_COLOR_CLEAR_VALUE	0x0C22
#endif

#define VR_MAX_DEVICES		64
#define VR_HMD_INDEX		0
#define VR_INIT_NONE		0
#define VR_APP_SCENE		1
#define VR_TRACKING_STANDING	1
#define VR_TEXTURE_OPENGL		1
#define VR_COLORSPACE_GAMMA		1
#define VR_SUBMIT_DEFAULT		0
#define VR_COMPOSITOR_OK		0

typedef struct { float m[3][4]; } HmdMatrix34_t;
typedef struct { float v[3]; } HmdVector3_t;

typedef struct
{
	HmdMatrix34_t	mDeviceToAbsoluteTracking;
	HmdVector3_t	vVelocity;
	HmdVector3_t	vAngularVelocity;
	int		eTrackingResult;
	bool		bPoseIsValid;
	bool		bDeviceIsConnected;
} TrackedDevicePose_t;

typedef struct
{
	void		*handle;
	int		eType;
	int		eColorSpace;
} Texture_t;

typedef struct
{
	float		uMin, vMin;
	float		uMax, vMax;
} VRTextureBounds_t;

struct VR_IVRSystem_FnTable
{
	void (VR_FN *GetRecommendedRenderTargetSize)( uint32_t *pnWidth, uint32_t *pnHeight );
	void *GetProjectionMatrix;	// not used
	void (VR_FN *GetProjectionRaw)( int eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom );
	void *ComputeDistortion;	// not used
	HmdMatrix34_t (VR_FN *GetEyeToHeadTransform)( int eEye );
};

struct VR_IVRCompositor_FnTable
{
	void (VR_FN *SetTrackingSpace)( int eOrigin );
	void *GetTrackingSpace;		// not used
	int (VR_FN *WaitGetPoses)( TrackedDevicePose_t *pRenderPoseArray, uint32_t unRenderPoseArrayCount,
		TrackedDevicePose_t *pGamePoseArray, uint32_t unGamePoseArrayCount );
	void *GetLastPoses;		// not used
	void *GetLastPoseForTrackedDeviceIndex;	// not used
	int (VR_FN *Submit)( int eEye, Texture_t *pTexture, VRTextureBounds_t *pBounds, int nSubmitFlags );
};

static const char *VR_SYSTEM_INTERFACE = "FnTable:IVRSystem_019";
static const char *VR_COMPOSITOR_INTERFACE = "FnTable:IVRCompositor_022";

typedef intptr_t (*pfnVR_InitInternal)( int *peError, int eType );
typedef intptr_t (*pfnVR_GetGenericInterface)( const char *pchInterfaceVersion, int *peError );
typedef bool (*pfnVR_IsHmdPresent)( void );

static CVAR_DEFINE_AUTO( vr_enable, "0", 0, "enable SteamVR (OpenVR) stereo rendering" );
static CVAR_DEFINE_AUTO( vr_worldscale, "39.37", 0, "game units per meter (GoldSrc: 1 unit = 1 inch)" );
static CVAR_DEFINE_AUTO( vr_flip_roll, "0", 0, "set to 1 if head roll is inverted" );
static CVAR_DEFINE_AUTO( vr_positional, "1", 0, "0 = ignore head movement (rotation only), useful for diagnosing warping" );
static CVAR_DEFINE_AUTO( vr_fov_scale, "1.0", 0, "diagnostic: scale the rendered field of view tangents (1.0 = exact)" );
static CVAR_DEFINE_AUTO( vr_crouch_height, "0.35", 0, "meters below your recentered height that count as physically crouching" );
static CVAR_DEFINE_AUTO( vr_physical_crouch, "0", 0, "1 = crouching in real life also crouches the character (the view then drops twice, see notes)" );
static CVAR_DEFINE_AUTO( vr_use_fbo, "0", 0, "1 = render each eye into its own buffer at SteamVR's recommended size (experimental). 0 = render in the window and copy" );
static CVAR_DEFINE_AUTO( vr_render_scale, "1.0", 0, "eye image size relative to what SteamVR recommends (needs a new map load or vid_restart to change)" );
static CVAR_DEFINE_AUTO( vr_finish, "1", 0, "wait for the GPU after submitting the eye images (matches Valve's sample)" );
static CVAR_DEFINE_AUTO( vr_ipd_scale, "1.0", 0, "diagnostic: scale the distance between the eyes (0 = mono, 1 = real)" );
static CVAR_DEFINE_AUTO( vr_testscene, "0", 0, "diagnostic: 1 = a world-fixed wireframe room over the game, 2 = only that room, 3 = a pure SteamVR-only test room that bypasses the game completely" );
static CVAR_DEFINE_AUTO( vr_hand_pitch, "60", 0, "tilt the controller's pointing direction: positive = down, negative = up (degrees). Adjust until the laser points where your hand naturally points" );
static CVAR_DEFINE_AUTO( vr_draw_hands, "1", 0, "draw placeholder controllers and the laser pointer" );
static CVAR_DEFINE_AUTO( vr_hand_weapon, "1", 0, "draw the weapon at your right hand instead of in front of your face" );
static CVAR_DEFINE_AUTO( vr_vm_grip_x, "13", 0, "where the right hand holds the weapon model (model axes): forward" );
static CVAR_DEFINE_AUTO( vr_vm_grip_y, "-4", 0, "where the right hand holds the weapon model: left (negative = right)" );
static CVAR_DEFINE_AUTO( vr_vm_grip_z, "-3", 0, "where the right hand holds the weapon model: up" );
static CVAR_DEFINE_AUTO( vr_vm_flip_roll, "0", 0, "set to 1 if the weapon is rolled the wrong way" );
static CVAR_DEFINE_AUTO( vr_vm_pitch, "0", 0, "extra rotation of the weapon model: pitch degrees" );
static CVAR_DEFINE_AUTO( vr_vm_yaw, "0", 0, "extra rotation of the weapon model: yaw degrees" );
static CVAR_DEFINE_AUTO( vr_vm_roll, "0", 0, "extra rotation of the weapon model: roll degrees" );
static CVAR_DEFINE_AUTO( vr_aim_trace, "1", 0, "1 = shots go to the exact point your hand ray hits, 0 = fixed distance (vr_aim_converge)" );
static CVAR_DEFINE_AUTO( vr_aim_converge, "96", 0, "only used when vr_aim_trace is 0: distance in units where the game's aim meets your hand's pointing direction" );
static CVAR_DEFINE_AUTO( vr_aim_debug, "0", 0, "print what the aim trace is doing about once a second" );
static CVAR_DEFINE_AUTO( vr_vm_hide_bones, "Bip01 L +left", 0, "parts of the weapon model attached to bones whose name contains one of these are not drawn (the left arm). Separate with +. vr_vm_dump_tex 2 lists the bones" );
static CVAR_DEFINE_AUTO( vr_vm_left_cut, "0", 0, "fallback for the left arm: hide model parts further than this many units to the left of your hand (0 = off)" );
static CVAR_DEFINE_AUTO( vr_vm_cap_radius, "1.8", 0, "size of the disc that closes the cut-off arm at the wrist (0 = none)" );
static CVAR_DEFINE_AUTO( vr_vm_cap_gray, "0.1", 0, "brightness of that disc, 0 = black, 1 = white" );
static CVAR_DEFINE_AUTO( vr_melee_models, "fist+phone+knife+switchblade+branch+nightstick+syringe+crowbar+pipe+axe+hammer+bat", 0, "weapon model names (part of the file name) that are swung by hand. Separate with +" );
static CVAR_DEFINE_AUTO( vr_melee_reach, "64", 0, "how far in front of your swinging hand the game looks for something to hit, in units" );
static CVAR_DEFINE_AUTO( vr_phone_light, "1", 0, "1 = a spotlight shines from the weapons listed in vr_light_models, where your hand points" );
static CVAR_DEFINE_AUTO( vr_light_models, "phone", 0, "weapon model names (part of the file name) that work as a flashlight. Separate with +" );
static CVAR_DEFINE_AUTO( vr_light_distance, "700", 0, "how far the flashlight reaches, in units" );
static CVAR_DEFINE_AUTO( vr_light_radius, "150", 0, "size of the lit area, in units" );
static CVAR_DEFINE_AUTO( vr_light_brightness, "200", 0, "brightness of the flashlight, 0 to 255" );
static CVAR_DEFINE_AUTO( vr_melee_freeze, "1", 0, "1 = melee weapons hold their first animation frame, so only your real arm swings" );
static CVAR_DEFINE_AUTO( vr_vm_scale, "0.5", 0, "size of the weapon model in your hand (1.0 = original)" );
static CVAR_DEFINE_AUTO( vr_vm_hide_tex, "sleeve", 0, "texture name fragments of weapon model parts to hide completely. Separate with + (a comma does not work in the console)" );
static CVAR_DEFINE_AUTO( vr_vm_clip_tex, "glove+hand+fingers+tex1+tex2", 0, "texture name fragments of weapon model parts (the arm) that are cut off behind the wrist. Separate with +" );
static CVAR_DEFINE_AUTO( vr_vm_arm_cut, "2", 0, "where the arm is cut off, in units behind your hand (bigger = more arm visible)" );
static CVAR_DEFINE_AUTO( vr_vm_dump_tex, "0", 0, "1 = print the texture name of every weapon mesh once" );
static qboolean vr_dump_done;
static CVAR_DEFINE_AUTO( vr_debug, "0", 0, "print VR diagnostics about once a second" );

// diagnostic: how much the game moves the camera by itself (view sway, kicks, smoothing) over one second
static float vr_cam_min[6], vr_cam_max[6];
static qboolean vr_cam_have;

static struct
{
	pfnVR_GetGenericInterface	get_interface;
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
	float		eye_rot[2][3][3];	// eye orientation relative to the head (identity on most headsets)
	float		eye_rot_deg[2];	// how far each eye is rotated from the head, degrees (diagnostic)
	int		dbg_viewport[2][4];
	uint32_t	rec_w, rec_h;
	int		err_prints;

	vec3_t		ref_pos;	// head position when recentered (game axes, units)
	float		ref_head_ovr[3];	// head position when recentered (OpenVR axes, meters)
	int		debug_frames;
	int		aim_debug_frames;
	float		ref_yaw;	// head yaw when recentered (degrees)

	vec3_t		head_pos;	// current head position, OpenVR axes, meters
	vec3_t		axis_fwd, axis_left, axis_up;	// current head basis in game axes (unrotated)

	float		cur_frustum[4];

	vec3_t		game_origin;	// game eye position of the current frame
	vec3_t		test_origin;	// where the diagnostic room is anchored
	qboolean	test_anchored;

	GLuint		tex[2];
	int		tex_w, tex_h;

	qboolean	use_fbo;	// eyes are rendered into their own buffers (otherwise: copied out of the window)
	qboolean	fbo_failed;
	qboolean	have_buffers;
	GLuint		eye_fbo[2], eye_tex[2], eye_depth[2];
	int		vm_asked, vm_applied;
	int		eye_w, eye_h;
	int		cur_eye;

	// controllers in game space, plus the pointing ray in tracking space (for the UI laser)
	struct
	{
		qboolean	valid;
		vec3_t		pos, fwd, left, up;
		float		origin_t[3], dir_t[3];
	} hand[2];		// 0 = left, 1 = right

	qboolean	ui_hit;
	float		ui_dist;	// meters
} vr;

static void VR_EnsureEyeTargets( void );
static void VR_UpdateWeaponProfile( void );
static void VR_WeaponSave_f( void );

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
	gEngfuncs.Cvar_RegisterVariable( &vr_positional );
	gEngfuncs.Cvar_RegisterVariable( &vr_fov_scale );
	gEngfuncs.Cvar_RegisterVariable( &vr_debug );
	gEngfuncs.Cvar_RegisterVariable( &vr_hand_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_draw_hands );
	gEngfuncs.Cvar_RegisterVariable( &vr_hand_weapon );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_grip_x );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_grip_y );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_grip_z );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_flip_roll );
	gEngfuncs.Cvar_RegisterVariable( &vr_aim_converge );
	gEngfuncs.Cvar_RegisterVariable( &vr_aim_debug );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_hide_bones );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_left_cut );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_cap_radius );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_cap_gray );
	gEngfuncs.Cvar_RegisterVariable( &vr_melee_models );
	gEngfuncs.Cvar_RegisterVariable( &vr_melee_reach );
	gEngfuncs.Cvar_RegisterVariable( &vr_melee_freeze );
	gEngfuncs.Cvar_RegisterVariable( &vr_phone_light );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_models );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_distance );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_radius );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_brightness );
	gEngfuncs.Cvar_RegisterVariable( &vr_aim_trace );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_yaw );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_roll );
	gEngfuncs.Cvar_RegisterVariable( &vr_testscene );
	gEngfuncs.Cvar_RegisterVariable( &vr_crouch_height );
	gEngfuncs.Cvar_RegisterVariable( &vr_physical_crouch );
	gEngfuncs.Cvar_RegisterVariable( &vr_finish );
	gEngfuncs.Cvar_RegisterVariable( &vr_render_scale );
	gEngfuncs.Cvar_RegisterVariable( &vr_use_fbo );
	gEngfuncs.Cvar_RegisterVariable( &vr_ipd_scale );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_scale );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_hide_tex );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_clip_tex );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_arm_cut );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_dump_tex );
	VR_InputInitCvars();
	VR_UIInitCvars();
	gEngfuncs.Cmd_AddCommand( "vr_weapon_save", VR_WeaponSave_f, "save the current weapon position/size settings for the weapon you are holding" );
	gEngfuncs.Cmd_AddCommand( "vr_recenter", VR_Recenter_f, "recenter the VR view on the current head position and direction" );
}

static void VR_Init( void )
{
	pfnVR_InitInternal initInternal;
	pfnVR_GetGenericInterface getInterface;
	pfnVR_IsHmdPresent isHmdPresent;
	int err = VR_INIT_NONE;
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

	vr.get_interface = getInterface;

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

	initInternal( &err, VR_APP_SCENE );

	if( err != VR_INIT_NONE )
	{
		VR_Fail( "VR_InitInternal failed", (int)err );
		return;
	}

	vr.system = (struct VR_IVRSystem_FnTable *)getInterface( VR_SYSTEM_INTERFACE, &err );

	if( !vr.system || err != VR_INIT_NONE )
	{
		VR_Fail( "can't get IVRSystem", (int)err );
		return;
	}

	vr.compositor = (struct VR_IVRCompositor_FnTable *)getInterface( VR_COMPOSITOR_INTERFACE, &err );

	if( !vr.compositor || err != VR_INIT_NONE )
	{
		VR_Fail( "can't get IVRCompositor", (int)err );
		return;
	}

	vr.compositor->SetTrackingSpace( VR_TRACKING_STANDING );

	for( eye = 0; eye < 2; eye++ )
	{
		float l, r, t, b;
		HmdMatrix34_t e;

		// raw projection: left/top are negative. top is negative = UP in OpenVR, so flip it
		vr.system->GetProjectionRaw( eye, &l, &r, &t, &b );
		vr.tan_frustum[eye][0] = l;
		vr.tan_frustum[eye][1] = r;
		vr.tan_frustum[eye][2] = -t;
		vr.tan_frustum[eye][3] = -b;

		e = vr.system->GetEyeToHeadTransform( eye );
		vr.eye_offset[eye][0] = e.m[0][3];
		vr.eye_offset[eye][1] = e.m[1][3];
		vr.eye_offset[eye][2] = e.m[2][3];
	}

	vr.initialized = true;

	// the game's view bobbing moves the camera, which is nauseating when the camera is your head
	gEngfuncs.Cvar_SetValue( "cl_bob", 0.0f );
	gEngfuncs.Con_Printf( "VR: SteamVR initialized. Eye FOV tangents L: %.2f %.2f %.2f %.2f\n",
		vr.tan_frustum[0][0], vr.tan_frustum[0][1], vr.tan_frustum[0][2], vr.tan_frustum[0][3] );
}

void VR_NewFrame( void )
{
	vr.frame_done = false;
}

/*
=================
SteamVR can change the field of view and the eye transform while running (IPD changes, render
resolution changes, headset finishing its start-up). Valve's sample re-reads them every frame, so do we.
=================
*/
static void VR_UpdateEyeParams( void )
{
	int eye, i, j;

	for( eye = 0; eye < 2; eye++ )
	{
		float l, r, t, b, trace;
		HmdMatrix34_t e;

		vr.system->GetProjectionRaw( eye, &l, &r, &t, &b );
		vr.tan_frustum[eye][0] = l;
		vr.tan_frustum[eye][1] = r;
		vr.tan_frustum[eye][2] = -t;
		vr.tan_frustum[eye][3] = -b;

		e = vr.system->GetEyeToHeadTransform( eye );

		for( i = 0; i < 3; i++ )
		{
			vr.eye_offset[eye][i] = e.m[i][3];

			for( j = 0; j < 3; j++ )
				vr.eye_rot[eye][i][j] = e.m[i][j];
		}

		trace = e.m[0][0] + e.m[1][1] + e.m[2][2];
		trace = bound( -1.0f, ( trace - 1.0f ) * 0.5f, 1.0f );
		vr.eye_rot_deg[eye] = RAD2DEG( acos( trace ));
	}
}

/*
=================
Controllers: where they are in the game world (same mapping as the eyes), and where they point.
=================
*/
static void VR_UpdateHands( const ref_viewpass_t *game )
{
	float d = game->viewangles[YAW] - vr.ref_yaw;
	float th = DEG2RAD( vr_hand_pitch.value ), c = cos( th ), s = sin( th );
	int h, i;

	for( h = 0; h < 2; h++ )
	{
		uint32_t dev = VR_InputGetDevice( h );
		const HmdMatrix34_t *m;
		float p[3], R[3], U[3], F[3], F2[3], U2[3], negR[3];
		vec3_t tmp;

		vr.hand[h].valid = false;

		if( dev >= VR_MAX_DEVICES || !vr.poses[dev].bPoseIsValid )
			continue;

		m = &vr.poses[dev].mDeviceToAbsoluteTracking;
		p[0] = m->m[0][3]; p[1] = m->m[1][3]; p[2] = m->m[2][3];
		R[0] = m->m[0][0]; R[1] = m->m[1][0]; R[2] = m->m[2][0];
		U[0] = m->m[0][1]; U[1] = m->m[1][1]; U[2] = m->m[2][1];
		F[0] = -m->m[0][2]; F[1] = -m->m[1][2]; F[2] = -m->m[2][2];

		// tilt the pointing direction about the controller's own right axis
		for( i = 0; i < 3; i++ )
		{
			F2[i] = c * F[i] - s * U[i];
			U2[i] = c * U[i] + s * F[i];
			negR[i] = -R[i];
			vr.hand[h].origin_t[i] = p[i];
			vr.hand[h].dir_t[i] = F2[i];
		}

		VR_ConvPos( p, tmp );
		VectorSubtract( tmp, vr.ref_pos, tmp );
		VR_RotateZ( tmp, d, tmp );
		VectorAdd( game->vieworigin, tmp, vr.hand[h].pos );

		VR_ConvDir( F2, tmp );
		VR_RotateZ( tmp, d, vr.hand[h].fwd );
		VR_ConvDir( negR, tmp );
		VR_RotateZ( tmp, d, vr.hand[h].left );
		VR_ConvDir( U2, tmp );
		VR_RotateZ( tmp, d, vr.hand[h].up );

		vr.hand[h].valid = true;
	}
}

static vec3_t vr_aim_target;
static qboolean vr_aim_hit;	// the hand ray hit something, vr_aim_target is that point
static qboolean vr_vm_light;	// the weapon in hand is a flashlight (the phone)
static qboolean vr_vm_melee;	// the weapon in hand is swung by hand
static qboolean vr_dump_bones_done;
static int vr_dbg_ent = -1;
static float vr_dbg_fraction;
static qboolean vr_dbg_solid;

#ifndef PM_STUDIO_BOX
#define PM_STUDIO_BOX	0x00000002
#endif

// pitch and yaw (relative to base_yaw) that look from one point to another
static void VR_AimAngles( const vec3_t from, const vec3_t to, float base_yaw, float *pitch, float *yaw )
{
	vec3_t d;
	float xy;

	VectorSubtract( to, from, d );
	xy = sqrt( d[0] * d[0] + d[1] * d[1] );
	*pitch = RAD2DEG( atan2( -d[2], xy ));
	*yaw = RAD2DEG( atan2( d[1], d[0] )) - base_yaw;

	while( *yaw > 180.0f ) *yaw -= 360.0f;
	while( *yaw < -180.0f ) *yaw += 360.0f;
}

// where does a ray from start along dir hit the world, up to dist? out is the hit point, or the end of the ray
static qboolean VR_TraceRay( const vec3_t start, const vec3_t dir, float dist, const vec3_t eye, vec3_t out )
{
	pmtrace_t hit;
	vec3_t end, to_hit;

	VectorMA( start, dist, dir, end );
	hit = gEngfuncs.CL_TraceLine( (float *)start, end, PM_STUDIO_BOX );

	vr_dbg_ent = hit.ent;
	vr_dbg_fraction = hit.fraction;
	vr_dbg_solid = hit.startsolid;

	VectorCopy( end, out );

	if( hit.startsolid || hit.fraction >= 1.0f )
		return false;

	// a hit right at the eye would make the aim jump around
	VectorSubtract( hit.endpos, eye, to_hit );

	if( DotProduct( to_hit, to_hit ) < 16.0f * 16.0f )
		return false;

	VectorCopy( hit.endpos, out );
	return true;
}

// the game aims, uses and shoots along angles we send it. The server fires from the player's own eye,
// so work out the angles from there to the point the hand ray hits (or far along the ray if it hits nothing).
// Also: how fast the hand swings and where, for melee.
static void VR_PublishAim( const ref_viewpass_t *game )
{
	float d = game->viewangles[YAW] - vr.ref_yaw;
	vec3_t eye, target, tmp, swing_dir, swing_target;
	float pitch, yaw, speed = 0.0f;
	uint32_t dev;

	vr_aim_hit = false;

	if( !vr.hand[1].valid )
	{
		gEngfuncs.Cvar_SetValue( "vr_in_hand_valid", 0.0f );
		gEngfuncs.Cvar_SetValue( "vr_in_hand_speed", 0.0f );
		return;
	}

	VectorCopy( game->vieworigin, eye );

	if( vr_aim_trace.value )
	{
		vr_aim_hit = VR_TraceRay( vr.hand[1].pos, vr.hand[1].fwd, 8192.0f, eye, target );
	}
	else
	{
		VectorScale( vr.hand[1].fwd, vr_aim_converge.value, tmp );
		VectorAdd( vr.hand[1].pos, tmp, target );
	}

	VectorCopy( target, vr_aim_target );
	VR_AimAngles( eye, target, game->viewangles[YAW], &pitch, &yaw );

	gEngfuncs.Cvar_SetValue( "vr_in_hand_pitch", pitch );
	gEngfuncs.Cvar_SetValue( "vr_in_hand_yaw", yaw );
	gEngfuncs.Cvar_SetValue( "vr_in_hand_valid", 1.0f );

	// for the flashlight, which lives in the engine
	gEngfuncs.Cvar_SetValue( "vr_in_ray_x", vr.hand[1].pos[0] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_y", vr.hand[1].pos[1] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_z", vr.hand[1].pos[2] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_dx", vr.hand[1].fwd[0] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_dy", vr.hand[1].fwd[1] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_dz", vr.hand[1].fwd[2] );

	// swing: hand velocity relative to the head, so walking around does not count
	dev = VR_InputGetDevice( 1 );

	if( dev < VR_MAX_DEVICES && vr.poses[dev].bPoseIsValid )
	{
		const float *hv = vr.poses[dev].vVelocity.v;
		const float *kv = vr.poses[VR_HMD_INDEX].vVelocity.v;
		float rel[3] = { hv[0] - kv[0], hv[1] - kv[1], hv[2] - kv[2] };
		float spitch, syaw;

		speed = sqrt( rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] );

		if( speed > 0.3f )
		{
			VR_ConvDir( rel, tmp );
			VectorScale( tmp, 1.0f / speed, tmp );
			VR_RotateZ( tmp, d, swing_dir );

			VR_TraceRay( vr.hand[1].pos, swing_dir, vr_melee_reach.value, eye, swing_target );
			VR_AimAngles( eye, swing_target, game->viewangles[YAW], &spitch, &syaw );

			gEngfuncs.Cvar_SetValue( "vr_in_swing_pitch", spitch );
			gEngfuncs.Cvar_SetValue( "vr_in_swing_yaw", syaw );
		}
	}

	gEngfuncs.Cvar_SetValue( "vr_in_hand_speed", speed );

	if( vr_aim_debug.value && ++vr.aim_debug_frames >= 72 )
	{
		vr.aim_debug_frames = 0;
		gEngfuncs.Con_Printf( "VR aim: hit %d, fraction %.3f, startsolid %d, ent %d, pitch %.1f, yaw %.1f, hand speed %.2f m/s, melee %d\n",
			vr_aim_hit, vr_dbg_fraction, vr_dbg_solid, vr_dbg_ent, pitch, yaw, speed, vr_vm_melee );
	}
}

// does the right hand point at the UI panel? if so, tell the engine where, so it can move the cursor
static void VR_PublishUI( void )
{
	float u = 0.0f, v = 0.0f, dist = 0.0f;

	vr.ui_hit = vr.hand[1].valid && VR_UIRayHit( vr.hand[1].origin_t, vr.hand[1].dir_t, &u, &v, &dist );
	vr.ui_dist = dist;
	VR_UISetPointer( vr.ui_hit, u, v );

	gEngfuncs.Cvar_SetValue( "vr_in_ui_hit", vr.ui_hit ? 1.0f : 0.0f );

	if( vr.ui_hit )
	{
		gEngfuncs.Cvar_SetValue( "vr_in_ui_x", u );
		gEngfuncs.Cvar_SetValue( "vr_in_ui_y", 1.0f - v );	// window coordinates start at the top
	}
}

/*
=================
The weapon model (viewmodel) is drawn where the right hand is. Called by the renderer while it draws an eye.
The model's origin is around the player's eye, so shift it by where the hand grips the model.
=================
*/
qboolean VR_GetViewModelPose( vec3_t origin, vec3_t angles )
{
	vec3_t shift;
	float xy, pitch, yaw, roll;

	vr.vm_asked++;
	VR_UpdateWeaponProfile();

	if( !vr.in_eye_pass || !vr_hand_weapon.value || !vr.hand[1].valid )
		return false;

	vr.vm_applied++;

	VectorScale( vr.hand[1].fwd, vr_vm_grip_x.value, shift );
	origin[0] = vr.hand[1].pos[0] - shift[0];
	origin[1] = vr.hand[1].pos[1] - shift[1];
	origin[2] = vr.hand[1].pos[2] - shift[2];

	VectorScale( vr.hand[1].left, vr_vm_grip_y.value, shift );
	VectorSubtract( origin, shift, origin );
	VectorScale( vr.hand[1].up, vr_vm_grip_z.value, shift );
	VectorSubtract( origin, shift, origin );

	xy = sqrt( vr.hand[1].fwd[0] * vr.hand[1].fwd[0] + vr.hand[1].fwd[1] * vr.hand[1].fwd[1] );
	pitch = RAD2DEG( atan2( -vr.hand[1].fwd[2], xy ));
	yaw = RAD2DEG( atan2( vr.hand[1].fwd[1], vr.hand[1].fwd[0] ));
	roll = RAD2DEG( atan2( vr.hand[1].left[2], vr.hand[1].up[2] ));

	if( vr_vm_flip_roll.value )
		roll = -roll;

	// the game stores the viewmodel's pitch negated, the studio renderer undoes that
	angles[PITCH] = -( pitch + vr_vm_pitch.value );
	angles[YAW] = yaw + vr_vm_yaw.value;
	angles[ROLL] = roll + vr_vm_roll.value;
	return true;
}

float VR_GetViewModelScale( void )
{
	return bound( 0.1f, vr_vm_scale.value, 4.0f );
}

static qboolean VR_ContainsNoCase( const char *s, const char *frag )
{
	size_t n = strlen( frag ), i;

	for( ; *s; s++ )
	{
		for( i = 0; i < n && s[i]; i++ )
		{
			if( tolower( (unsigned char)s[i] ) != tolower( (unsigned char)frag[i] ))
				break;
		}

		if( i == n )
			return true;
	}

	return false;
}

static qboolean VR_ListMatches( const char *list, const char *name, qboolean split_on_space )
{
	const char *p = list;

#define VR_IS_SEP( c ) ( (c) == '+' || (c) == '|' || (c) == ',' || (c) == ';' || ( split_on_space && (c) == ' ' ))

	while( p && *p )
	{
		char frag[64];
		int n = 0;

		while( *p && VR_IS_SEP( *p )) p++;
		while( *p && !VR_IS_SEP( *p ) && n < 63 ) frag[n++] = *p++;
		frag[n] = 0;

		if( n && VR_ContainsNoCase( name, frag ))
			return true;
	}

#undef VR_IS_SEP
	return false;
}

// called by the studio renderer for every mesh of the weapon model while it is drawn at the hand
int VR_ViewModelMeshMode( int mesh, const char *texname )
{
	if( vr_vm_dump_tex.value )
	{
		gEngfuncs.Con_Printf( "VR weapon '%s' mesh %d: texture '%s'\n",
			( tr.viewent && tr.viewent->model ) ? tr.viewent->model->name : "?", mesh, texname );
		vr_dump_done = true;
	}

	if( VR_ListMatches( vr_vm_hide_tex.string, texname, true ))
		return 1;

	if( VR_ListMatches( vr_vm_clip_tex.string, texname, true ))
		return 2;

	return 0;
}

// everything in front of a plane just behind the hand is drawn, the arm behind it is cut off
void VR_ViewModelClip( qboolean on )
{
	GLdouble eq[4];
	float view[16];
	vec3_t n, c;

	if( !on || !vr.hand[1].valid )
	{
		pglDisable( GL_CLIP_PLANE0 );
		return;
	}

	VectorCopy( vr.hand[1].fwd, n );
	VectorMA( vr.hand[1].pos, -vr_vm_arm_cut.value, n, c );

	eq[0] = n[0];
	eq[1] = n[1];
	eq[2] = n[2];
	eq[3] = -DotProduct( n, c );

	Matrix4x4_ToArrayFloatGL( RI.worldviewMatrix, view );
	pglMatrixMode( GL_MODELVIEW );
	pglPushMatrix();
	pglLoadMatrixf( view );
	pglClipPlane( GL_CLIP_PLANE0, eq );
	pglEnable( GL_CLIP_PLANE0 );
	pglPopMatrix();
}

// bones: the left arm is found by bone name. vr_vm_dump_tex 2 prints them
qboolean VR_WantBoneDump( void )
{
	if( vr_vm_dump_tex.value < 2.0f || vr_dump_bones_done )
		return false;

	vr_dump_bones_done = true;
	vr_dump_done = true;
	return true;
}

qboolean VR_ViewModelBoneHidden( const char *bonename )
{
	return VR_ListMatches( vr_vm_hide_bones.string, bonename, false );
}

// fallback for the left arm when bone names don't tell: anything far enough to the left of the hand
qboolean VR_ViewModelVertexHidden( const float *pos )
{
	vec3_t rel;

	if( vr_vm_left_cut.value <= 0.0f || !vr.hand[1].valid )
		return false;

	VectorSubtract( pos, vr.hand[1].pos, rel );
	return DotProduct( rel, vr.hand[1].left ) > vr_vm_left_cut.value;
}

qboolean VR_ViewModelFrozen( void )
{
	return vr_vm_melee && vr_melee_freeze.value;
}

// a dark disc that closes the arm where it is cut off, so you can't look into it
void VR_DrawArmCap( void )
{
	float proj[16], view[16], r = vr_vm_cap_radius.value, g = vr_vm_cap_gray.value;
	vec3_t c;
	int i, unit, saved_tmu;

	if( r <= 0.0f || !vr_vm_clip_tex.string[0] || !vr.hand[1].valid )
		return;

	VectorMA( vr.hand[1].pos, -vr_vm_arm_cut.value, vr.hand[1].fwd, c );

	Matrix4x4_ToArrayFloatGL( RI.projectionMatrix, proj );
	Matrix4x4_ToArrayFloatGL( RI.worldviewMatrix, view );

	saved_tmu = glState.activeTMU;
	pglPushAttrib( GL_ALL_ATTRIB_BITS );

	for( unit = GL_MaxTextureUnits() - 1; unit >= 0; unit-- )
	{
		GL_SelectTexture( unit );
		pglDisable( GL_TEXTURE_2D );
	}

	pglDisable( GL_BLEND );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_CULL_FACE );
	pglDisable( GL_FOG );
	pglDisable( GL_CLIP_PLANE0 );
	pglEnable( GL_DEPTH_TEST );
	pglDepthMask( GL_TRUE );

	pglMatrixMode( GL_PROJECTION );
	pglPushMatrix();
	pglLoadMatrixf( proj );
	pglMatrixMode( GL_MODELVIEW );
	pglPushMatrix();
	pglLoadMatrixf( view );

	pglColor3f( g, g, g );
	pglBegin( GL_TRIANGLE_FAN );
	pglVertex3f( c[0], c[1], c[2] );

	for( i = 0; i <= 16; i++ )
	{
		float a = i * ( 2.0f * M_PI_F / 16.0f ), ca = cos( a ) * r, sa = sin( a ) * r;

		pglVertex3f( c[0] + vr.hand[1].left[0] * ca + vr.hand[1].up[0] * sa,
			c[1] + vr.hand[1].left[1] * ca + vr.hand[1].up[1] * sa,
			c[2] + vr.hand[1].left[2] * ca + vr.hand[1].up[2] * sa );
	}

	pglEnd();

	pglMatrixMode( GL_MODELVIEW );
	pglPopMatrix();
	pglMatrixMode( GL_PROJECTION );
	pglPopMatrix();
	pglMatrixMode( GL_MODELVIEW );

	pglPopAttrib();
	GL_SelectTexture( saved_tmu );
}

/*
=================
Per-weapon settings. The values of the weapon cvars when the first weapon shows up are the defaults.
When you switch weapons they are restored, then vr_weapon_<model>.cfg is run if it exists.
"vr_weapon_save" writes that file for the weapon you are holding.
=================
*/
static convar_t *vr_tune_vars[] =
{
	&vr_vm_grip_x, &vr_vm_grip_y, &vr_vm_grip_z, &vr_vm_pitch, &vr_vm_yaw,
	&vr_vm_roll, &vr_vm_flip_roll, &vr_vm_scale, &vr_hand_pitch, &vr_vm_arm_cut, &vr_vm_left_cut
};

#define VR_TUNE_COUNT ( (int)( sizeof( vr_tune_vars ) / sizeof( vr_tune_vars[0] )))

static float vr_tune_base[16];
static qboolean vr_tune_have_base;
static char vr_tune_model[64];

// "models/weapons/glock/v_glock.mdl" -> "v_glock"
static void VR_ModelBaseName( const char *modelname, char *out, size_t size )
{
	const char *base = modelname, *p;
	size_t n = 0;

	for( p = modelname; *p; p++ )
	{
		if( *p == '/' || *p == '\\' )
			base = p + 1;
	}

	while( *base && *base != '.' && n < size - 1 )
		out[n++] = *base++;

	out[n] = 0;
}

static void VR_WeaponFileName( const char *modelname, char *out, size_t size )
{
	char name[48];

	VR_ModelBaseName( modelname, name, sizeof( name ));
	Q_snprintf( out, size, "vr_weapon_%s.cfg", name );
}

static void VR_UpdateWeaponProfile( void )
{
	const char *model;
	char file[96], cmd[128];
	int i;

	if( !tr.viewent || !tr.viewent->model )
		return;

	model = tr.viewent->model->name;

	if( !vr_tune_have_base )
	{
		for( i = 0; i < VR_TUNE_COUNT; i++ )
			vr_tune_base[i] = vr_tune_vars[i]->value;

		vr_tune_have_base = true;
	}
	else if( !Q_strcmp( model, vr_tune_model ))
	{
		return;
	}
	else
	{
		for( i = 0; i < VR_TUNE_COUNT; i++ )
			gEngfuncs.Cvar_SetValue( vr_tune_vars[i]->name, vr_tune_base[i] );
	}

	Q_strncpy( vr_tune_model, model, sizeof( vr_tune_model ));

	{
		char base[48];

		VR_ModelBaseName( model, base, sizeof( base ));
		vr_vm_melee = VR_ListMatches( vr_melee_models.string, base, true );
		vr_vm_light = VR_ListMatches( vr_light_models.string, base, true );
		gEngfuncs.Cvar_SetValue( "vr_in_melee", vr_vm_melee ? 1.0f : 0.0f );
	}

	VR_WeaponFileName( model, file, sizeof( file ));

	if( gEngfuncs.fsapi->FileExists( file, false ))
	{
		Q_snprintf( cmd, sizeof( cmd ), "exec %s\n", file );
		gEngfuncs.Cbuf_AddText( cmd );
	}
}

static void VR_WeaponSave_f( void )
{
	char text[768], file[96];
	int i, len = 0;

	if( !vr_tune_model[0] )
	{
		gEngfuncs.Con_Printf( "VR: hold a weapon first\n" );
		return;
	}

	VR_WeaponFileName( vr_tune_model, file, sizeof( file ));

	for( i = 0; i < VR_TUNE_COUNT; i++ )
		len += Q_snprintf( text + len, sizeof( text ) - len, "%s %g\n", vr_tune_vars[i]->name, vr_tune_vars[i]->value );

	if( gEngfuncs.fsapi->WriteFile( file, text, len ))
		gEngfuncs.Con_Printf( "VR: saved settings for %s to %s\n", vr_tune_model, file );
	else gEngfuncs.Con_Printf( S_ERROR "VR: can't write %s\n", file );
}

static void VR_TrackCamera( const ref_viewpass_t *game )
{
	float v[6];
	int i;

	for( i = 0; i < 3; i++ )
	{
		v[i] = game->viewangles[i];
		v[3 + i] = game->vieworigin[i];
	}

	for( i = 0; i < 6; i++ )
	{
		if( !vr_cam_have || v[i] < vr_cam_min[i] ) vr_cam_min[i] = v[i];
		if( !vr_cam_have || v[i] > vr_cam_max[i] ) vr_cam_max[i] = v[i];
	}

	vr_cam_have = true;
}

#define VR_LIGHT_KEY	0x5652	// so we always reuse the same dynamic light

static dlight_t *VR_AllocLight( void )
{
	int i, freeslot = -1;

	for( i = 0; i < MAX_DLIGHTS; i++ )
	{
		if( gp_dlights[i].key == VR_LIGHT_KEY )
			return &gp_dlights[i];

		if( freeslot < 0 && ( gp_dlights[i].die < gp_cl->time || !gp_dlights[i].radius ))
			freeslot = i;
	}

	if( freeslot < 0 )
		return NULL;

	memset( &gp_dlights[freeslot], 0, sizeof( dlight_t ));
	gp_dlights[freeslot].key = VR_LIGHT_KEY;
	return &gp_dlights[freeslot];
}

// a spotlight pool where the hand points, lit with the engine's dynamic light so both eyes see it
static void VR_UpdateHandLight( const ref_viewpass_t *game )
{
	vec3_t end, org, to;
	dlight_t *dl;
	float dist, bright;
	qboolean hit;

	if( !vr_phone_light.value || !vr_vm_light || !vr.hand[1].valid )
		return;

	hit = VR_TraceRay( vr.hand[1].pos, vr.hand[1].fwd, vr_light_distance.value, game->vieworigin, end );

	// a little in front of the surface, so the light does not sit inside it
	if( hit ) VectorMA( end, -8.0f, vr.hand[1].fwd, org );
	else VectorCopy( end, org );

	VectorSubtract( org, vr.hand[1].pos, to );
	dist = VectorLength( to );
	bright = vr_light_brightness.value / ( 1.0f + ( dist / 350.0f ) * ( dist / 350.0f ));

	if( !( dl = VR_AllocLight( )))
		return;

	VectorCopy( org, dl->origin );
	dl->radius = vr_light_radius.value;
	dl->color.r = (uint8_t)bound( 40.0f, bright, 255.0f );
	dl->color.g = (uint8_t)bound( 40.0f, bright * 0.96f, 255.0f );
	dl->color.b = (uint8_t)bound( 40.0f, bright * 0.85f, 255.0f );
	dl->die = gp_cl->time + 0.05f;
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
	{
		int wait_err = vr.compositor->WaitGetPoses( vr.poses, VR_MAX_DEVICES, NULL, 0 );

		if( wait_err != VR_COMPOSITOR_OK && vr.err_prints++ < 10 )
			gEngfuncs.Con_Printf( S_ERROR "VR: WaitGetPoses failed (code %d)\n", wait_err );
	}

	VR_UpdateEyeParams();
	hmd = &vr.poses[VR_HMD_INDEX];

	if( !hmd->bPoseIsValid )
		return false;

	VR_EnsureEyeTargets();

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
		VectorCopy( p, vr.ref_head_ovr );
		VR_ConvPos( p, vr.ref_pos );
		VectorCopy( vr.axis_fwd, fwd );
		vr.ref_yaw = RAD2DEG( atan2( fwd[1], fwd[0] ));
		vr.have_ref = true;
	}

	{
		float rel = RAD2DEG( atan2( vr.axis_fwd[1], vr.axis_fwd[0] )) - vr.ref_yaw;
		float xy = sqrt( vr.axis_fwd[0] * vr.axis_fwd[0] + vr.axis_fwd[1] * vr.axis_fwd[1] );
		float pitch = RAD2DEG( atan2( -vr.axis_fwd[2], xy ));
		qboolean crouch = vr_physical_crouch.value && ( vr.head_pos[1] < vr.ref_head_ovr[1] - vr_crouch_height.value );

		while( rel > 180.0f ) rel -= 360.0f;
		while( rel < -180.0f ) rel += 360.0f;

		VR_InputPoll( rel, pitch, crouch );
	}

	if( vr_debug.value )
		VR_TrackCamera( game );

	VR_UpdateHands( game );
	VR_PublishAim( game );
	VR_UpdateHandLight( game );
	VR_PublishUI();

	return true;
}

void *VR_GetSystemTable( void )
{
	return vr.system;
}

qboolean VR_IsActive( void )
{
	return vr.initialized && vr_enable.value;
}

void *VR_GetInterface( const char *fntable_name )
{
	int err = 0;

	if( !vr.get_interface )
		return NULL;

	return (void *)vr.get_interface( fntable_name, &err );
}

void VR_GetHeadMatrix( float m[3][4] )
{
	memcpy( m, vr.poses[VR_HMD_INDEX].mDeviceToAbsoluteTracking.m, sizeof( float ) * 12 );
}

qboolean VR_GetEyeTarget( GLuint *fbo, int *width, int *height )
{
	if( !vr.in_eye_pass || !vr.use_fbo )
		return false;

	if( fbo ) *fbo = vr.eye_fbo[vr.cur_eye & 1];
	if( width ) *width = vr.eye_w;
	if( height ) *height = vr.eye_h;
	return true;
}

static void VR_FreeEyeBuffers( void )
{
	if( !vr.eye_fbo[0] )
		return;

	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglDeleteFramebuffers( 2, vr.eye_fbo );
	pglDeleteRenderbuffers( 2, vr.eye_depth );
	pglDeleteTextures( 2, vr.eye_tex );
	memset( vr.eye_fbo, 0, sizeof( vr.eye_fbo ));
	memset( vr.eye_depth, 0, sizeof( vr.eye_depth ));
	memset( vr.eye_tex, 0, sizeof( vr.eye_tex ));
	vr.have_buffers = false;
}

// one color texture plus one depth/stencil buffer per eye, made with plain GL (no engine helpers)
static qboolean VR_CreateEyeBuffers( int w, int h )
{
	int i;

	VR_FreeEyeBuffers();

	pglGenTextures( 2, vr.eye_tex );
	pglGenRenderbuffers( 2, vr.eye_depth );
	pglGenFramebuffers( 2, vr.eye_fbo );

	for( i = 0; i < 2; i++ )
	{
		GLenum status;

		pglBindTexture( GL_TEXTURE_2D, vr.eye_tex[i] );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );

		pglBindRenderbuffer( GL_RENDERBUFFER, vr.eye_depth[i] );
		pglRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h );

		pglBindFramebuffer( GL_FRAMEBUFFER, vr.eye_fbo[i] );
		pglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, vr.eye_tex[i], 0 );
		pglFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, vr.eye_depth[i] );

		status = pglCheckFramebufferStatus( GL_FRAMEBUFFER );

		if( status != GL_FRAMEBUFFER_COMPLETE )
		{
			gEngfuncs.Con_Printf( S_ERROR "VR: eye buffer %d is incomplete (status 0x%x)\n", i, (unsigned int)status );
			pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
			VR_FreeEyeBuffers();
			return false;
		}
	}

	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglBindRenderbuffer( GL_RENDERBUFFER, 0 );
	glState.currentTextures[glState.activeTMU] = -1;
	glState.currentTexturesIndex[glState.activeTMU] = -1;
	return true;
}

static void VR_EnsureEyeTargets( void )
{
	uint32_t rw = 0, rh = 0;
	int w, h;
	qboolean need = vr_use_fbo.value || vr_testscene.value >= 3.0f;

	if( !need )
	{
		vr.use_fbo = false;
		return;
	}

	if( vr.fbo_failed )
		return;

	vr.system->GetRecommendedRenderTargetSize( &rw, &rh );
	vr.rec_w = rw;
	vr.rec_h = rh;

	w = (int)( rw * vr_render_scale.value );
	h = (int)( rh * vr_render_scale.value );
	w = bound( 256, w, 8192 ) & ~1;
	h = bound( 256, h, 8192 ) & ~1;

	if( !vr.have_buffers || vr.eye_w != w || vr.eye_h != h )
	{
		pglFinish();

		if( !VR_CreateEyeBuffers( w, h ))
		{
			gEngfuncs.Con_Printf( S_ERROR "VR: can't create %dx%d eye buffers, using the window instead (lower quality)\n", w, h );
			vr.fbo_failed = true;
			vr.use_fbo = false;
			return;
		}

		vr.eye_w = w;
		vr.eye_h = h;
		vr.have_buffers = true;
		gEngfuncs.Con_Printf( "VR: eye buffers are %dx%d\n", w, h );
	}

	vr.use_fbo = vr_use_fbo.value != 0.0f;
}

void VR_SetupEyeView( int eye, const ref_viewpass_t *game, ref_viewpass_t *out )
{
	const HmdMatrix34_t *m = &vr.poses[VR_HMD_INDEX].mDeviceToAbsoluteTracking;
	float t[3] = { vr.eye_offset[eye][0] * vr_ipd_scale.value, vr.eye_offset[eye][1] * vr_ipd_scale.value, vr.eye_offset[eye][2] * vr_ipd_scale.value };
	float ep[3];
	vec3_t eyepos, offset, fwd, left, up;
	float d, xy, tan_x, tan_y;
	int i;

	*out = *game;
	VectorCopy( game->vieworigin, vr.game_origin );

	// eye position in tracking space: head position + head rotation * eye offset
	for( i = 0; i < 3; i++ )
	{
		float head = vr_positional.value ? m->m[i][3] : vr.ref_head_ovr[i];

		ep[i] = head + m->m[i][0] * t[0] + m->m[i][1] * t[1] + m->m[i][2] * t[2];
	}

	VR_ConvPos( ep, eyepos );
	VectorSubtract( eyepos, vr.ref_pos, offset );

	// the mouse/keyboard yaw turns the whole rig; the head only adds to it
	d = game->viewangles[YAW] - vr.ref_yaw;

	{
		// eye orientation = head orientation * eye-to-head rotation (identity on most headsets)
		float R[3][3], right[3], upv[3], back[3], negright[3], forward[3];
		int a, b, c;

		for( a = 0; a < 3; a++ )
			for( b = 0; b < 3; b++ )
			{
				R[a][b] = 0.0f;

				for( c = 0; c < 3; c++ )
					R[a][b] += m->m[a][c] * vr.eye_rot[eye][c][b];
			}

		right[0] = R[0][0]; right[1] = R[1][0]; right[2] = R[2][0];
		upv[0] = R[0][1]; upv[1] = R[1][1]; upv[2] = R[2][1];
		back[0] = R[0][2]; back[1] = R[1][2]; back[2] = R[2][2];
		negright[0] = -right[0]; negright[1] = -right[1]; negright[2] = -right[2];
		forward[0] = -back[0]; forward[1] = -back[1]; forward[2] = -back[2];

		VR_ConvDir( forward, fwd );
		VR_ConvDir( negright, left );
		VR_ConvDir( upv, up );
	}

	VR_RotateZ( offset, d, offset );
	VR_RotateZ( fwd, d, fwd );
	VR_RotateZ( left, d, left );
	VR_RotateZ( up, d, up );

	VectorAdd( game->vieworigin, offset, out->vieworigin );

	xy = sqrt( fwd[0] * fwd[0] + fwd[1] * fwd[1] );
	out->viewangles[PITCH] = RAD2DEG( atan2( -fwd[2], xy ));
	out->viewangles[YAW] = RAD2DEG( atan2( fwd[1], fwd[0] ));
	out->viewangles[ROLL] = RAD2DEG( atan2( left[2], up[2] ));

	if( vr_flip_roll.value )
		out->viewangles[ROLL] = -out->viewangles[ROLL];

	// asymmetric frustum from the headset; the culling frustum gets a symmetric superset
	for( i = 0; i < 4; i++ )
		vr.cur_frustum[i] = vr.tan_frustum[eye][i] * vr_fov_scale.value;

	tan_x = Q_max( -vr.cur_frustum[0], vr.cur_frustum[1] );
	tan_y = Q_max( vr.cur_frustum[2], -vr.cur_frustum[3] );
	out->fov_x = 2.0f * RAD2DEG( atan( tan_x ));
	out->fov_y = 2.0f * RAD2DEG( atan( tan_y ));

	vr.cur_eye = eye;
	vr.in_eye_pass = true;

	if( vr.use_fbo )
	{
		// start from a clean buffer: nothing from the previous frame may show through
		GLfloat clear[4];
		GLboolean scissor = pglIsEnabled( GL_SCISSOR_TEST );

		pglGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
		pglBindFramebuffer( GL_FRAMEBUFFER, vr.eye_fbo[eye & 1] );

		if( scissor )
			pglDisable( GL_SCISSOR_TEST );

		pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		pglDepthMask( GL_TRUE );
		pglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
		pglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		pglClearColor( clear[0], clear[1], clear[2], clear[3] );

		if( scissor )
			pglEnable( GL_SCISSOR_TEST );
	}
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

/*
=================
Diagnostic: a wireframe room that is fixed in the world, drawn with the same matrices as the game.
If this room warps when you turn your head, the problem is in how the eye images are made or shown.
If it stays rock solid while the game world warps, the problem is in what Cry of Fear draws.
=================
*/
static void VR_DrawTestScene( void )
{
	const float half = 240.0f, step = 24.0f, height = 192.0f;
	float proj[16], view[16], clear[4], x;
	vec3_t o;
	int saved_tmu, unit;

	if( !vr_testscene.value || vr_testscene.value >= 3.0f )
	{
		vr.test_anchored = false;
		return;
	}

	if( !vr.test_anchored )
	{
		VectorCopy( vr.game_origin, vr.test_origin );
		vr.test_anchored = true;
	}

	VectorCopy( vr.test_origin, o );
	o[2] -= 64.0f;	// floor, under the eyes

	Matrix4x4_ToArrayFloatGL( RI.projectionMatrix, proj );
	Matrix4x4_ToArrayFloatGL( RI.worldviewMatrix, view );

	saved_tmu = glState.activeTMU;
	pglPushAttrib( GL_ALL_ATTRIB_BITS );

	for( unit = GL_MaxTextureUnits() - 1; unit >= 0; unit-- )
	{
		GL_SelectTexture( unit );
		pglDisable( GL_TEXTURE_2D );
	}

	pglDisable( GL_BLEND );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_CULL_FACE );
	pglDisable( GL_SCISSOR_TEST );
	pglEnable( GL_DEPTH_TEST );
	pglDepthMask( GL_TRUE );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

	if( vr_testscene.value >= 2.0f )
	{
		pglGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
		pglClearColor( 0.04f, 0.04f, 0.07f, 1.0f );
		pglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		pglClearColor( clear[0], clear[1], clear[2], clear[3] );
	}

	pglMatrixMode( GL_PROJECTION );
	pglPushMatrix();
	pglLoadMatrixf( proj );
	pglMatrixMode( GL_MODELVIEW );
	pglPushMatrix();
	pglLoadMatrixf( view );

	pglLineWidth( 2.0f );
	pglBegin( GL_LINES );

	for( x = -half; x <= half; x += step )
	{
		// floor grid
		pglColor3f( 0.9f, 0.3f, 0.3f );
		pglVertex3f( o[0] + x, o[1] - half, o[2] );
		pglVertex3f( o[0] + x, o[1] + half, o[2] );
		pglColor3f( 0.3f, 0.4f, 0.95f );
		pglVertex3f( o[0] - half, o[1] + x, o[2] );
		pglVertex3f( o[0] + half, o[1] + x, o[2] );

		// four walls, lines both ways
		pglColor3f( 0.3f, 0.9f, 0.4f );
		pglVertex3f( o[0] + x, o[1] + half, o[2] );
		pglVertex3f( o[0] + x, o[1] + half, o[2] + height );
		pglVertex3f( o[0] + x, o[1] - half, o[2] );
		pglVertex3f( o[0] + x, o[1] - half, o[2] + height );
		pglVertex3f( o[0] + half, o[1] + x, o[2] );
		pglVertex3f( o[0] + half, o[1] + x, o[2] + height );
		pglVertex3f( o[0] - half, o[1] + x, o[2] );
		pglVertex3f( o[0] - half, o[1] + x, o[2] + height );
	}

	for( x = 0.0f; x <= height; x += step )
	{
		pglColor3f( 0.9f, 0.9f, 0.3f );
		pglVertex3f( o[0] - half, o[1] - half, o[2] + x );
		pglVertex3f( o[0] + half, o[1] - half, o[2] + x );
		pglVertex3f( o[0] + half, o[1] - half, o[2] + x );
		pglVertex3f( o[0] + half, o[1] + half, o[2] + x );
		pglVertex3f( o[0] + half, o[1] + half, o[2] + x );
		pglVertex3f( o[0] - half, o[1] + half, o[2] + x );
		pglVertex3f( o[0] - half, o[1] + half, o[2] + x );
		pglVertex3f( o[0] - half, o[1] - half, o[2] + x );
	}

	// a white cross hair box straight ahead (world +X) at 3 m, as a fixed reference
	pglColor3f( 1.0f, 1.0f, 1.0f );
	{
		float cx = o[0] + 118.0f, cy = o[1], cz = o[2] + 64.0f, r = 12.0f;
		int sx, sy, sz;

		for( sx = -1; sx <= 1; sx += 2 )
			for( sy = -1; sy <= 1; sy += 2 )
			{
				pglVertex3f( cx + sx * r, cy + sy * r, cz - r );
				pglVertex3f( cx + sx * r, cy + sy * r, cz + r );
			}

		for( sy = -1; sy <= 1; sy += 2 )
			for( sz = -1; sz <= 1; sz += 2 )
			{
				pglVertex3f( cx - r, cy + sy * r, cz + sz * r );
				pglVertex3f( cx + r, cy + sy * r, cz + sz * r );
			}

		for( sx = -1; sx <= 1; sx += 2 )
			for( sz = -1; sz <= 1; sz += 2 )
			{
				pglVertex3f( cx + sx * r, cy - r, cz + sz * r );
				pglVertex3f( cx + sx * r, cy + r, cz + sz * r );
			}
	}

	pglEnd();

	pglMatrixMode( GL_MODELVIEW );
	pglPopMatrix();
	pglMatrixMode( GL_PROJECTION );
	pglPopMatrix();
	pglMatrixMode( GL_MODELVIEW );

	pglPopAttrib();
	GL_SelectTexture( saved_tmu );
}

/*
=================
Placeholder controllers and the laser pointer, drawn into each eye (no depth test, so always visible)
=================
*/
static void VR_DrawHands( void )
{
	float proj[16], view[16];
	int saved_tmu, unit, h;

	if( !vr_draw_hands.value || ( !vr.hand[0].valid && !vr.hand[1].valid ))
		return;

	Matrix4x4_ToArrayFloatGL( RI.projectionMatrix, proj );
	Matrix4x4_ToArrayFloatGL( RI.worldviewMatrix, view );

	saved_tmu = glState.activeTMU;
	pglPushAttrib( GL_ALL_ATTRIB_BITS );

	for( unit = GL_MaxTextureUnits() - 1; unit >= 0; unit-- )
	{
		GL_SelectTexture( unit );
		pglDisable( GL_TEXTURE_2D );
	}

	pglDisable( GL_BLEND );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_CULL_FACE );
	pglDisable( GL_SCISSOR_TEST );
	pglDisable( GL_DEPTH_TEST );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

	pglMatrixMode( GL_PROJECTION );
	pglPushMatrix();
	pglLoadMatrixf( proj );
	pglMatrixMode( GL_MODELVIEW );
	pglPushMatrix();
	pglLoadMatrixf( view );

	pglLineWidth( 3.0f );
	pglBegin( GL_LINES );

	for( h = 0; h < 2; h++ )
	{
			vec3_t a, b;

			if( !vr.hand[h].valid )
			continue;

		// small axes: blue = pointing direction, red = sideways, green = up
		pglColor3f( 0.3f, 0.5f, 1.0f );
		pglVertex3f( vr.hand[h].pos[0], vr.hand[h].pos[1], vr.hand[h].pos[2] );
		VectorScale( vr.hand[h].fwd, 4.0f, a );
		VectorAdd( vr.hand[h].pos, a, b );
		pglVertex3f( b[0], b[1], b[2] );

		pglColor3f( 1.0f, 0.3f, 0.3f );
		VectorScale( vr.hand[h].left, 2.5f, a );
		VectorAdd( vr.hand[h].pos, a, b );
		pglVertex3f( b[0], b[1], b[2] );
		VectorSubtract( vr.hand[h].pos, a, b );
		pglVertex3f( b[0], b[1], b[2] );

		pglColor3f( 0.3f, 1.0f, 0.3f );
		pglVertex3f( vr.hand[h].pos[0], vr.hand[h].pos[1], vr.hand[h].pos[2] );
		VectorScale( vr.hand[h].up, 2.5f, a );
		VectorAdd( vr.hand[h].pos, a, b );
		pglVertex3f( b[0], b[1], b[2] );

		if( h == 1 )
		{
			// laser from the right hand: bright when it is on the UI, dim otherwise
			vec3_t to_target;
			float len = 40.0f;

			if( vr.ui_hit )
			{
				len = vr.ui_dist * vr_worldscale.value;
			}
			else if( vr_aim_hit )
			{
				VectorSubtract( vr_aim_target, vr.hand[h].pos, to_target );
				len = VectorLength( to_target );
			}

			if( vr.ui_hit ) pglColor3f( 0.2f, 1.0f, 1.0f );
			else if( vr_aim_hit ) pglColor3f( 0.3f, 1.0f, 0.3f );
			else pglColor3f( 0.35f, 0.35f, 0.45f );

			VectorScale( vr.hand[h].fwd, 5.0f, a );
			VectorAdd( vr.hand[h].pos, a, b );
			pglVertex3f( b[0], b[1], b[2] );
			VectorScale( vr.hand[h].fwd, len, a );
			VectorAdd( vr.hand[h].pos, a, b );
			pglVertex3f( b[0], b[1], b[2] );

			// a marker where the shot will land
			if( !vr.ui_hit && vr_aim_hit )
			{
				float m = 1.0f + len * 0.01f;

				pglColor3f( 1.0f, 0.2f, 0.2f );
				pglVertex3f( b[0] - m, b[1], b[2] ); pglVertex3f( b[0] + m, b[1], b[2] );
				pglVertex3f( b[0], b[1] - m, b[2] ); pglVertex3f( b[0], b[1] + m, b[2] );
				pglVertex3f( b[0], b[1], b[2] - m ); pglVertex3f( b[0], b[1], b[2] + m );
			}
		}
	}

	pglEnd();

	pglMatrixMode( GL_MODELVIEW );
	pglPopMatrix();
	pglMatrixMode( GL_PROJECTION );
	pglPopMatrix();
	pglMatrixMode( GL_MODELVIEW );

	pglPopAttrib();
	GL_SelectTexture( saved_tmu );
}

void VR_EndEyePass( int eye )
{
	int w, h;

	vr.in_eye_pass = false;

	if( vr_debug.value )
		pglGetIntegerv( GL_VIEWPORT, vr.dbg_viewport[eye & 1] );

	VR_DrawTestScene();
	VR_DrawHands();

	if( vr.use_fbo )
		return;	// already rendered into the eye's own buffer

	// the scene target (or the window) is still bound from R_SetupGL
	GL_GetSceneTargetSize( &w, &h );
	VR_EnsureTextures( w, h );

	pglBindTexture( GL_TEXTURE_2D, vr.tex[eye] );
	pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h );
	VR_ForgetTextureBinding();
}

/*
=================
Diagnostic (vr_testscene 3): a room drawn with nothing but SteamVR's own poses and field of view,
exactly the way Valve's sample does it. It bypasses the game, its camera, its projection and its
rig mapping. If THIS warps too, the problem is not in the engine at all.
Everything is in meters in SteamVR's standing space (floor at y = 0).
=================
*/
static void VR_RenderPureTest( int eye )
{
	const HmdMatrix34_t *m = &vr.poses[VR_HMD_INDEX].mDeviceToAbsoluteTracking;
	float Re[3][3], pe[3], proj[16], view[16];
	float l = vr.tan_frustum[eye][0], r = vr.tan_frustum[eye][1];
	float u = vr.tan_frustum[eye][2], dn = vr.tan_frustum[eye][3];
	const float n = 0.05f, f = 100.0f;
	int a, b, c, i, unit, saved_tmu;
	float x;

	for( a = 0; a < 3; a++ )
		for( b = 0; b < 3; b++ )
		{
			Re[a][b] = 0.0f;

			for( c = 0; c < 3; c++ )
				Re[a][b] += m->m[a][c] * vr.eye_rot[eye][c][b];
		}

	for( i = 0; i < 3; i++ )
		pe[i] = m->m[i][3] + m->m[i][0] * vr.eye_offset[eye][0] + m->m[i][1] * vr.eye_offset[eye][1] + m->m[i][2] * vr.eye_offset[eye][2];

	memset( proj, 0, sizeof( proj ));
	proj[0] = 2.0f / ( r - l );
	proj[5] = 2.0f / ( u - dn );
	proj[8] = ( r + l ) / ( r - l );
	proj[9] = ( u + dn ) / ( u - dn );
	proj[10] = -( f + n ) / ( f - n );
	proj[11] = -1.0f;
	proj[14] = -2.0f * f * n / ( f - n );

	// view = inverse of the eye's pose (rotation transposed, translation rotated back)
	memset( view, 0, sizeof( view ));

	for( i = 0; i < 3; i++ )
	{
		for( a = 0; a < 3; a++ )
			view[a * 4 + i] = Re[a][i];

		view[12 + i] = -( Re[0][i] * pe[0] + Re[1][i] * pe[1] + Re[2][i] * pe[2] );
	}

	view[15] = 1.0f;

	saved_tmu = glState.activeTMU;
	pglPushAttrib( GL_ALL_ATTRIB_BITS );

	for( unit = GL_MaxTextureUnits() - 1; unit >= 0; unit-- )
	{
		GL_SelectTexture( unit );
		pglDisable( GL_TEXTURE_2D );
	}

	pglBindFramebuffer( GL_FRAMEBUFFER, vr.eye_fbo[eye & 1] );
	pglViewport( 0, 0, vr.eye_w, vr.eye_h );
	pglDisable( GL_SCISSOR_TEST );
	pglDisable( GL_BLEND );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_CULL_FACE );
	pglEnable( GL_DEPTH_TEST );
	pglDepthMask( GL_TRUE );
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	pglClearColor( 0.05f, 0.05f, 0.08f, 1.0f );
	pglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	pglMatrixMode( GL_PROJECTION );
	pglPushMatrix();
	pglLoadMatrixf( proj );
	pglMatrixMode( GL_MODELVIEW );
	pglPushMatrix();
	pglLoadMatrixf( view );

	pglLineWidth( 2.0f );
	pglBegin( GL_LINES );

	for( x = -3.0f; x <= 3.001f; x += 0.5f )
	{
		// floor grid
		pglColor3f( 0.45f, 0.45f, 0.6f );
		pglVertex3f( x, 0.0f, -3.0f ); pglVertex3f( x, 0.0f, 3.0f );
		pglVertex3f( -3.0f, 0.0f, x ); pglVertex3f( 3.0f, 0.0f, x );

		// four walls
		pglColor3f( 0.3f, 0.7f, 0.4f );
		pglVertex3f( x, 0.0f, -3.0f ); pglVertex3f( x, 3.0f, -3.0f );
		pglVertex3f( x, 0.0f, 3.0f ); pglVertex3f( x, 3.0f, 3.0f );
		pglVertex3f( -3.0f, 0.0f, x ); pglVertex3f( -3.0f, 3.0f, x );
		pglVertex3f( 3.0f, 0.0f, x ); pglVertex3f( 3.0f, 3.0f, x );
	}

	for( x = 0.5f; x <= 3.001f; x += 0.5f )
	{
		pglColor3f( 0.8f, 0.8f, 0.3f );
		pglVertex3f( -3.0f, x, -3.0f ); pglVertex3f( 3.0f, x, -3.0f );
		pglVertex3f( 3.0f, x, -3.0f ); pglVertex3f( 3.0f, x, 3.0f );
		pglVertex3f( 3.0f, x, 3.0f ); pglVertex3f( -3.0f, x, 3.0f );
		pglVertex3f( -3.0f, x, 3.0f ); pglVertex3f( -3.0f, x, -3.0f );
	}

	// axes at the center of the play space: red = +x, green = up, blue = +z (toward the back)
	pglColor3f( 1.0f, 0.2f, 0.2f ); pglVertex3f( 0.0f, 0.02f, 0.0f ); pglVertex3f( 1.0f, 0.02f, 0.0f );
	pglColor3f( 0.2f, 1.0f, 0.2f ); pglVertex3f( 0.0f, 0.02f, 0.0f ); pglVertex3f( 0.0f, 1.0f, 0.0f );
	pglColor3f( 0.3f, 0.4f, 1.0f ); pglVertex3f( 0.0f, 0.02f, 0.0f ); pglVertex3f( 0.0f, 0.02f, 1.0f );

	// a white cube two meters in front (-z), at head height
	pglColor3f( 1.0f, 1.0f, 1.0f );
	{
		const float cx = 0.0f, cy = 1.5f, cz = -2.0f, e = 0.25f;
		int sx, sy, sz;

		for( sx = -1; sx <= 1; sx += 2 )
			for( sy = -1; sy <= 1; sy += 2 )
			{
				pglVertex3f( cx + sx * e, cy + sy * e, cz - e ); pglVertex3f( cx + sx * e, cy + sy * e, cz + e );
			}

		for( sy = -1; sy <= 1; sy += 2 )
			for( sz = -1; sz <= 1; sz += 2 )
			{
				pglVertex3f( cx - e, cy + sy * e, cz + sz * e ); pglVertex3f( cx + e, cy + sy * e, cz + sz * e );
			}

		for( sx = -1; sx <= 1; sx += 2 )
			for( sz = -1; sz <= 1; sz += 2 )
			{
				pglVertex3f( cx + sx * e, cy - e, cz + sz * e ); pglVertex3f( cx + sx * e, cy + e, cz + sz * e );
			}
	}

	pglEnd();

	pglMatrixMode( GL_MODELVIEW );
	pglPopMatrix();
	pglMatrixMode( GL_PROJECTION );
	pglPopMatrix();
	pglMatrixMode( GL_MODELVIEW );

	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglPopAttrib();
	GL_SelectTexture( saved_tmu );
}

void VR_FinishFrame( void )
{
	int eye;
	qboolean pure = vr_testscene.value >= 3.0f && vr.have_buffers;

	if( pure )
	{
		VR_RenderPureTest( 0 );
		VR_RenderPureTest( 1 );
	}

	if( vr.use_fbo )
		pglBindFramebuffer( GL_FRAMEBUFFER, 0 );

	pglFlush();

	if( vr_finish.value )
		pglFinish();

	for( eye = 0; eye < 2; eye++ )
	{
		Texture_t tex;
		VRTextureBounds_t bounds;
		int err;

		tex.handle = (void *)(uintptr_t)(( vr.use_fbo || pure ) ? vr.eye_tex[eye] : vr.tex[eye] );
		tex.eType = VR_TEXTURE_OPENGL;
		tex.eColorSpace = VR_COLORSPACE_GAMMA;

		bounds.uMin = 0.0f;
		bounds.uMax = 1.0f;
		bounds.vMin = 0.0f;
		bounds.vMax = 1.0f;

		err = vr.compositor->Submit( eye, &tex, &bounds, VR_SUBMIT_DEFAULT );

		if( err != VR_COMPOSITOR_OK && vr.err_prints++ < 10 )
			gEngfuncs.Con_Printf( S_ERROR "VR: Submit failed for eye %d (code %d)\n", eye, (int)err );
	}

	pglFlush();

	vr.frame_done = true;

	if( vr_dump_done )
	{
		vr_dump_done = false;
		vr_dump_bones_done = false;
		gEngfuncs.Cvar_SetValue( "vr_vm_dump_tex", 0.0f );
	}

	// The eye images are captured. Clear the window to transparent so that everything the engine
	// draws next (HUD, menus, subtitles) lands on its own and can be shown on the floating UI panel.
	{
		GLfloat clear[4];
		GLboolean scissor = pglIsEnabled( GL_SCISSOR_TEST );

		pglGetFloatv( GL_COLOR_CLEAR_VALUE, clear );

		if( scissor )
			pglDisable( GL_SCISSOR_TEST );

		pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		pglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		pglClear( GL_COLOR_BUFFER_BIT );
		pglClearColor( clear[0], clear[1], clear[2], clear[3] );

		if( scissor )
			pglEnable( GL_SCISSOR_TEST );
	}

	if( vr_debug.value && ++vr.debug_frames >= 72 )
	{
		vr.debug_frames = 0;
		gEngfuncs.Con_Printf( "VR debug: eye0 tan L%.3f R%.3f U%.3f D%.3f (rot %.2f deg), eye1 tan L%.3f R%.3f U%.3f D%.3f (rot %.2f deg)\n",
			vr.tan_frustum[0][0], vr.tan_frustum[0][1], vr.tan_frustum[0][2], vr.tan_frustum[0][3], vr.eye_rot_deg[0],
			vr.tan_frustum[1][0], vr.tan_frustum[1][1], vr.tan_frustum[1][2], vr.tan_frustum[1][3], vr.eye_rot_deg[1] );
		gEngfuncs.Con_Printf( "VR debug: eye offsets %.4f %.4f %.4f / %.4f %.4f %.4f m, recommended %ux%u, fbo %d (%dx%d), window %dx%d\n",
			vr.eye_offset[0][0], vr.eye_offset[0][1], vr.eye_offset[0][2], vr.eye_offset[1][0], vr.eye_offset[1][1], vr.eye_offset[1][2],
			vr.rec_w, vr.rec_h, vr.use_fbo, vr.eye_w, vr.eye_h, gpGlobals->width, gpGlobals->height );
		gEngfuncs.Con_Printf( "VR debug: GL viewport after eye0 %d %d %d %d, after eye1 %d %d %d %d, head %.3f %.3f %.3f m, scale %.2f, fovscale %.2f\n",
			vr.dbg_viewport[0][0], vr.dbg_viewport[0][1], vr.dbg_viewport[0][2], vr.dbg_viewport[0][3],
			vr.dbg_viewport[1][0], vr.dbg_viewport[1][1], vr.dbg_viewport[1][2], vr.dbg_viewport[1][3],
			vr.head_pos[0], vr.head_pos[1], vr.head_pos[2], vr_worldscale.value, vr_fov_scale.value );
		gEngfuncs.Con_Printf( "VR debug: weapon hook asked %d times, applied %d times (0 applied means the weapon is not drawn through the normal viewmodel path)\n", vr.vm_asked, vr.vm_applied );
		vr.vm_asked = vr.vm_applied = 0;

		if( vr_cam_have )
		{
			// stand still and keep your head still: these should all be (nearly) zero
			gEngfuncs.Con_Printf( "VR debug: game camera moved in the last second: yaw %.3f, pitch %.3f, roll %.3f degrees, origin %.3f %.3f %.3f units\n",
				vr_cam_max[1] - vr_cam_min[1], vr_cam_max[0] - vr_cam_min[0], vr_cam_max[2] - vr_cam_min[2],
				vr_cam_max[3] - vr_cam_min[3], vr_cam_max[4] - vr_cam_min[4], vr_cam_max[5] - vr_cam_min[5] );
			vr_cam_have = false;
		}
	}
}
