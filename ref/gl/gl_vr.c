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
#include <windows.h>
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
static CVAR_DEFINE_AUTO( vr_sym_proj, "1", FCVAR_ARCHIVE, "1 = draw each eye with a centred (symmetric) camera like the normal game and let SteamVR crop the part your lens needs. The game's own water, glass and sky effects assume a centred camera and warp otherwise. 0 = the headset's off-centre frustum" );
static CVAR_DEFINE_AUTO( vr_flip_vfov, "1", FCVAR_ARCHIVE, "diagnostic: swap the up and down field of view. Try 1 if what is drawn sits a little too low or high compared to the menu pointer" );
static CVAR_DEFINE_AUTO( vr_fov_scale, "1.0", 0, "diagnostic: scale the rendered field of view tangents (1.0 = exact)" );
static CVAR_DEFINE_AUTO( vr_crouch_height, "0.30", FCVAR_ARCHIVE, "meters below your recentered height that count as physically crouching (you stand up again at 60% of this)" );
static CVAR_DEFINE_AUTO( vr_crouch_comp, "28", FCVAR_ARCHIVE, "units the view is given back when you crouch (the game lowers it too, so it is not counted twice). 0 = measured automatically (experimental)" );
static CVAR_DEFINE_AUTO( vr_nofreeze_models, "", 0, "weapons that are never held in the idle pose (their own animation is used): part of the file name, + between names" );
static CVAR_DEFINE_AUTO( vr_vm_rot_local, "1", FCVAR_ARCHIVE, "1 = the weapon's pitch/yaw/roll turn in the hand's own axes. 0 = the old way (added to world angles)" );
static CVAR_DEFINE_AUTO( vr_light_pitch, "0", 0, "tilt the spotlight up (+) or down (-) from where the controller points, degrees (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_light_yaw, "0", 0, "turn the spotlight left (+) or right (-) from where the controller points, degrees (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_vm_mirror, "-1", 0, "mirror the weapon in hand left-right: -1 = automatic (vr_mirror_models), 0 = never, 1 = always (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_mirror_models, "flare", 0, "weapon model names (separated by +) that the game holds in the left hand, mirrored to the right" );
static CVAR_DEFINE_AUTO( vr_eye_prepass, "2", FCVAR_ARCHIVE, "draw an eye twice and keep the second picture (for objects that are black in one eye, or a sky that lags, when the game's opengl32.dll is used): 1 = left eye twice, 2 = both eyes twice" );
static CVAR_DEFINE_AUTO( vr_roomscale, "1", FCVAR_ARCHIVE, "1 = your character walks to where you really stand in your room" );
static CVAR_DEFINE_AUTO( vr_room_deadzone, "4", FCVAR_ARCHIVE, "how far (game units, about 2.5 cm each) you have to step before your character follows" );
static CVAR_DEFINE_AUTO( vr_hide_models, "", 0, "weapon / prop model names (parts, separated by +) that are not drawn at all. Find the name with vr_ents. For things that should not be visible, like the big flat terrain" );
static CVAR_DEFINE_AUTO( vr_hide_renderfx, "", 0, "render fx numbers (separated by +) whose entities are not drawn, e.g. 92+184. See vr_ents" );
static CVAR_DEFINE_AUTO( vr_scope_line, "1", FCVAR_ARCHIVE, "thickness of the scope crosshair in pixels" );
static CVAR_DEFINE_AUTO( vr_anim_still, "idle+fidget", 0, "weapon animations (name parts, separated by +) that never play: the weapon stays in the first frame. Keeps the aim steady" );
static CVAR_DEFINE_AUTO( vr_anim_end, "draw+equip", 0, "weapon animations (name parts, separated by +) that are skipped: the weapon is shown in the ready pose right away" );
static CVAR_DEFINE_AUTO( vr_empty_hand, "models/weapons/v_actions.mdl", FCVAR_ARCHIVE, "the model shown as your right hand when you hold nothing. Empty = none" );
static CVAR_DEFINE_AUTO( vr_room_maxlag, "14", FCVAR_ARCHIVE, "how far (game units) your head can get ahead of your character before the view is pulled back. This is what stops you walking through walls" );
static CVAR_DEFINE_AUTO( vr_eye_flush, "0", FCVAR_ARCHIVE, "experiment for thin / see-through textures that are missing in VR: 1 = glFlush, 2 = glFinish at the end of each eye" );
static CVAR_DEFINE_AUTO( vr_aim_pitch, "0", 0, "aim the shots higher (+) or lower (-) than the controller points, degrees (saved per weapon). For items like the camera" );
static CVAR_DEFINE_AUTO( vr_aim_yaw, "0", 0, "aim the shots to the left (+) or right (-) of where the controller points, degrees (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_light_x, "6", 0, "where the light sits, forward of the hand (weapon axes, saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_light_y, "0", 0, "where the light sits, left of the hand (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_light_z, "0", 0, "where the light sits, above the hand (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_scope, "1", FCVAR_ARCHIVE, "1 = rifle scope: lift the rifle to your eye and the scope shows a zoomed view" );
static CVAR_DEFINE_AUTO( vr_scope_models, "rifle+sniper+hunting", 0, "weapon model names (separated by +) that have a scope" );
static CVAR_DEFINE_AUTO( vr_scope_fov, "2", 0, "field of view of the scope image in degrees (smaller = more zoom, saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_scope_bone, "Object02", FCVAR_ARCHIVE, "part of the name of the rifle bone the scope lens follows (empty or not found = the bone with the most vertices)" );
static CVAR_DEFINE_AUTO( vr_scope_debug, "0", 0, "1 = print which bone the scope follows and how far it moved" );
static CVAR_DEFINE_AUTO( vr_scope_follow, "1", FCVAR_ARCHIVE, "1 = the scope lens moves with the rifle during reload animations" );
static CVAR_DEFINE_AUTO( vr_scope_x, "0.4", 0, "scope eyepiece position, forward of the hand (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_scope_y, "-0.3", 0, "scope eyepiece position, left of the hand (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_scope_z, "2.5", 0, "scope eyepiece position, above the hand (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_scope_radius, "0.4", 0, "radius of the scope lens in game units (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_scope_cam, "6", 0, "how far in front of the eyepiece the scope camera sits" );
static CVAR_DEFINE_AUTO( vr_scope_range, "20", 0, "the scope view only renders while your head is this close to the eyepiece" );
static CVAR_DEFINE_AUTO( vr_sky_dome, "0", 0, "1 = draw the sky models (sky_sphere_*.mdl) around you with our own code. Off by default: the game draws its own sky" );
static CVAR_DEFINE_AUTO( vr_hide_crosshair, "1", FCVAR_ARCHIVE, "1 = switch the game's crosshair off (the laser and red cross are your aim)" );
static CVAR_DEFINE_AUTO( vr_physical_crouch, "1", FCVAR_ARCHIVE, "1 = crouching in real life also crouches the character, so you can get through low gaps" );
static CVAR_DEFINE_AUTO( vr_use_fbo, "0", 0, "1 = render each eye into its own buffer at SteamVR's recommended size (experimental). 0 = render in the window and copy" );
static CVAR_DEFINE_AUTO( vr_render_scale, "1.0", 0, "eye image size relative to what SteamVR recommends (needs a new map load or vid_restart to change)" );
static CVAR_DEFINE_AUTO( vr_finish, "1", 0, "wait for the GPU after submitting the eye images (matches Valve's sample)" );
static CVAR_DEFINE_AUTO( vr_ipd_scale, "1.0", 0, "diagnostic: scale the distance between the eyes (0 = mono, 1 = real)" );
static CVAR_DEFINE_AUTO( vr_testscene, "0", 0, "diagnostic: 1 = a world-fixed wireframe room over the game, 2 = only that room, 3 = a pure SteamVR-only test room that bypasses the game completely" );
static CVAR_DEFINE_AUTO( vr_hand_pitch, "60", FCVAR_ARCHIVE, "tilt the controller's pointing direction: positive = down, negative = up (degrees). Adjust until the laser points where your hand naturally points" );
static CVAR_DEFINE_AUTO( vr_draw_axes, "0", FCVAR_ARCHIVE, "draw the small colored axes at each controller (debug visual)" );
static CVAR_DEFINE_AUTO( vr_draw_laser, "0", FCVAR_ARCHIVE, "draw the laser line and the red aim cross from your right hand (debug visual)" );
static CVAR_DEFINE_AUTO( vr_draw_hands, "1", FCVAR_ARCHIVE, "draw placeholder controllers and the laser pointer" );
static CVAR_DEFINE_AUTO( vr_hand_weapon, "1", FCVAR_ARCHIVE, "draw the weapon at your right hand instead of in front of your face" );
static CVAR_DEFINE_AUTO( vr_vm_grip_x, "13", FCVAR_ARCHIVE, "where the right hand holds the weapon model (model axes): forward" );
static CVAR_DEFINE_AUTO( vr_vm_grip_y, "-4", FCVAR_ARCHIVE, "where the right hand holds the weapon model: left (negative = right)" );
static CVAR_DEFINE_AUTO( vr_vm_grip_z, "-3", FCVAR_ARCHIVE, "where the right hand holds the weapon model: up" );
static CVAR_DEFINE_AUTO( vr_vm_flip_roll, "0", FCVAR_ARCHIVE, "set to 1 if the weapon is rolled the wrong way" );
static CVAR_DEFINE_AUTO( vr_vm_pitch, "0", FCVAR_ARCHIVE, "extra rotation of the weapon model: pitch degrees" );
static CVAR_DEFINE_AUTO( vr_vm_yaw, "0", FCVAR_ARCHIVE, "extra rotation of the weapon model: yaw degrees" );
static CVAR_DEFINE_AUTO( vr_vm_roll, "0", FCVAR_ARCHIVE, "extra rotation of the weapon model: roll degrees" );
static CVAR_DEFINE_AUTO( vr_aim_trace, "1", FCVAR_ARCHIVE, "1 = shots go to the exact point your hand ray hits, 0 = fixed distance (vr_aim_converge)" );
static CVAR_DEFINE_AUTO( vr_aim_converge, "96", 0, "only used when vr_aim_trace is 0: distance in units where the game's aim meets your hand's pointing direction" );
static CVAR_DEFINE_AUTO( vr_aim_debug, "0", 0, "print what the aim trace is doing about once a second" );
static CVAR_DEFINE_AUTO( vr_vm_hide_bones, "^l_", FCVAR_ARCHIVE, "parts of the weapon model attached to bones whose name starts with (^) or contains one of these are not drawn (the left arm). Separate with +. vr_vm_dump_tex 2 lists the bones" );
static CVAR_DEFINE_AUTO( vr_vm_cap_radius, "0", FCVAR_ARCHIVE, "size of the disc that closes the cut-off arm at the wrist (0 = none)" );
static CVAR_DEFINE_AUTO( vr_vm_cap_gray, "0.1", FCVAR_ARCHIVE, "brightness of that disc, 0 = black, 1 = white" );
static CVAR_DEFINE_AUTO( vr_static_models, "mobile+flashlight+flare+lantern+syringe+camera+radio", 0, "weapon model names (part of the file name) whose animations never play, the model stays in one pose in your hand. Separate with +" );
static CVAR_DEFINE_AUTO( vr_melee_models, "fist+actions+axe+branch+nightstick+sledge+pickaxe+shovel+switchblade", 0, "weapon model names (part of the file name) that are swung by hand. Separate with +" );
static CVAR_DEFINE_AUTO( vr_melee_tip, "18", FCVAR_ARCHIVE, "how far beyond your hand the tip of a melee weapon is, in units (swings are traced from there)" );
static CVAR_DEFINE_AUTO( vr_melee_reach, "64", FCVAR_ARCHIVE, "how far in front of your swinging hand the game looks for something to hit, in units" );
static CVAR_DEFINE_AUTO( vr_omni_models, "flare+lantern", 0, "weapon model names that light up everything around your hand (flare, lantern). Separate with +" );
static CVAR_DEFINE_AUTO( vr_lhand, "1", FCVAR_ARCHIVE, "1 = a left hand is drawn at your left controller" );
static CVAR_DEFINE_AUTO( vr_lhand_model, "models/weapons/switchblade/v_switchblade.mdl", 0, "weapon model whose left arm is used as the left hand" );
static CVAR_DEFINE_AUTO( vr_lhand_hide_bones, "^r_+grippity+blade", 0, "parts of the left hand model that are not drawn (the right arm and the weapon). Separate with +" );
static CVAR_DEFINE_AUTO( vr_lh_grip_x, "12", FCVAR_ARCHIVE, "left hand position on the controller: forward" );
static CVAR_DEFINE_AUTO( vr_lh_grip_y, "-6", FCVAR_ARCHIVE, "left hand position on the controller: left (negative = right)" );
static CVAR_DEFINE_AUTO( vr_lh_grip_z, "-3", FCVAR_ARCHIVE, "left hand position on the controller: up" );
static CVAR_DEFINE_AUTO( vr_lh_pitch, "0", FCVAR_ARCHIVE, "left hand rotation: pitch degrees" );
static CVAR_DEFINE_AUTO( vr_lh_yaw, "0", FCVAR_ARCHIVE, "left hand rotation: yaw degrees" );
static CVAR_DEFINE_AUTO( vr_lh_roll, "270", FCVAR_ARCHIVE, "left hand rotation: roll degrees" );
static CVAR_DEFINE_AUTO( vr_lh_flip_roll, "0", FCVAR_ARCHIVE, "set to 1 if the left hand is rolled the wrong way" );
static CVAR_DEFINE_AUTO( vr_lh_scale, "0.45", FCVAR_ARCHIVE, "size of the left hand" );
static CVAR_DEFINE_AUTO( vr_lh_arm_cut, "6", FCVAR_ARCHIVE, "where the left arm is cut off, in units behind the hand" );
static CVAR_DEFINE_AUTO( vr_lh_cut_pitch, "30", FCVAR_ARCHIVE, "tilt of the left arm cut: pitch degrees" );
static CVAR_DEFINE_AUTO( vr_lh_cut_yaw, "0", FCVAR_ARCHIVE, "tilt of the left arm cut: yaw degrees" );
static CVAR_DEFINE_AUTO( vr_vm_cut_pitch, "0", FCVAR_ARCHIVE, "tilt of the weapon hand's arm cut: pitch degrees (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_vm_cut_yaw, "0", FCVAR_ARCHIVE, "tilt of the weapon hand's arm cut: yaw degrees (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_phone_light, "1", FCVAR_ARCHIVE, "1 = a spotlight shines from the weapons listed in vr_light_models, where your hand points" );
static CVAR_DEFINE_AUTO( vr_light_models, "mobile+flashlight", 0, "weapon model names (part of the file name) that work as a flashlight. Separate with +" );
static CVAR_DEFINE_AUTO( vr_light_distance, "700", FCVAR_ARCHIVE, "how far the flashlight reaches, in units" );
static CVAR_DEFINE_AUTO( vr_dual, "1", FCVAR_ARCHIVE, "1 = a dual wielded weapon is shown in your left hand" );
static CVAR_DEFINE_AUTO( vr_dual_x, "0", FCVAR_ARCHIVE, "left hand weapon: extra grip offset, forward (added to the weapon's own numbers, mirrored)" );
static CVAR_DEFINE_AUTO( vr_dual_y, "0", FCVAR_ARCHIVE, "left hand weapon: extra grip offset, sideways" );
static CVAR_DEFINE_AUTO( vr_dual_z, "0", FCVAR_ARCHIVE, "left hand weapon: extra grip offset, up" );
static CVAR_DEFINE_AUTO( vr_dual_pitch, "0", FCVAR_ARCHIVE, "left hand weapon: extra pitch" );
static CVAR_DEFINE_AUTO( vr_dual_yaw, "0", FCVAR_ARCHIVE, "left hand weapon: extra yaw" );
static CVAR_DEFINE_AUTO( vr_dual_roll, "0", FCVAR_ARCHIVE, "left hand weapon: extra roll" );
static CVAR_DEFINE_AUTO( vr_dual_scale, "1", FCVAR_ARCHIVE, "left hand weapon: size multiplier on top of the weapon's own scale" );
static CVAR_DEFINE_AUTO( vr_muzzle_x, "0", FCVAR_ARCHIVE, "where the muzzle flash, shells and tracers start, moved along the weapon (saved per weapon)" );
static CVAR_DEFINE_AUTO( vr_muzzle_y, "0", FCVAR_ARCHIVE, "muzzle: sideways" );
static CVAR_DEFINE_AUTO( vr_muzzle_z, "0", FCVAR_ARCHIVE, "muzzle: up" );
static CVAR_DEFINE_AUTO( vr_dual_builtin, "1", FCVAR_ARCHIVE, "1 = the left weapon of a dual wield is drawn by the engine's own studio renderer (it then turns with your hand). 0 = the game's renderer" );
static CVAR_DEFINE_AUTO( vr_hand_collide, "0", 0, "1 = the weapon in your hand is pushed back by walls instead of going through them" );
static CVAR_DEFINE_AUTO( vr_hand_reach, "18", FCVAR_ARCHIVE, "how far in front of the hand the weapon counts as touching a wall (units)" );
static CVAR_DEFINE_AUTO( vr_anim_play, "shoot+fire+reload+shot", FCVAR_ARCHIVE, "animations of a weapon that play (part of the animation name, separated by +). All others hold the idle pose. A weapon can have its own choice (vr_anim_toggle, saved with vr_weapon_save)" );
static CVAR_DEFINE_AUTO( vr_anim_mask, "0", 0, "animations that play for this weapon, as a number (bit per animation number). 0 = decided by the names in vr_anim_play" );
static CVAR_DEFINE_AUTO( vr_empty_hand_seq, "8", FCVAR_ARCHIVE, "animation of the empty fist model (v_actions.mdl) that is shown when your hand is empty. -1 = whatever the game plays" );
static CVAR_DEFINE_AUTO( vr_empty_hand_frame, "0", FCVAR_ARCHIVE, "frame of that animation" );
static CVAR_DEFINE_AUTO( vr_vm_lock_seq, "-1", 0, "testing: show this animation sequence of the weapon model (-1 = normal). vr_vm_seqlist lists them" );
static CVAR_DEFINE_AUTO( vr_vm_lock_frame, "0", 0, "testing: frame of that sequence to hold" );
static CVAR_DEFINE_AUTO( vr_melee_freeze, "1", FCVAR_ARCHIVE, "1 = melee weapons hold their first animation frame, so only your real arm swings" );
static CVAR_DEFINE_AUTO( vr_vm_scale, "0.5", FCVAR_ARCHIVE, "size of the weapon model in your hand (1.0 = original)" );
static CVAR_DEFINE_AUTO( vr_vm_hide_tex, "sleeve", FCVAR_ARCHIVE, "texture name fragments of weapon model parts to hide completely. Separate with + (a comma does not work in the console)" );
static CVAR_DEFINE_AUTO( vr_vm_clip_tex, "glove+hand+fingers+tex1+tex2", FCVAR_ARCHIVE, "texture name fragments of weapon model parts (the arm) that are cut off behind the wrist. Separate with +" );
static CVAR_DEFINE_AUTO( vr_vm_arm_cut, "2", FCVAR_ARCHIVE, "where the arm is cut off, in units behind your hand (bigger = more arm visible)" );
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
	qboolean	crouching;	// physically crouching (with hysteresis)
	float		crouch_comp;	// units added to the eye height so the game's own crouch is not counted twice
	vec3_t		head_game;	// current head position in the game world, units
	float		head_game_yaw;	// current head yaw in the game world, degrees
	vec3_t		axis_fwd, axis_left, axis_up;	// current head basis in game axes (unrotated)

	float		cur_frustum[4];
	float		sym_tan_x, sym_tan_y;
	float		eye_bounds[2][4];	// what part of the (centred) picture the lens needs: uMin, uMax, vMin, vMax

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
static void VR_WeaponReset_f( void );
static void VR_VMInfo_f( void );
static void VR_GLInfo_f( void );
static void VR_Ents_f( void );
static void VR_WeaponSave_f( void );
static void VR_Center_f( void );
static void VR_HideCrosshair( void );

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

static void VR_SkyInfo_f( void );
static void VR_ViewEnts_f( void );

static void VR_VisualsOff_f( void )
{
	gEngfuncs.Cvar_SetValue( "vr_draw_axes", 0.0f );
	gEngfuncs.Cvar_SetValue( "vr_draw_laser", 0.0f );
	gEngfuncs.Con_Printf( "VR: controller debug visuals are off (vr_visuals_on brings them back)\n" );
}

static void VR_VisualsOn_f( void )
{
	gEngfuncs.Cvar_SetValue( "vr_draw_axes", 1.0f );
	gEngfuncs.Cvar_SetValue( "vr_draw_laser", 1.0f );
	gEngfuncs.Con_Printf( "VR: controller debug visuals are on\n" );
}

static void VR_CaptureTuneDefaults( void );

void VR_InitCvars( void )
{
	// before anything is registered the cvars still hold the defaults from this file, not what an old config saved
	VR_CaptureTuneDefaults();

	gEngfuncs.Cvar_RegisterVariable( &vr_enable );
	gEngfuncs.Cvar_RegisterVariable( &vr_worldscale );
	gEngfuncs.Cvar_RegisterVariable( &vr_flip_roll );
	gEngfuncs.Cvar_RegisterVariable( &vr_positional );
	gEngfuncs.Cvar_RegisterVariable( &vr_fov_scale );
	gEngfuncs.Cvar_RegisterVariable( &vr_flip_vfov );
	gEngfuncs.Cvar_RegisterVariable( &vr_sym_proj );
	gEngfuncs.Cvar_RegisterVariable( &vr_debug );
	gEngfuncs.Cvar_RegisterVariable( &vr_hand_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_draw_hands );
	gEngfuncs.Cvar_RegisterVariable( &vr_draw_axes );
	gEngfuncs.Cvar_RegisterVariable( &vr_draw_laser );
	gEngfuncs.Cmd_AddCommand( "vr_visuals_off", VR_VisualsOff_f, "hide all the controller debug visuals (axes, laser, aim cross) for a clean, immersive view" );
	gEngfuncs.Cmd_AddCommand( "vr_visuals_on", VR_VisualsOn_f, "show the controller debug visuals again" );
	gEngfuncs.Cvar_RegisterVariable( &vr_hand_weapon );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_grip_x );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_grip_y );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_grip_z );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_flip_roll );
	gEngfuncs.Cvar_RegisterVariable( &vr_aim_converge );
	gEngfuncs.Cvar_RegisterVariable( &vr_aim_debug );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_hide_bones );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_cap_radius );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_cap_gray );
	gEngfuncs.Cvar_RegisterVariable( &vr_melee_models );
	gEngfuncs.Cvar_RegisterVariable( &vr_static_models );
	gEngfuncs.Cvar_RegisterVariable( &vr_melee_tip );
	gEngfuncs.Cvar_RegisterVariable( &vr_melee_reach );
	gEngfuncs.Cvar_RegisterVariable( &vr_melee_freeze );
	gEngfuncs.Cvar_RegisterVariable( &vr_muzzle_x );
	gEngfuncs.Cvar_RegisterVariable( &vr_muzzle_y );
	gEngfuncs.Cvar_RegisterVariable( &vr_muzzle_z );
	gEngfuncs.Cvar_RegisterVariable( &vr_dual );
	gEngfuncs.Cvar_RegisterVariable( &vr_dual_x );
	gEngfuncs.Cvar_RegisterVariable( &vr_dual_y );
	gEngfuncs.Cvar_RegisterVariable( &vr_dual_z );
	gEngfuncs.Cvar_RegisterVariable( &vr_dual_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_dual_yaw );
	gEngfuncs.Cvar_RegisterVariable( &vr_dual_roll );
	gEngfuncs.Cvar_RegisterVariable( &vr_dual_scale );
	gEngfuncs.Cvar_RegisterVariable( &vr_hand_collide );
	gEngfuncs.Cvar_RegisterVariable( &vr_dual_builtin );
	gEngfuncs.Cvar_RegisterVariable( &vr_hand_reach );
	gEngfuncs.Cvar_RegisterVariable( &vr_anim_play );
	gEngfuncs.Cvar_RegisterVariable( &vr_anim_mask );
	gEngfuncs.Cvar_RegisterVariable( &vr_empty_hand_seq );
	VR_StudioCmds();
	gEngfuncs.Cvar_RegisterVariable( &vr_empty_hand_frame );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_lock_seq );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_lock_frame );
	gEngfuncs.Cmd_AddCommand( "vr_vm_seqlist", VR_DumpSequences, "list the animation sequences of the weapon you hold" );
	gEngfuncs.Cvar_RegisterVariable( &vr_phone_light );
	gEngfuncs.Cvar_RegisterVariable( &vr_omni_models );
	gEngfuncs.Cvar_RegisterVariable( &vr_lhand );
	gEngfuncs.Cvar_RegisterVariable( &vr_lhand_model );
	gEngfuncs.Cvar_RegisterVariable( &vr_lhand_hide_bones );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_grip_x );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_grip_y );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_grip_z );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_yaw );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_roll );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_flip_roll );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_scale );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_arm_cut );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_cut_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_lh_cut_yaw );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_cut_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_cut_yaw );
	gEngfuncs.Cmd_AddCommand( "vr_vm_ents", VR_ViewEnts_f, "hold a weapon that is not part of your body (branch, camera) and run this: lists the models near your hand" );
	gEngfuncs.Cmd_AddCommand( "vr_skyinfo", VR_SkyInfo_f, "print the sky the map asks for and any sky model entities, to work out how the map draws its sky" );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_models );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_distance );
	gEngfuncs.Cvar_RegisterVariable( &vr_aim_trace );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_yaw );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_roll );
	gEngfuncs.Cvar_RegisterVariable( &vr_testscene );
	gEngfuncs.Cvar_RegisterVariable( &vr_crouch_height );
	gEngfuncs.Cvar_RegisterVariable( &vr_crouch_comp );
	gEngfuncs.Cvar_RegisterVariable( &vr_hide_crosshair );
	gEngfuncs.Cvar_RegisterVariable( &vr_sky_dome );
	gEngfuncs.Cvar_RegisterVariable( &vr_roomscale );
	gEngfuncs.Cvar_RegisterVariable( &vr_room_deadzone );
	gEngfuncs.Cvar_RegisterVariable( &vr_hide_models );
	gEngfuncs.Cvar_RegisterVariable( &vr_hide_renderfx );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_line );
	gEngfuncs.Cvar_RegisterVariable( &vr_anim_still );
	gEngfuncs.Cvar_RegisterVariable( &vr_anim_end );
	gEngfuncs.Cvar_RegisterVariable( &vr_empty_hand );
	gEngfuncs.Cvar_RegisterVariable( &vr_room_maxlag );
	gEngfuncs.Cvar_RegisterVariable( &vr_eye_flush );
	gEngfuncs.Cvar_RegisterVariable( &vr_aim_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_aim_yaw );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_x );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_y );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_z );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_models );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_fov );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_follow );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_bone );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_debug );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_x );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_y );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_z );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_radius );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_cam );
	gEngfuncs.Cvar_RegisterVariable( &vr_scope_range );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_pitch );
	gEngfuncs.Cvar_RegisterVariable( &vr_light_yaw );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_mirror );
	gEngfuncs.Cvar_RegisterVariable( &vr_mirror_models );
	gEngfuncs.Cvar_RegisterVariable( &vr_eye_prepass );
	gEngfuncs.Cvar_RegisterVariable( &vr_vm_rot_local );
	gEngfuncs.Cvar_RegisterVariable( &vr_nofreeze_models );
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
	VR_CofInitCvars();
	VR_InputInitCvars();
	VR_ReloadInitCvars();
	VR_UIInitCvars();
	gEngfuncs.Cmd_AddCommand( "vr_vm_center", VR_Center_f, "put the middle of the weapon in your hand. vr_vm_center <size> also sizes it (size = length in units)" );
	gEngfuncs.Cmd_AddCommand( "vr_vm_info", VR_VMInfo_f, "print what the game says about the weapon model in hand (scale, size, auto-fit) - for weapons that look wrong" );
	gEngfuncs.Cmd_AddCommand( "vr_ents", VR_Ents_f, "list the things drawn in this scene (model, render fx, distance, size). \"vr_ents all\" lists everything" );
	gEngfuncs.Cmd_AddCommand( "vr_gl_info", VR_GLInfo_f, "print which OpenGL driver / opengl32.dll the game is using" );
	gEngfuncs.Cmd_AddCommand( "vr_weapon_reset", VR_WeaponReset_f, "put the weapon in hand back to the default position/size and overwrite its saved file" );
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
		vr.tan_frustum[eye][2] = vr_flip_vfov.value ? b : -t;
		vr.tan_frustum[eye][3] = vr_flip_vfov.value ? t : -b;

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
		vr.tan_frustum[eye][2] = vr_flip_vfov.value ? b : -t;
		vr.tan_frustum[eye][3] = vr_flip_vfov.value ? t : -b;

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

	{
		vec3_t hp, hf;

		VR_ConvPos( vr.head_pos, hp );
		VectorSubtract( hp, vr.ref_pos, hp );
		hp[2] += vr.crouch_comp;
		VR_RotateZ( hp, d, hp );
		VectorAdd( game->vieworigin, hp, vr.head_game );
		VR_RotateZ( vr.axis_fwd, d, hf );
		vr.head_game_yaw = RAD2DEG( atan2( hf[1], hf[0] ));
	}

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
		tmp[2] += vr.crouch_comp;
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
static qboolean vr_vm_omni, vr_vm_flare;	// flare or lantern in hand
static qboolean vr_vm_nofreeze;	// never held in the idle pose, whatever the lists say (camera, branch)
static qboolean vr_vm_static;	// the weapon in hand never animates (phone, syringe)
static qboolean vr_vm_light;	// the weapon in hand is a flashlight (the phone)
static qboolean vr_vm_melee;	// the weapon in hand is swung by hand
static qboolean vr_vm_scope;	// the weapon in hand has a scope
static char vr_tune_model[64];
static qboolean vr_vm_mirror_auto;
static qboolean vr_pose_axes_ok;	// the weapon can be drawn straight from its axes (no flip_roll)
// axes of the weapon in hand, from the last pose
static qboolean vr_pose_valid;
static vec3_t vr_pose_f, vr_pose_l, vr_pose_u;
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
static qboolean VR_TwoHandBasis( vec3_t f, vec3_t l, vec3_t u );

// the game's crosshair has no place in VR: the laser and the red cross are the aim
static void VR_HideCrosshair( void )
{
	static const char *names[] = { "crosshair", "cl_crosshair", "hud_crosshair" };
	int i;

	if( !vr_hide_crosshair.value )
		return;

	for( i = 0; i < 3; i++ )
	{
		cvar_t *cv = gEngfuncs.pfnGetCvarPointer( names[i] );

		if( cv && cv->value != 0.0f )
			gEngfuncs.Cvar_SetValue( names[i], 0.0f );
	}
}

static void VR_RotateAxis( vec3_t v, const vec3_t axis, float degrees );
static void VR_PublishDual( const ref_viewpass_t *game );

// How fast a hand moves (relative to the head, from the controller's own velocity: no smoothing, so a swing is seen
// at once) and where it hits. The trace starts at the tip of the weapon, not at the hand, and goes the way the hand
// moves: that is what gets hit, and what the game aims at while the swing goes on.
static float VR_SwingHand( int hand, const ref_viewpass_t *game, const vec3_t eye, float d, const char *cv_pitch, const char *cv_yaw, const char *cv_hit )
{
	uint32_t dev = VR_InputGetDevice( hand );
	vec3_t tmp, dir, tip, target;
	float speed = 0.0f, pitch, yaw;
	qboolean hit = false;

	if( !vr.hand[hand].valid || dev >= VR_MAX_DEVICES || !vr.poses[dev].bPoseIsValid )
	{
		gEngfuncs.Cvar_SetValue( cv_hit, 0.0f );
		return 0.0f;
	}

	{
		const float *hv = vr.poses[dev].vVelocity.v;
		const float *kv = vr.poses[VR_HMD_INDEX].vVelocity.v;
		float rel[3] = { hv[0] - kv[0], hv[1] - kv[1], hv[2] - kv[2] };

		speed = sqrt( rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] );

		if( speed > 0.3f )
		{
			VR_ConvDir( rel, tmp );
			VectorScale( tmp, 1.0f / speed, tmp );
			VR_RotateZ( tmp, d, dir );

			VectorMA( vr.hand[hand].pos, vr_melee_tip.value, vr.hand[hand].fwd, tip );
			hit = VR_TraceRay( tip, dir, vr_melee_reach.value, eye, target );
			VR_AimAngles( eye, target, game->viewangles[YAW], &pitch, &yaw );

			gEngfuncs.Cvar_SetValue( cv_pitch, pitch );
			gEngfuncs.Cvar_SetValue( cv_yaw, yaw );
		}
	}

	gEngfuncs.Cvar_SetValue( cv_hit, hit ? 1.0f : 0.0f );
	return speed;
}

static vec3_t vr_game_origin;
static qboolean vr_game_origin_ok;

// What may be drawn is decided from several places at once, not from one: the eye being drawn, the middle of your head
// and the game's own view point. In roomscale (or when you crouch, or lean over a low wall) these can sit in different
// leaves of the map, and a surface that is only visible from one of them used to be skipped: textures that vanish in
// one eye, parts of the world that disappear at random. The renderer merges the visibility of all of them
int VR_GetPvsOrigins( vec3_t out[3] )
{
	if( !vr.in_eye_pass )
		return 0;

	VectorCopy( vr.head_game, out[0] );

	if( vr_game_origin_ok )
	{
		VectorCopy( vr_game_origin, out[1] );
		return 2;
	}

	return 1;
}

static void VR_PublishAim( const ref_viewpass_t *game )
{
	VectorCopy( game->vieworigin, vr_game_origin );
	vr_game_origin_ok = true;

	float d = game->viewangles[YAW] - vr.ref_yaw;
	vec3_t eye, target, tmp, swing_dir, swing_target, aimdir;
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

	{
		vec3_t tf, tl, tu;

		if( !VR_TwoHandBasis( tf, tl, tu ))
		{
			VectorCopy( vr.hand[1].fwd, tf );
			VectorCopy( vr.hand[1].left, tl );
			VectorCopy( vr.hand[1].up, tu );
		}

		// vr_aim_pitch / vr_aim_yaw: where the weapon really shoots, when the way you hold it differs from the way it points
		VR_RotateAxis( tf, tu, vr_aim_yaw.value );
		VR_RotateAxis( tl, tu, vr_aim_yaw.value );
		VR_RotateAxis( tf, tl, vr_aim_pitch.value );
		VectorCopy( tf, aimdir );
	}

	if( vr_aim_trace.value )
	{
		vr_aim_hit = VR_TraceRay( vr.hand[1].pos, aimdir, 8192.0f, eye, target );
	}
	else
	{
		VectorScale( aimdir, vr_aim_converge.value, tmp );
		VectorAdd( vr.hand[1].pos, tmp, target );
	}

	VectorCopy( target, vr_aim_target );

	gEngfuncs.Cvar_SetValue( "vr_in_hit_x", target[0] );
	gEngfuncs.Cvar_SetValue( "vr_in_hit_y", target[1] );
	gEngfuncs.Cvar_SetValue( "vr_in_hit_z", target[2] );
	gEngfuncs.Cvar_SetValue( "vr_in_hit_ok", ( vr_aim_hit && vr_dbg_ent == 0 ) ? 1.0f : 0.0f );

	VR_AimAngles( eye, target, game->viewangles[YAW], &pitch, &yaw );

	gEngfuncs.Cvar_SetValue( "vr_in_hand_pitch", pitch );
	gEngfuncs.Cvar_SetValue( "vr_in_hand_yaw", yaw );
	gEngfuncs.Cvar_SetValue( "vr_in_hand_valid", 1.0f );

	// the plain direction the hand points, used by the use key
	{
		float dpitch, dyaw;

		VR_AimAngles( vec3_origin, vr.hand[1].fwd, game->viewangles[YAW], &dpitch, &dyaw );
		gEngfuncs.Cvar_SetValue( "vr_in_dir_pitch", dpitch );
		gEngfuncs.Cvar_SetValue( "vr_in_dir_yaw", dyaw );
	}

	// for the flashlight, which lives in the engine
	gEngfuncs.Cvar_SetValue( "vr_in_ray_x", vr.hand[1].pos[0] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_y", vr.hand[1].pos[1] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_z", vr.hand[1].pos[2] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_dx", vr.hand[1].fwd[0] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_dy", vr.hand[1].fwd[1] );
	gEngfuncs.Cvar_SetValue( "vr_in_ray_dz", vr.hand[1].fwd[2] );

	// swing: hand velocity relative to the head, so walking around does not count. Both hands: the right one swings the
	// weapon you hold, the left one throws punches or swings the weapon in the left hand
	speed = VR_SwingHand( 1, game, eye, d, "vr_in_swing_pitch", "vr_in_swing_yaw", "vr_in_swing_hit" );
	gEngfuncs.Cvar_SetValue( "vr_in_hand_speed", speed );
	gEngfuncs.Cvar_SetValue( "vr_in_lhand_speed", VR_SwingHand( 0, game, eye, d, "vr_in_lswing_pitch", "vr_in_lswing_yaw", "vr_in_lswing_hit" ));
	VR_PublishDual( game );

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

	static cvar_t *menu_var;

	if( !menu_var )
		menu_var = gEngfuncs.pfnGetCvarPointer( "vr_in_menu" );

	// the panel only matters while a menu is open, in the game you aim at the world
	vr.ui_hit = ( !menu_var || menu_var->value > 0.5f ) && vr.hand[1].valid
		&& VR_UIRayHit( vr.hand[1].origin_t, vr.hand[1].dir_t, &u, &v, &dist );
	vr.ui_dist = dist;
	VR_UISetPointer( vr.ui_hit, u, v );

	gEngfuncs.Cvar_SetValue( "vr_in_ui_hit", vr.ui_hit ? 1.0f : 0.0f );

	if( vr.ui_hit )
	{
		gEngfuncs.Cvar_SetValue( "vr_in_ui_x", u );
		gEngfuncs.Cvar_SetValue( "vr_in_ui_y", 1.0f - v );	// window coordinates start at the top
	}

	// the left hand points at the panel too (a second cursor)
	{
		float lu = 0.0f, lv = 0.0f, ld = 0.0f;
		qboolean lhit = ( !menu_var || menu_var->value > 0.5f ) && vr.hand[0].valid
			&& VR_UIRayHit( vr.hand[0].origin_t, vr.hand[0].dir_t, &lu, &lv, &ld );

		gEngfuncs.Cvar_SetValue( "vr_in_ui_lhit", lhit ? 1.0f : 0.0f );
		{
			static cvar_t *lkbd;

			if( !lkbd ) lkbd = gEngfuncs.pfnGetCvarPointer( "vr_in_ui_lkbd" );
			VR_UISetPointerLeft( lhit && lkbd && lkbd->value > 0.5f, lu, lv );
		}

		if( lhit )
		{
			gEngfuncs.Cvar_SetValue( "vr_in_ui_lx", lu );
			gEngfuncs.Cvar_SetValue( "vr_in_ui_ly", 1.0f - lv );
		}
	}
}

/*
=================
The weapon model (viewmodel) is drawn where the right hand is. Called by the renderer while it draws an eye.
The model's origin is around the player's eye, so shift it by where the hand grips the model.
=================
*/
// turn v around the unit axis by this many degrees (right hand rule)
static void VR_RotateAxis( vec3_t v, const vec3_t axis, float degrees )
{
	float c = cos( DEG2RAD( degrees )), s = sin( DEG2RAD( degrees ));
	vec3_t cr, out;

	if( degrees == 0.0f )
		return;

	CrossProduct( axis, v, cr );
	VectorScale( v, c, out );
	VectorMA( out, s, cr, out );
	VectorMA( out, DotProduct( axis, v ) * ( 1.0f - c ), axis, out );
	VectorCopy( out, v );
}

// two hands on a long gun: it points from the right hand to the left hand. returns the gun's axes
static qboolean VR_TwoHandBasis( vec3_t f, vec3_t l, vec3_t u )
{
	vec3_t d;
	float len;

	if( !VR_ReloadTwoHandAim() || !vr.hand[0].valid || !vr.hand[1].valid )
		return false;

	VectorSubtract( vr.hand[0].pos, vr.hand[1].pos, d );
	len = VectorLength( d );

	if( len < 6.0f )
		return false;

	VectorScale( d, 1.0f / len, f );

	// keep the right hand's roll: its up direction, made square with the new barrel direction
	VectorMA( vr.hand[1].up, -DotProduct( vr.hand[1].up, f ), f, u );

	if( VectorLength( u ) < 0.1f )
		return false;

	VectorNormalize( u );
	CrossProduct( u, f, l );
	VectorNormalize( l );
	return true;
}

// on a ladder or a boat the hands are not shown
qboolean VR_ClimbHide( void )
{
	static cvar_t *c;

	if( !c ) c = gEngfuncs.pfnGetCvarPointer( "vr_in_climb" );
	return c && c->value > 0.5f;
}

qboolean VR_TwoHandActive( void )
{
	vec3_t f, l, u;

	return VR_TwoHandBasis( f, l, u );
}

qboolean VR_GetViewModelPose( vec3_t origin, vec3_t angles )
{
	float xy, pitch, yaw, roll;
	float gx, gy, gz, rp, ry, rr;
	vec3_t bf, bl, bu;

	vr.vm_asked++;
	VR_UpdateWeaponProfile();

	if( !vr.in_eye_pass || !vr_hand_weapon.value || !vr.hand[1].valid )
		return false;

	vr.vm_applied++;
	vr_pose_valid = false;

	{
		// extra offsets while a manual reload is going on (vr_rl_tune 2)
		const float *rlo = VR_ReloadRightOffsets();

		gx = vr_vm_grip_x.value + rlo[0];
		gy = vr_vm_grip_y.value + rlo[1];
		gz = vr_vm_grip_z.value + rlo[2];
		rp = rlo[3]; ry = rlo[4]; rr = rlo[5];
	}

	// saved numbers from old experiments (grips of 100+ units) fling a weapon far away from the hand
	gx = bound( -40.0f, gx, 40.0f );
	gy = bound( -40.0f, gy, 40.0f );
	gz = bound( -40.0f, gz, 40.0f );

	if( !VR_TwoHandBasis( bf, bl, bu ))
	{
		VectorCopy( vr.hand[1].fwd, bf );
		VectorCopy( vr.hand[1].left, bl );
		VectorCopy( vr.hand[1].up, bu );
	}

	// the weapon's own yaw / pitch / roll are turned in the hand's own axes (adding them to world angles goes
	// wild when the hand is tilted): yaw around the hand's up, then pitch around its side, then roll along it
	if( vr_vm_rot_local.value )
	{
	VR_RotateAxis( bf, bu, vr_vm_yaw.value + ry );
	VR_RotateAxis( bl, bu, vr_vm_yaw.value + ry );
	VR_RotateAxis( bf, bl, vr_vm_pitch.value + rp );
	VR_RotateAxis( bu, bl, vr_vm_pitch.value + rp );
	}

	if( vr_vm_rot_local.value )
	{
		float rolloff = vr_vm_roll.value + rr;

		if( vr_vm_flip_roll.value )
			rolloff = -rolloff;

		VR_RotateAxis( bl, bf, rolloff );
		VR_RotateAxis( bu, bf, rolloff );
	}

	// the grip offsets are in the weapon's own frame, so yaw / pitch / roll turn the weapon around the spot you
	// hold it, not around the model's origin (that swung it away from the hand when the angle was large)
	VectorCopy( vr.hand[1].pos, origin );
	VectorMA( origin, -gx, bf, origin );
	VectorMA( origin, -gy, bl, origin );
	VectorMA( origin, -gz, bu, origin );

	// the weapon cannot go through walls: when its front would be inside one, it is pushed back along its own axis.
	// Only what you see moves, the aim still comes from the controller, so shots go where you point
	if( vr_hand_collide.value )
	{
		vec3_t reach_end;
		pmtrace_t wall;

		VectorMA( vr.hand[1].pos, vr_hand_reach.value, bf, reach_end );
		wall = gEngfuncs.CL_TraceLine( vr.hand[1].pos, reach_end, 0x00000008 /* PM_WORLD_ONLY */ );

		if( !wall.startsolid && wall.fraction < 1.0f )
			VectorMA( origin, -( 1.0f - wall.fraction ) * vr_hand_reach.value, bf, origin );
	}

	xy = sqrt( bf[0] * bf[0] + bf[1] * bf[1] );
	pitch = RAD2DEG( atan2( -bf[2], xy ));
	yaw = RAD2DEG( atan2( bf[1], bf[0] ));
	roll = RAD2DEG( atan2( bl[2], bu[2] ));

	if( vr_vm_flip_roll.value )
		roll = -roll;

	// the game stores the viewmodel's pitch negated, the studio renderer undoes that
	if( vr_vm_rot_local.value )
	{
		angles[PITCH] = -pitch;
		angles[YAW] = yaw;
		angles[ROLL] = roll;
	}
	else
	{
		angles[PITCH] = -( pitch + vr_vm_pitch.value + rp );
		angles[YAW] = yaw + vr_vm_yaw.value + ry;
		angles[ROLL] = roll + vr_vm_roll.value + rr;
	}

	VectorCopy( bf, vr_pose_f );
	VectorCopy( bl, vr_pose_l );
	VectorCopy( bu, vr_pose_u );
	vr_pose_valid = true;
	vr_pose_axes_ok = vr_vm_rot_local.value && !vr_vm_flip_roll.value;

	return true;
}

// ---- vr_vm_center: put the middle of the weapon in your hand, and optionally size it
static qboolean vr_center_wanted;
static float vr_center_fit;
static vec3_t vr_center_sum, vr_center_min, vr_center_max;
static int vr_center_n;

static void VR_Center_f( void )
{
	vr_center_wanted = true;
	vr_center_fit = gEngfuncs.Cmd_Argc() > 1 ? Q_atof( gEngfuncs.Cmd_Argv( 1 )) : 0.0f;
	vr_center_n = 0;
	VectorClear( vr_center_sum );
	gEngfuncs.Con_Printf( "VR: centering the weapon in your hand (run vr_weapon_save afterwards to keep it)\n" );
}

int VR_EyePrepass( void )
{
	int v = bound( 0, (int)vr_eye_prepass.value, 2 );

	// drawing an eye twice was a workaround for the DLL's one-shot pass; with the re-arm it only costs frame rate
	if( v && VR_CofFrameArmed() && !VR_CofKeepPrepass())
		return 0;

	return v;
}

qboolean VR_SkyModelsOn( void )
{
	// our own sky dome fights with the sky the game's client.dll draws (it clears the picture and ignores depth)
	if( VR_CofActive())
		return false;

	return vr_sky_dome.value != 0.0f;
}

qboolean VR_CenterWanted( void )
{
	return vr_center_wanted;
}

void VR_CenterAccum( const float *p )
{
	int i;

	if( !vr_center_n )
	{
		VectorCopy( p, vr_center_min );
		VectorCopy( p, vr_center_max );
	}

	for( i = 0; i < 3; i++ )
	{
		if( p[i] < vr_center_min[i] ) vr_center_min[i] = p[i];
		if( p[i] > vr_center_max[i] ) vr_center_max[i] = p[i];
	}

	VectorAdd( vr_center_sum, p, vr_center_sum );
	vr_center_n++;
}

void VR_CenterFinish( void )
{
	vec3_t c, d, ext;
	float size;

	if( !vr_center_wanted || vr_center_n < 8 || !vr.hand[1].valid )
		return;

	VectorScale( vr_center_sum, 1.0f / vr_center_n, c );
	VectorSubtract( vr.hand[1].pos, c, d );

	gEngfuncs.Cvar_SetValue( "vr_vm_grip_x", vr_vm_grip_x.value - DotProduct( d, vr.hand[1].fwd ));
	gEngfuncs.Cvar_SetValue( "vr_vm_grip_y", vr_vm_grip_y.value - DotProduct( d, vr.hand[1].left ));
	gEngfuncs.Cvar_SetValue( "vr_vm_grip_z", vr_vm_grip_z.value - DotProduct( d, vr.hand[1].up ));

	VectorSubtract( vr_center_max, vr_center_min, ext );
	size = VectorLength( ext );

	if( vr_center_fit > 0.0f && size > 0.1f )
	{
		gEngfuncs.Cvar_SetValue( "vr_vm_scale", bound( 0.02f, vr_vm_scale.value * vr_center_fit / size, 4.0f ));
		gEngfuncs.Con_Printf( "VR: weapon was %.1f units long, scale set. run vr_vm_center again once to re-center it\n", size );
	}
	else
	{
		gEngfuncs.Con_Printf( "VR: weapon centered (it is %.1f units across). vr_vm_center <size> also resizes it\n", size );
	}

	vr_center_wanted = false;
	vr_center_n = 0;
}

float VR_GetViewModelScale( void )
{
	// down to 0.002: some models (the camera) are hundreds of units large
	return bound( 0.002f, vr_vm_scale.value, 2.0f );
}

// the weapon's axes as they were set by the last pose: drawing from these avoids the trouble Euler angles
// give when the weapon points up or down (gimbal lock: it spins wildly when you move your hand)
void VR_MuzzleOffset( vec3_t att, const vec3_t f, const vec3_t l, const vec3_t u )
{
	VectorMA( att, vr_muzzle_x.value, f, att );
	VectorMA( att, vr_muzzle_y.value, l, att );
	VectorMA( att, vr_muzzle_z.value, u, att );
}

qboolean VR_GetViewModelAxes( vec3_t f, vec3_t l, vec3_t u )
{
	if( !vr_pose_valid || !vr_pose_axes_ok )
		return false;

	VectorCopy( vr_pose_f, f );
	VectorCopy( vr_pose_l, l );
	VectorCopy( vr_pose_u, u );
	return true;
}

// the same axes, whatever the rotation mode is (the muzzle offset must not depend on how the weapon is drawn)
qboolean VR_GetPoseAxes( vec3_t f, vec3_t l, vec3_t u )
{
	if( !vr_pose_valid )
		return false;

	VectorCopy( vr_pose_f, f );
	VectorCopy( vr_pose_l, l );
	VectorCopy( vr_pose_u, u );
	return true;
}

qboolean VR_ViewModelMirror( void )
{
	return vr_vm_mirror.value > 0.5f || ( vr_vm_mirror.value < 0.0f && vr_vm_mirror_auto );
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

		if( !n )
			continue;

		if( frag[0] == '^' )
		{
			// ^name: the name must start with it
			if( n > 1 && !Q_strnicmp( name, frag + 1, n - 1 ))
				return true;
		}
		else if( VR_ContainsNoCase( name, frag ))
		{
			return true;
		}
	}

#undef VR_IS_SEP
	return false;
}

static qboolean vr_left_pass;	// the universal left hand is being drawn

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
	float view[16], cut, cp, cy;
	vec3_t n, c, t;
	int hnd = vr_left_pass ? 0 : 1;

	if( !on || !vr.hand[hnd].valid )
	{
		pglDisable( GL_CLIP_PLANE0 );
		return;
	}

	cut = vr_left_pass ? vr_lh_arm_cut.value : vr_vm_arm_cut.value;
	cp = DEG2RAD( vr_left_pass ? vr_lh_cut_pitch.value : vr_vm_cut_pitch.value );
	cy = DEG2RAD( vr_left_pass ? vr_lh_cut_yaw.value : vr_vm_cut_yaw.value );

	// the cut plane faces along the hand, tilted by yaw (toward the left) and pitch (toward up)
	VectorScale( vr.hand[hnd].fwd, cos( cy ), t );
	VectorMA( t, sin( cy ), vr.hand[hnd].left, t );
	VectorScale( t, cos( cp ), n );
	VectorMA( n, sin( cp ), vr.hand[hnd].up, n );
	VectorMA( vr.hand[hnd].pos, -cut, n, c );

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

void VR_SetHandPass( qboolean left )
{
	vr_left_pass = left;
}

qboolean VR_ViewModelBoneHidden( const char *bonename )
{
	qboolean hidden = VR_ListMatches( vr_left_pass ? vr_lhand_hide_bones.string : vr_vm_hide_bones.string, bonename, false );

	// a loose shell part of the shotgun model
	if( !Q_stricmp( bonename, "lolshel" ))
		return true;

	// the flare model has a loose part (Object01) that floats beside it
	if( VR_ContainsNoCase( vr_tune_model, "flare" ) && !Q_stricmp( bonename, "Object01" ))
		return true;

	// a weapon the game holds in the left hand: show its left arm (mirrored it becomes the right one), hide the other.
	// What you put in vr_vm_hide_bones still hides those parts, only the default left arm entry is left out here
	if( !vr_left_pass && VR_ViewModelMirror( ))
	{
		char list[256], *p;

		Q_strncpy( list, vr_vm_hide_bones.string, sizeof( list ));
		while(( p = Q_strstr( list, "^l_" )) != NULL )
			memmove( p, p + 3, Q_strlen( p + 3 ) + 1 );

		hidden = !Q_strnicmp( bonename, "r_", 2 ) || ( list[0] && list[0] != '+' && VR_ListMatches( list, bonename, false ));
	}

	return VR_ReloadBoneHidden( vr_left_pass, bonename, hidden );
}

const char *VR_LeftHandModel( void )
{
	return vr_lhand_model.string;
}

float VR_GetLeftHandScale( void )
{
	return bound( 0.1f, vr_lh_scale.value, 4.0f );
}

// the universal left hand: the left arm of a viewmodel, at the left controller
qboolean VR_GetLeftHandPoseOfs( vec3_t origin, vec3_t angles, const float *ofs )
{
	vec3_t shift;
	float xy, pitch, yaw, roll;
	float o[6] = { 0, 0, 0, 0, 0, 0 };

	if( ofs )
		memcpy( o, ofs, sizeof( o ));

	if( !vr_lhand.value || !vr.in_eye_pass || !vr.hand[0].valid )
		return false;

	// dual wielding: the left hand holds its own weapon, the universal hand would sit on top of it
	if( vr_dual.value )
	{
		char dualmodel[64];

		if( VR_CofDualModel( dualmodel, sizeof( dualmodel )))
			return false;
	}

	VectorScale( vr.hand[0].fwd, vr_lh_grip_x.value + o[0], shift );
	VectorSubtract( vr.hand[0].pos, shift, origin );
	VectorScale( vr.hand[0].left, vr_lh_grip_y.value + o[1], shift );
	VectorSubtract( origin, shift, origin );
	VectorScale( vr.hand[0].up, vr_lh_grip_z.value + o[2], shift );
	VectorSubtract( origin, shift, origin );

	xy = sqrt( vr.hand[0].fwd[0] * vr.hand[0].fwd[0] + vr.hand[0].fwd[1] * vr.hand[0].fwd[1] );
	pitch = RAD2DEG( atan2( -vr.hand[0].fwd[2], xy ));
	yaw = RAD2DEG( atan2( vr.hand[0].fwd[1], vr.hand[0].fwd[0] ));
	roll = RAD2DEG( atan2( vr.hand[0].left[2], vr.hand[0].up[2] ));

	if( vr_lh_flip_roll.value )
		roll = -roll;

	angles[PITCH] = -( pitch + vr_lh_pitch.value + o[3] );
	angles[YAW] = yaw + vr_lh_yaw.value + o[4];
	angles[ROLL] = roll + vr_lh_roll.value + o[5];
	return true;
}

qboolean VR_GetLeftHandPose( vec3_t origin, vec3_t angles )
{
	return VR_GetLeftHandPoseOfs( origin, angles, NULL );
}

// a controller's position and basis in the game world (game units), for the reload system
qboolean VR_GetHandBasis( int hand, vec3_t pos, vec3_t fwd, vec3_t left, vec3_t up )
{
	if( hand < 0 || hand > 1 || !vr.hand[hand].valid )
		return false;

	VectorCopy( vr.hand[hand].pos, pos );
	VectorCopy( vr.hand[hand].fwd, fwd );
	VectorCopy( vr.hand[hand].left, left );
	VectorCopy( vr.hand[hand].up, up );
	return true;
}

void VR_GetHeadGame( vec3_t pos, float *yaw )
{
	VectorCopy( vr.head_game, pos );
	*yaw = vr.head_game_yaw;
}

qboolean VR_ViewModelFrozen( void )
{
	static cvar_t *whip;

	if( !whip )
		whip = gEngfuncs.pfnGetCvarPointer( "vr_in_whip" );

	// melee weapons, and any weapon while you pistol whip: your own arm does the swinging
	if( vr_vm_nofreeze )
		return whip && whip->value > 0.5f;

	return ( vr_vm_melee && vr_melee_freeze.value ) || vr_vm_static || ( whip && whip->value > 0.5f );
}

qboolean VR_ViewModelLock( int *seq, float *frame )
{
	if( vr_vm_lock_seq.value < 0.0f )
	{
		// empty hand: the fist is shown in one fixed pose, not in whatever frame its animation is on
		if( vr_empty_hand_seq.value >= 0.0f && VR_ContainsNoCase( vr_tune_model, "v_actions" ))
		{
			*seq = (int)vr_empty_hand_seq.value;
			*frame = vr_empty_hand_frame.value;
			return true;
		}

		return false;
	}

	*seq = (int)vr_vm_lock_seq.value;
	*frame = vr_vm_lock_frame.value;
	return true;
}

// weapons that hold still are shown in their idle pose: the same hand position every time
qboolean VR_IsIdleLabel( const char *label )
{
	return VR_ContainsNoCase( label, "idle" );
}

// some GL calls made on our behalf (SteamVR) can leave an error behind, which the engine then blames on the next texture it loads
void VR_DrainGLErrors( void )
{
	int i;

	for( i = 0; i < 8; i++ )
	{
		if( pglGetError() == GL_NO_ERROR )
			break;
	}
}

qboolean VR_FrameDone( void )
{
	return vr.frame_done;
}

qboolean VR_GetHandPose( int hand, float m[3][4], uint32_t *device )
{
	uint32_t dev = VR_InputGetDevice( hand );

	if( dev >= VR_MAX_DEVICES || !vr.poses[dev].bPoseIsValid )
		return false;

	memcpy( m, vr.poses[dev].mDeviceToAbsoluteTracking.m, sizeof( float ) * 12 );
	*device = dev;
	return true;
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
extern convar_t vr_rl_grip_radius, vr_rl_grip_x, vr_rl_grip_y, vr_rl_grip_z;

static convar_t *vr_tune_vars[] =
{
	&vr_vm_grip_x, &vr_vm_grip_y, &vr_vm_grip_z, &vr_vm_pitch, &vr_vm_yaw,
	&vr_vm_roll, &vr_vm_flip_roll, &vr_vm_scale, &vr_hand_pitch, &vr_vm_arm_cut, &vr_vm_cut_pitch, &vr_vm_cut_yaw,
	&vr_light_x, &vr_light_y, &vr_light_z, &vr_scope_x, &vr_scope_y, &vr_scope_z, &vr_scope_radius, &vr_scope_fov,
	&vr_light_pitch, &vr_light_yaw, &vr_vm_mirror, &vr_aim_pitch, &vr_aim_yaw, &vr_anim_mask, &vr_rl_grip_radius, &vr_rl_grip_x, &vr_rl_grip_y, &vr_rl_grip_z, &vr_muzzle_x, &vr_muzzle_y, &vr_muzzle_z
};

#define VR_TUNE_COUNT ( (int)( sizeof( vr_tune_vars ) / sizeof( vr_tune_vars[0] )))

static float vr_tune_base[64];
static qboolean vr_tune_have_base;

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

// the settings every weapon starts from: the defaults in this file. a weapon without a saved file gets exactly these
static void VR_CaptureTuneDefaults( void )
{
	int i;

	if( vr_tune_have_base )
		return;

	for( i = 0; i < VR_TUNE_COUNT; i++ )
		vr_tune_base[i] = Q_atof( vr_tune_vars[i]->string );

	vr_tune_have_base = true;
}

// built-in weapon profiles (the defaults for every install). A file vr_weapon_<model>.cfg in the game folder still overrides them.
static const char *vr_builtin_profiles[][2] =
{
	{ "v_actions", "vr_vm_grip_x -12\nvr_vm_grip_y -20\nvr_vm_grip_z -11\nvr_vm_pitch -70\nvr_vm_yaw 80\nvr_vm_roll -10\nvr_vm_flip_roll 0\nvr_vm_scale 0.19\nvr_hand_pitch 60\nvr_vm_arm_cut 6\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_branch", "vr_vm_grip_x 13\nvr_vm_grip_y -6.5\nvr_vm_grip_z -8\nvr_vm_pitch 7\nvr_vm_yaw 0\nvr_vm_roll 70\nvr_vm_flip_roll 0\nvr_vm_scale 0.18\nvr_hand_pitch 60\nvr_vm_arm_cut 6\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0\nvr_scope_y 0\nvr_scope_z 3\nvr_scope_radius 1.2\nvr_scope_fov 10\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_camera", "vr_vm_grip_x 10\nvr_vm_grip_y -5\nvr_vm_grip_z -3\nvr_vm_pitch 60\nvr_vm_yaw 0\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_aim_pitch 70\nvr_vm_scale 0.02\nvr_hand_pitch 60\nvr_vm_arm_cut 20\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_flare", "vr_vm_grip_x 12\nvr_vm_grip_y -4\nvr_vm_grip_z -5\nvr_vm_pitch 0\nvr_vm_yaw 10\nvr_vm_roll 60\nvr_vm_flip_roll 0\nvr_vm_scale 0.5\nvr_hand_pitch 60\nvr_vm_arm_cut 7\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_flashlight", "vr_vm_grip_x 8\nvr_vm_grip_y -4\nvr_vm_grip_z -5\nvr_vm_pitch 100\nvr_vm_yaw 0\nvr_vm_roll 70\nvr_vm_flip_roll 0\nvr_vm_scale 1.2\nvr_hand_pitch 60\nvr_vm_arm_cut 6\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 100\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_g43", "vr_vm_grip_x 4\nvr_vm_grip_y -1.5\nvr_vm_grip_z -2\nvr_vm_pitch 0\nvr_vm_yaw 2.3\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.45\nvr_hand_pitch 60\nvr_vm_arm_cut 6\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_glock", "vr_vm_grip_x 13\nvr_vm_grip_y -4\nvr_vm_grip_z -3\nvr_vm_pitch 0\nvr_vm_yaw 0\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.5\nvr_hand_pitch 60\nvr_vm_arm_cut 6\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_lantern", "vr_vm_grip_x 9\nvr_vm_grip_y 3\nvr_vm_grip_z -2\nvr_vm_pitch 20\nvr_vm_yaw 10\nvr_vm_roll 20\nvr_vm_flip_roll 0\nvr_vm_scale 0.45\nvr_hand_pitch 60\nvr_vm_arm_cut 7\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_m16", "vr_vm_grip_x 6\nvr_vm_grip_y -2.5\nvr_vm_grip_z -4\nvr_vm_pitch 0\nvr_vm_yaw 0\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.45\nvr_hand_pitch 60\nvr_vm_arm_cut 6\n" },
	{ "v_mobile", "vr_vm_grip_x 8\nvr_vm_grip_y -4\nvr_vm_grip_z 2\nvr_vm_pitch 0\nvr_vm_yaw -60\nvr_vm_roll -40\nvr_vm_flip_roll 0\nvr_vm_scale 0.6\nvr_hand_pitch 60\nvr_vm_arm_cut 6\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch -10\nvr_light_yaw -80\nvr_vm_mirror -1\n" },
	{ "v_mp5", "vr_vm_grip_x 10\nvr_vm_grip_y -4\nvr_vm_grip_z -5\nvr_vm_pitch 5\nvr_vm_yaw -4\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.4\nvr_hand_pitch 60\nvr_vm_arm_cut 6\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 40\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_nightstick", "vr_vm_grip_x 8\nvr_vm_grip_y -3\nvr_vm_grip_z -3\nvr_vm_pitch 95\nvr_vm_yaw -150\nvr_vm_roll -90\nvr_vm_flip_roll 0\nvr_vm_scale 0.45\nvr_hand_pitch 60\nvr_vm_arm_cut 5\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_p345", "vr_vm_grip_x 14\nvr_vm_grip_y -4\nvr_vm_grip_z -3\nvr_vm_pitch 0\nvr_vm_yaw 0\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.5\nvr_hand_pitch 60\nvr_vm_arm_cut 6\n" },
	{ "v_revolver", "vr_vm_grip_x 13\nvr_vm_grip_y -4\nvr_vm_grip_z -3\nvr_vm_pitch 0\nvr_vm_yaw 0\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.5\nvr_hand_pitch 60\nvr_vm_arm_cut 6\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_rifle", "vr_vm_grip_x 3\nvr_vm_grip_y -2\nvr_vm_grip_z -3\nvr_vm_pitch 0\nvr_vm_yaw 0\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.3\nvr_hand_pitch 60\nvr_vm_arm_cut 6\n" },
	{ "v_shotgun", "vr_vm_grip_x 5\nvr_vm_grip_y -3\nvr_vm_grip_z -4\nvr_vm_pitch 0\nvr_vm_yaw 0\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.5\nvr_hand_pitch 60\nvr_vm_arm_cut 7\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\n" },
	{ "v_sledgehammer", "vr_vm_grip_x 11\nvr_vm_grip_y -6\nvr_vm_grip_z -3\nvr_vm_pitch 40\nvr_vm_yaw 0\nvr_vm_roll -40\nvr_vm_flip_roll 0\nvr_vm_scale 0.6\nvr_hand_pitch 60\nvr_vm_arm_cut 7\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_switchblade", "vr_vm_grip_x 12\nvr_vm_grip_y -3\nvr_vm_grip_z -4.5\nvr_vm_pitch 0\nvr_vm_yaw 20\nvr_vm_roll 70\nvr_vm_flip_roll 0\nvr_vm_scale 0.45\nvr_hand_pitch 60\nvr_vm_arm_cut 5\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_syringe", "vr_vm_grip_x 13\nvr_vm_grip_y -3\nvr_vm_grip_z -3\nvr_vm_pitch 50\nvr_vm_yaw 20\nvr_vm_roll 70\nvr_vm_flip_roll 0\nvr_vm_scale 0.7\nvr_hand_pitch 60\nvr_vm_arm_cut 6\nvr_vm_cut_pitch 0\nvr_vm_cut_yaw 0\nvr_light_x 6\nvr_light_y 0\nvr_light_z 0\nvr_scope_x 0.4\nvr_scope_y -0.3\nvr_scope_z 2.5\nvr_scope_radius 0.4\nvr_scope_fov 2\nvr_light_pitch 0\nvr_light_yaw 0\nvr_vm_mirror -1\n" },
	{ "v_vp70", "vr_vm_grip_x 16\nvr_vm_grip_y -4\nvr_vm_grip_z -3\nvr_vm_pitch 0\nvr_vm_yaw 0\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.5\nvr_hand_pitch 60\nvr_vm_arm_cut 6\n" },
	{ "v_wheelchair_browning", "vr_vm_grip_x 17\nvr_vm_grip_y -4\nvr_vm_grip_z -4\nvr_vm_pitch 0\nvr_vm_yaw -10\nvr_vm_roll 0\nvr_vm_flip_roll 0\nvr_vm_scale 0.7\nvr_hand_pitch 60\nvr_vm_arm_cut 6\n" },
};

// set the numbers of a built-in profile (cvar name and value per line)

// ---- dual wielding: the weapon in the left hand
// The game draws it itself (client.dll, its "secondary view"), so all we do is give it the pose of the left hand,
// worked out from the numbers of that weapon (the built in profile), mirrored for the left hand, plus vr_dual_*.
static qboolean VR_TextNumber( const char *text, const char *key, float *out )
{
	const char *p = text;
	int n = (int)Q_strlen( key );

	while( p && *p )
	{
		if( !Q_strncmp( p, key, n ) && ( p[n] == ' ' || p[n] == '\t' ))
		{
			p += n;
			while( *p == ' ' || *p == '\t' || *p == '"' ) p++;
			*out = Q_atof( p );
			return true;
		}

		while( *p && *p != '\n' ) p++;
		while( *p == '\n' || *p == '\r' ) p++;
	}

	return false;
}

static float VR_WeaponNumber( const char *base, const char *key, float def )
{
	float v = def;
	int i;

	for( i = 0; i < (int)( sizeof( vr_builtin_profiles ) / sizeof( vr_builtin_profiles[0] )); i++ )
	{
		if( !Q_stricmp( vr_builtin_profiles[i][0], base ))
		{
			VR_TextNumber( vr_builtin_profiles[i][1], key, &v );
			break;
		}
	}

	return v;
}

static qboolean VR_GetDualPose( const char *model, vec3_t origin, float *vang, float *scale )
{
	char base[48];
	float gx, gy, gz, wp, wy, wr, xy, pitch, yaw, roll;
	vec3_t bf, bl, bu;

	if( !vr.in_eye_pass || !vr.hand[0].valid )
		return false;

	VR_ModelBaseName( model, base, sizeof( base ));

	gx = bound( -40.0f, VR_WeaponNumber( base, "vr_vm_grip_x", 0.0f ) + vr_dual_x.value, 40.0f );
	gy = bound( -40.0f, VR_WeaponNumber( base, "vr_vm_grip_y", 0.0f ) + vr_dual_y.value, 40.0f );
	gz = bound( -40.0f, VR_WeaponNumber( base, "vr_vm_grip_z", 0.0f ) + vr_dual_z.value, 40.0f );
	wp = VR_WeaponNumber( base, "vr_vm_pitch", 0.0f ) + vr_dual_pitch.value;
	wy = VR_WeaponNumber( base, "vr_vm_yaw", 0.0f ) + vr_dual_yaw.value;
	wr = VR_WeaponNumber( base, "vr_vm_roll", 0.0f ) + vr_dual_roll.value;
	*scale = VR_WeaponNumber( base, "vr_vm_scale", 0.5f ) * vr_dual_scale.value;

	// the other hand: the same hold, reflected across the body's middle plane
	gy = -gy;
	wy = -wy;
	wr = -wr;

	VectorCopy( vr.hand[0].fwd, bf );
	VectorCopy( vr.hand[0].left, bl );
	VectorCopy( vr.hand[0].up, bu );

	VR_RotateAxis( bf, bu, wy );
	VR_RotateAxis( bl, bu, wy );
	VR_RotateAxis( bf, bl, wp );
	VR_RotateAxis( bu, bl, wp );
	VR_RotateAxis( bl, bf, wr );
	VR_RotateAxis( bu, bf, wr );

	VectorCopy( vr.hand[0].pos, origin );
	VectorMA( origin, -gx, bf, origin );
	VectorMA( origin, -gy, bl, origin );
	VectorMA( origin, -gz, bu, origin );

	xy = sqrt( bf[0] * bf[0] + bf[1] * bf[1] );
	pitch = RAD2DEG( atan2( -bf[2], xy ));
	yaw = RAD2DEG( atan2( bf[1], bf[0] ));
	roll = RAD2DEG( atan2( bl[2], bu[2] ));

	// the game's view angles: pitch positive looks down. It turns them into the entity angles itself
	vang[0] = pitch;
	vang[1] = yaw;
	vang[2] = roll;
	return true;
}

void VR_DrawDualWeapon( void )
{
	char model[64];
	vec3_t origin;
	float vang[3], scale;

	if( !vr_dual.value || !vr.in_eye_pass || !VR_CofDualModel( model, sizeof( model )))
		return;

	if( !VR_GetDualPose( model, origin, vang, &scale ))
		return;

	// like the weapon in the right hand: it is drawn in front of the world, so it does not go into walls
	pglDepthRange( gldepthmin, gldepthmin + 0.3f * ( gldepthmax - gldepthmin ));
	pglMatrixMode( GL_MODELVIEW );
	pglPushMatrix();
	pglTranslatef( origin[0], origin[1], origin[2] );
	pglScalef( scale, scale, scale );
	pglTranslatef( -origin[0], -origin[1], -origin[2] );

	// the engine's own studio renderer honours the angles we give; the game's renderer for this second weapon
	// followed the arm but did not turn with the hand
	R_StudioForceBuiltin( vr_dual_builtin.value != 0.0f );
	VR_CofDrawDual( origin, vang );
	R_StudioForceBuiltin( false );

	pglPopMatrix();
	pglDepthRange( gldepthmin, gldepthmax );
}

// tell the engine whether we are dual wielding, and where the left hand shoots
static void VR_PublishDual( const ref_viewpass_t *game )
{
	char model[64], base[48];
	qboolean dual = VR_CofDualModel( model, sizeof( model ));

	gEngfuncs.Cvar_SetValue( "vr_in_dual", dual ? 1.0f : 0.0f );

	if( dual )
	{
		vec3_t eye, target;
		float p, y;

		VR_ModelBaseName( model, base, sizeof( base ));
		gEngfuncs.Cvar_SetValue( "vr_in_dual_lmelee", VR_ListMatches( vr_melee_models.string, base, true ) ? 1.0f : 0.0f );

		if( vr.hand[0].valid )
		{
			VectorCopy( game->vieworigin, eye );
			VR_TraceRay( vr.hand[0].pos, vr.hand[0].fwd, 8192.0f, eye, target );
			VR_AimAngles( eye, target, game->viewangles[YAW], &p, &y );
			gEngfuncs.Cvar_SetValue( "vr_in_lhand_pitch", p );
			gEngfuncs.Cvar_SetValue( "vr_in_lhand_yaw", y );
		}
	}
	else
	{
		gEngfuncs.Cvar_SetValue( "vr_in_dual_lmelee", 0.0f );
	}
}


static void VR_ApplyBuiltinProfile( const char *base )
{
	int i;

	for( i = 0; i < (int)( sizeof( vr_builtin_profiles ) / sizeof( vr_builtin_profiles[0] )); i++ )
	{
		const char *p;

		if( Q_stricmp( vr_builtin_profiles[i][0], base ))
			continue;

		p = vr_builtin_profiles[i][1];

		while( *p )
		{
			char name[48], value[24];
			int n = 0, m = 0;

			while( *p && *p != ' ' && n < 47 ) name[n++] = *p++;
			name[n] = 0;
			while( *p == ' ' ) p++;
			while( *p && *p != '\n' && m < 23 ) value[m++] = *p++;
			value[m] = 0;
			while( *p == '\n' ) p++;

			if( n && m )
				gEngfuncs.Cvar_SetValue( name, Q_atof( value ));
		}

		return;
	}
}

// does this animation play? Only shooting and reloading do, unless the weapon has its own list (vr_anim_mask)
qboolean VR_AnimPlays( int seq, const char *label )
{
	int mask = (int)vr_anim_mask.value;

	if( mask != 0 )
		return seq >= 0 && seq < 24 && FBitSet( mask, 1 << seq );

	return label && label[0] && VR_ListMatches( vr_anim_play.string, label, true );
}

int VR_AnimMask( void )
{
	return (int)vr_anim_mask.value;
}

void VR_AnimSetMask( int mask )
{
	gEngfuncs.Cvar_SetValue( "vr_anim_mask", mask );
}

// 0 = play the animation, 1 = hold its first frame, 2 = show its last frame
int VR_AnimOverride( const char *label )
{
	if( !label || !label[0] )
		return 0;

	if( VR_ListMatches( vr_anim_still.string, label, true ))
		return 1;

	if( VR_ListMatches( vr_anim_end.string, label, true ))
		return 2;

	return 0;
}

qboolean VR_HandReady( void )
{
	return vr.in_eye_pass && vr.hand[1].valid && vr_hand_weapon.value;
}

const char *VR_EmptyHandName( void )
{
	return vr_empty_hand.string;
}

static void VR_UpdateWeaponProfile( void )
{
	const char *model;
	char file[96], cmd[128];
	int i;

	if( !tr.viewent || !tr.viewent->model )
	{
		// nothing in hand (new map, menu): forget the weapon, so a stale light cannot stay on
		vr_tune_model[0] = 0;
		vr_vm_light = vr_vm_omni = vr_vm_flare = vr_vm_scope = false;
		return;
	}

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
		vr_vm_static = VR_ListMatches( vr_static_models.string, base, true );
		vr_vm_nofreeze = VR_ListMatches( vr_nofreeze_models.string, base, true );
		vr_vm_omni = VR_ListMatches( vr_omni_models.string, base, true );
		vr_vm_flare = VR_ContainsNoCase( base, "flare" );
		vr_vm_scope = VR_ListMatches( vr_scope_models.string, base, true );
		vr_vm_mirror_auto = VR_ListMatches( vr_mirror_models.string, base, true );
		VR_ApplyBuiltinProfile( base );
		gEngfuncs.Cvar_SetValue( "vr_in_melee", vr_vm_melee ? 1.0f : 0.0f );
		gEngfuncs.Cvar_SetValue( "vr_in_fists", VR_ContainsNoCase( base, "v_actions" ) ? 1.0f : 0.0f );
		// the left hand throws the punches when the right hand holds a one handed item or nothing
		gEngfuncs.Cvar_SetValue( "vr_in_oneh", ( VR_ContainsNoCase( base, "mobile" ) || vr_vm_omni ) ? 1.0f : 0.0f );
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
	char text[1024], file[96];
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

static void VR_WeaponReset_f( void )
{
	char text[1024], file[96];
	int i, len = 0;

	if( !vr_tune_model[0] )
	{
		gEngfuncs.Con_Printf( "VR: hold a weapon first\n" );
		return;
	}

	// back to the defaults, and overwrite the saved file so old (broken) numbers are gone
	for( i = 0; i < VR_TUNE_COUNT; i++ )
		gEngfuncs.Cvar_SetValue( vr_tune_vars[i]->name, vr_tune_base[i] );

	VR_WeaponFileName( vr_tune_model, file, sizeof( file ));

	for( i = 0; i < VR_TUNE_COUNT; i++ )
		len += Q_snprintf( text + len, sizeof( text ) - len, "%s %g\n", vr_tune_vars[i]->name, vr_tune_base[i] );

	gEngfuncs.fsapi->WriteFile( file, text, len );
	gEngfuncs.Con_Printf( "VR: %s is back to the defaults (and %s was overwritten)\n", vr_tune_model, file );
}

static void VR_VMInfo_f( void )
{
	cl_entity_t *v = tr.viewent;

	if( !v || !v->model )
	{
		gEngfuncs.Con_Printf( "VR: no weapon model\n" );
		return;
	}

	gEngfuncs.Con_Printf( "weapon model %s: bone-space size %.1f (glock is 24), scale used %.3f, mirror %s\n", v->model->name,
		R_StudioModelRadius( v->model ), VR_GetViewModelScale(), VR_ViewModelMirror() ? "yes" : "no" );
}

// entities that are not drawn: by model name (vr_hide_models) or render fx number (vr_hide_renderfx)
qboolean VR_EntityHidden( const cl_entity_t *e )
{
	const char *p;

	if( !e || ( !vr_hide_models.string[0] && !vr_hide_renderfx.string[0] ))
		return false;

	if( e->model && vr_hide_models.string[0] && VR_ListMatches( vr_hide_models.string, e->model->name, true ))
		return true;

	p = vr_hide_renderfx.string;

	while( *p )
	{
		while( *p == '+' || *p == ' ' || *p == ',' ) p++;

		if( *p >= '0' && *p <= '9' )
		{
			int v = atoi( p );

			while( *p >= '0' && *p <= '9' ) p++;

			if( e->curstate.renderfx == v && e != tr.viewent )
				return true;
		}
		else if( *p ) p++;
	}

	return false;
}

static void VR_Ents_f( void )
{
	int n, i, shown = 0;
	qboolean all = gEngfuncs.Cmd_Argc() > 1 && !Q_stricmp( gEngfuncs.Cmd_Argv( 1 ), "all" );

	if( !tr.draw_list )
	{
		gEngfuncs.Con_Printf( "VR: nothing drawn yet\n" );
		return;
	}

	n = tr.draw_list->num_solid_entities + tr.draw_list->num_trans_entities;
	gEngfuncs.Con_Printf( "%d entities drawn (showing %s). model, render fx, render mode, distance, size\n", n, all ? "all" : "custom render fx or very big ones" );

	for( i = 0; i < n; i++ )
	{
		cl_entity_t *e = i < tr.draw_list->num_solid_entities ? tr.draw_list->solid_entities[i] : tr.draw_list->trans_entities[i - tr.draw_list->num_solid_entities];
		vec3_t d;
		float rad = 0.0f;
		int k;

		if( !e || !e->model )
			continue;

		for( k = 0; k < 3; k++ )
			rad = Q_max( rad, Q_max( fabs( e->model->mins[k] ), fabs( e->model->maxs[k] )));

		if( e->model->type == mod_studio )
			rad = Q_max( rad, R_StudioModelRadius( e->model ));

		if( !all && e->curstate.renderfx <= 21 && rad < 1500.0f )
			continue;

		VectorSubtract( e->origin, RI.rvp.vieworigin, d );
		gEngfuncs.Con_Printf( "  %s: ent %d, renderfx %d, rendermode %d, %.0f away, size %.0f\n", e->model->name, e->index,
			e->curstate.renderfx, e->curstate.rendermode, VectorLength( d ), rad );

		if( ++shown >= 120 )
			break;
	}
}

static void VR_GLInfo_f( void )
{
	gEngfuncs.Con_Printf( "GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\n", glConfig.vendor_string, glConfig.renderer_string, glConfig.version_string );

#if defined( _WIN32 )
	{
		char path[260] = "";
		HMODULE m = GetModuleHandleA( "opengl32.dll" );

		if( m && GetModuleFileNameA( m, path, sizeof( path )))
			gEngfuncs.Con_Printf( "opengl32.dll loaded from: %s\n", path );
		else gEngfuncs.Con_Printf( "opengl32.dll: not found in the process\n" );
	}
#endif
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

// vr_vm_ents: hold the branch or the camera and run this. lists every model near your hand
static void VR_ViewEnts_f( void )
{
	int i, n;

	if( !tr.draw_list || !vr.hand[1].valid )
		return;

	n = tr.draw_list->num_solid_entities + tr.draw_list->num_trans_entities;
	gEngfuncs.Con_Printf( "viewmodel entity: %s\n", tr.viewent && tr.viewent->model ? tr.viewent->model->name : "(none)" );

	for( i = 0; i < n; i++ )
	{
		cl_entity_t *e = i < tr.draw_list->num_solid_entities ? tr.draw_list->solid_entities[i]
			: tr.draw_list->trans_entities[i - tr.draw_list->num_solid_entities];
		vec3_t d;

		if( !e || !e->model || e->model->type != mod_studio )
			continue;

		VectorSubtract( e->origin, vr.hand[1].pos, d );

		if( VectorLength( d ) > 120.0f )
			continue;

		gEngfuncs.Con_Printf( "  %s: ent %d, %.0f units from your hand, movetype %d, aiment %d, scale %.2f, rendermode %d, renderfx %d, seq %d\n",
			e->model->name, e->index, VectorLength( d ), e->curstate.movetype, e->curstate.aiment, e->curstate.scale,
			e->curstate.rendermode, e->curstate.renderfx, e->curstate.sequence );
	}
}

// vr_skyinfo: which sky does the map ask for, and which sky models are in the scene?
static void VR_SkyInfo_f( void )
{
	int i, n, count = 0;

	gEngfuncs.Con_Printf( "sky texture asked for by the map: '%s'\n", gp_movevars->skyName );

	if( !tr.draw_list )
		return;

	n = tr.draw_list->num_solid_entities + tr.draw_list->num_trans_entities;

	for( i = 0; i < n; i++ )
	{
		cl_entity_t *e = i < tr.draw_list->num_solid_entities ? tr.draw_list->solid_entities[i]
			: tr.draw_list->trans_entities[i - tr.draw_list->num_solid_entities];

		if( !e || !e->model || !VR_ContainsNoCase( e->model->name, "sky" ))
			continue;

		count++;
		gEngfuncs.Con_Printf( "  %s: ent %d, origin %.0f %.0f %.0f, rendermode %d, renderfx %d, renderamt %d, scale %.2f\n", e->model->name, e->index,
			e->origin[0], e->origin[1], e->origin[2], e->curstate.rendermode, e->curstate.renderfx, e->curstate.renderamt, e->curstate.scale );
	}

	gEngfuncs.Con_Printf( "%d sky model entities in the scene (of %d)\n", count, n );
}

// ---- the held phone / flashlight / flare / lantern
// The game (client.dll) makes its own light for these, but at your head. Here we only work out where
// your hand puts it; gl_vr_cof.c moves the game's own light there before each picture is drawn.
static void VR_UpdateHandLight( const ref_viewpass_t *game )
{
	vec3_t start, dir, side, up, end, spot;

	if( !vr_phone_light.value || !vr.hand[1].valid
		|| !tr.viewent || !tr.viewent->model || Q_strcmp( tr.viewent->model->name, vr_tune_model ))
	{
		VR_CofSetHandLight( false, false, false, NULL, NULL, NULL );
		return;
	}

	// the light can be moved (vr_light_x/y/z) and aimed away from where the controller points (vr_light_pitch / vr_light_yaw)
	VectorCopy( vr.hand[1].fwd, dir );
	VectorCopy( vr.hand[1].left, side );
	VectorCopy( vr.hand[1].up, up );
	VR_RotateAxis( dir, up, vr_light_yaw.value );
	VR_RotateAxis( side, up, vr_light_yaw.value );
	VR_RotateAxis( dir, side, vr_light_pitch.value );

	if( vr_pose_valid )
	{
		VectorMA( vr.hand[1].pos, vr_light_x.value, vr_pose_f, start );
		VectorMA( start, vr_light_y.value, vr_pose_l, start );
		VectorMA( start, vr_light_z.value, vr_pose_u, start );
	}
	else
	{
		VectorMA( vr.hand[1].pos, vr_light_x.value, vr.hand[1].fwd, start );
		VectorMA( start, vr_light_y.value, vr.hand[1].left, start );
		VectorMA( start, vr_light_z.value, vr.hand[1].up, start );
	}

	// where the beam lands, for the game's point lights that sit on the wall the beam hits
	if( VR_TraceRay( start, dir, vr_light_distance.value, game->vieworigin, end ))
		VectorMA( end, -8.0f, dir, spot );
	else VectorCopy( end, spot );

	VR_CofSetHandLight( true, vr_vm_omni, VR_ContainsNoCase( vr_tune_model, "mobile" ), start, dir, spot );
}

/*
=================
Roomscale: when you step away from the spot you were recentered on, your character walks there too.
The renderer measures the distance (vr_in_room_x/y, game axes), the engine turns it into walking
(in_vr.c). While your character catches up, the view is kept exactly where your head is: the part of
the walk that really happened is taken off the distance, so nothing visibly slides.
=================
*/
static void VR_RoomFollow( const ref_viewpass_t *game )
{
	static vec3_t prev_vo, last_dir;
	static qboolean prev_ok, following;
	static float last_cmd_time;
	vec3_t hp, dp;
	float d, len;

	if( !vr_roomscale.value || !vr_positional.value || !vr.have_ref )
	{
		prev_ok = following = false;
		gEngfuncs.Cvar_SetValue( "vr_in_room_x", 0.0f );
		gEngfuncs.Cvar_SetValue( "vr_in_room_y", 0.0f );
		return;
	}

	d = game->viewangles[YAW] - vr.ref_yaw;

	// the part of the walk we asked for: take it off the distance, so the view stays where the head is
	if( prev_ok && gp_cl->time < last_cmd_time + 0.35f && gp_cl->time >= last_cmd_time )
	{
		VectorSubtract( game->vieworigin, prev_vo, dp );
		dp[2] = 0.0f;

		if( VectorLength( dp ) < 80.0f )
		{
			float moved = DotProduct( dp, last_dir );

			if( moved > 0.0f )
			{
				vec3_t shift;

				VectorScale( last_dir, moved, shift );
				VR_RotateZ( shift, -d, shift );
				vr.ref_pos[0] += shift[0];
				vr.ref_pos[1] += shift[1];
			}
		}
	}

	VectorCopy( game->vieworigin, prev_vo );
	prev_ok = true;

	// how far is the head from the spot we recentered on (in game axes)?
	VR_ConvPos( vr.head_pos, hp );
	VectorSubtract( hp, vr.ref_pos, hp );
	VR_RotateZ( hp, d, hp );
	hp[2] = 0.0f;
	len = VectorLength( hp );

	// the body is stuck (a wall): the view is pulled back to a small distance from it, you cannot look through the wall
	if( vr_room_maxlag.value > 0.0f && len > vr_room_maxlag.value )
	{
		vec3_t push;

		VectorScale( hp, ( len - vr_room_maxlag.value ) / len, push );
		VR_RotateZ( push, -d, push );
		vr.ref_pos[0] += push[0];
		vr.ref_pos[1] += push[1];
		VectorScale( hp, vr_room_maxlag.value / len, hp );
		len = vr_room_maxlag.value;
	}

	if( !following && len > vr_room_deadzone.value )
		following = true;
	else if( following && len < vr_room_deadzone.value * 0.35f )
		following = false;

	if( following && len > 0.001f )
	{
		VectorScale( hp, 1.0f / len, last_dir );
		last_cmd_time = gp_cl->time;
		gEngfuncs.Cvar_SetValue( "vr_in_room_x", hp[0] );
		gEngfuncs.Cvar_SetValue( "vr_in_room_y", hp[1] );
	}
	else
	{
		gEngfuncs.Cvar_SetValue( "vr_in_room_x", 0.0f );
		gEngfuncs.Cvar_SetValue( "vr_in_room_y", 0.0f );
	}
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

	VR_RoomFollow( game );

	{
		float rel = RAD2DEG( atan2( vr.axis_fwd[1], vr.axis_fwd[0] )) - vr.ref_yaw;
		float xy = sqrt( vr.axis_fwd[0] * vr.axis_fwd[0] + vr.axis_fwd[1] * vr.axis_fwd[1] );
		float pitch = RAD2DEG( atan2( -vr.axis_fwd[2], xy ));
		float below = vr.ref_head_ovr[1] - vr.head_pos[1];
		qboolean crouch;
		float target, step;

		if( !vr.crouching && below > vr_crouch_height.value )
			vr.crouching = true;
		else if( vr.crouching && below < vr_crouch_height.value * 0.6f )
			vr.crouching = false;

		crouch = vr_physical_crouch.value && vr.crouching;

		// the game lowers its view when the character crouches, in its own time. the engine measures exactly how much,
		// and we give that height back, so the view follows your real head only
		{
			static cvar_t *cv_drop;
			float drop;

			if( !cv_drop )
				cv_drop = gEngfuncs.pfnGetCvarPointer( "vr_in_duck_drop" );

			drop = cv_drop ? cv_drop->value : 0.0f;
			target = drop;
			step = 0.0f;

			// vr_crouch_comp > 0 forces a fixed amount instead (the old way)
			if( vr_crouch_comp.value > 0.0f )
			{
				target = crouch ? vr_crouch_comp.value : 0.0f;
				step = vr_crouch_comp.value * bound( 0.001f, (float)gp_host->frametime, 0.1f ) / 0.35f;
			}

			if( step > 0.0f )
				vr.crouch_comp += bound( -step, target - vr.crouch_comp, step );
			else if( crouch || vr.crouch_comp > 0.5f )
				vr.crouch_comp = target;	// while you crouch, and until the game has stood up again
			else
				vr.crouch_comp = 0.0f;
		}

		while( rel > 180.0f ) rel -= 360.0f;
		while( rel < -180.0f ) rel += 360.0f;

		VR_InputPoll( rel, pitch, crouch );
	}

	if( vr_debug.value )
		VR_TrackCamera( game );

	VR_UpdateHands( game );
	VR_PublishAim( game );
	VR_ReloadTick();
	VR_HideCrosshair();
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

// the weapon events (muzzle flash, shells, tracers) are run before the eye pictures: they need the hand pose too
qboolean VR_BeginEventPose( void )
{
	qboolean was = vr.in_eye_pass;

	if( vr.initialized && vr_enable.value )
		vr.in_eye_pass = true;

	return was;
}

void VR_EndEventPose( qboolean was )
{
	vr.in_eye_pass = was;
}

qboolean VR_GetPassSize( int *width, int *height )
{
	if( !vr.in_eye_pass || vr.eye_w <= 0 || vr.eye_h <= 0 )
		return false;

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
	offset[2] += vr.crouch_comp;

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

	vr.sym_tan_x = tan_x;
	vr.sym_tan_y = tan_y;

	// the part of the centred picture that is the headset's real frustum (see vr_sym_proj)
	vr.eye_bounds[eye & 1][0] = ( vr.cur_frustum[0] + tan_x ) / ( 2.0f * tan_x );
	vr.eye_bounds[eye & 1][1] = ( vr.cur_frustum[1] + tan_x ) / ( 2.0f * tan_x );
	vr.eye_bounds[eye & 1][2] = ( vr.cur_frustum[3] + tan_y ) / ( 2.0f * tan_y );
	vr.eye_bounds[eye & 1][3] = ( vr.cur_frustum[2] + tan_y ) / ( 2.0f * tan_y );

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

	if( vr_sym_proj.value )
	{
		*left = -vr.sym_tan_x;
		*right = vr.sym_tan_x;
		*up = vr.sym_tan_y;
		*down = -vr.sym_tan_y;
		return true;
	}

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
Rifle scope. While the rifle is lifted to your eye, the scene is drawn once more from the scope
(narrow field of view, looking along the barrel) into a texture, and that texture is shown on a
round lens at the eyepiece when each eye is drawn. Only for the plain window path (no vr_use_fbo).
=================
*/
static GLuint vr_scope_tex;
static int vr_scope_tex_w, vr_scope_tex_h;
static qboolean vr_scope_in_pass;
static qboolean vr_scope_ready;
static vec3_t vr_scope_pos, vr_scope_f, vr_scope_l, vr_scope_u;
static float vr_scope_half_w = 0.28f;	// half the width of the middle square, as a fraction of the picture

// how far the gun moved from its idle pose, from the studio code (reload animation)
static float vr_scope_delta[3][4];
static qboolean vr_scope_delta_ok;
static double vr_scope_delta_time;

const char *VR_ScopeBoneName( void )
{
	return vr_scope_bone.string;
}

qboolean VR_ScopeDebug( void )
{
	return vr_scope_debug.value != 0.0f;
}

void VR_ScopeSetDelta( const float m[3][4] )
{
	vr_scope_delta_ok = m != NULL;

	if( m )
	{
		memcpy( vr_scope_delta, m, sizeof( vr_scope_delta ));
		vr_scope_delta_time = gp_cl ? gp_cl->time : 0.0;
	}
}

qboolean VR_ScopePass( void )
{
	return vr_scope_in_pass;
}

static qboolean VR_ScopeCompute( void )
{
	vec3_t org, ang, d;
	qboolean ok, old = vr.in_eye_pass;

	if( !vr_scope.value || !vr_vm_scope || vr.use_fbo || !vr.hand[1].valid )
		return false;

	vr.in_eye_pass = true;
	ok = VR_GetViewModelPose( org, ang );
	vr.in_eye_pass = old;

	if( !ok || !vr_pose_valid )
		return false;

	VectorCopy( vr_pose_f, vr_scope_f );
	VectorCopy( vr_pose_l, vr_scope_l );
	VectorCopy( vr_pose_u, vr_scope_u );

	VectorMA( vr.hand[1].pos, vr_scope_x.value, vr_scope_f, vr_scope_pos );
	VectorMA( vr_scope_pos, vr_scope_y.value, vr_scope_l, vr_scope_pos );
	VectorMA( vr_scope_pos, vr_scope_z.value, vr_scope_u, vr_scope_pos );

	// the lens is part of the gun: it moves with it while it reloads
	if( vr_scope_follow.value && vr_scope_delta_ok && gp_cl && gp_cl->time - vr_scope_delta_time < 0.25 )
	{
		vec3_t p, f, l, u;
		int i;

		for( i = 0; i < 3; i++ )
		{
			p[i] = vr_scope_delta[i][0] * vr_scope_pos[0] + vr_scope_delta[i][1] * vr_scope_pos[1] + vr_scope_delta[i][2] * vr_scope_pos[2] + vr_scope_delta[i][3];
			f[i] = vr_scope_delta[i][0] * vr_scope_f[0] + vr_scope_delta[i][1] * vr_scope_f[1] + vr_scope_delta[i][2] * vr_scope_f[2];
			l[i] = vr_scope_delta[i][0] * vr_scope_l[0] + vr_scope_delta[i][1] * vr_scope_l[1] + vr_scope_delta[i][2] * vr_scope_l[2];
			u[i] = vr_scope_delta[i][0] * vr_scope_u[0] + vr_scope_delta[i][1] * vr_scope_u[1] + vr_scope_delta[i][2] * vr_scope_u[2];
		}

		VectorCopy( p, vr_scope_pos );
		VectorNormalize( f );
		VectorNormalize( l );
		VectorNormalize( u );
		VectorCopy( f, vr_scope_f );
		VectorCopy( l, vr_scope_l );
		VectorCopy( u, vr_scope_u );
	}

	// the head must be close to the eyepiece, and behind it
	VectorSubtract( vr.head_game, vr_scope_pos, d );

	if( VectorLength( d ) > vr_scope_range.value || DotProduct( d, vr_scope_f ) > 0.0f )
		return false;

	return true;
}

// called before the eyes are drawn: fills "out" with the scope's view when the scope is in use.
// The picture is drawn exactly like an eye (the whole window), and the lens shows its middle square.
qboolean VR_SetupScopeView( const ref_viewpass_t *game, ref_viewpass_t *out )
{
	float xy, fov, aspect;

	vr_scope_ready = false;

	if( !VR_ScopeCompute( ))
		return false;

	fov = bound( 0.5f, vr_scope_fov.value, 60.0f );
	aspect = (float)gpGlobals->width / (float)gpGlobals->height;

	*out = *game;
	VectorMA( vr_scope_pos, vr_scope_cam.value, vr_scope_f, out->vieworigin );

	xy = sqrt( vr_scope_f[0] * vr_scope_f[0] + vr_scope_f[1] * vr_scope_f[1] );
	out->viewangles[PITCH] = RAD2DEG( atan2( -vr_scope_f[2], xy ));
	out->viewangles[YAW] = RAD2DEG( atan2( vr_scope_f[1], vr_scope_f[0] ));
	out->viewangles[ROLL] = RAD2DEG( atan2( vr_scope_l[2], vr_scope_u[2] ));

	// vr_scope_fov is the height of the picture, the engine wants the width
	out->fov_y = fov;
	out->fov_x = RAD2DEG( 2.0f * atan( tan( DEG2RAD( fov ) * 0.5f ) * aspect ));

	vr_scope_in_pass = true;
	return true;
}

// called after the scope's view was drawn: keep the picture
void VR_EndScopePass( void )
{
	int w, h;

	vr_scope_in_pass = false;

	// the scene target (or the window) is still bound from R_SetupGL, the same way the eyes copy it
	GL_GetSceneTargetSize( &w, &h );

	if( w < 16 || h < 16 )
		return;

	if( !vr_scope_tex || vr_scope_tex_w != w || vr_scope_tex_h != h )
	{
		if( vr_scope_tex )
			pglDeleteTextures( 1, &vr_scope_tex );

		pglGenTextures( 1, &vr_scope_tex );
		pglBindTexture( GL_TEXTURE_2D, vr_scope_tex );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		vr_scope_tex_w = w;
		vr_scope_tex_h = h;
	}
	else pglBindTexture( GL_TEXTURE_2D, vr_scope_tex );

	pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h );
	VR_ForgetTextureBinding();
	vr_scope_half_w = 0.5f * (float)h / (float)w;
	vr_scope_ready = true;
}

#define VR_SCOPE_SEGS	40

// the round lens at the eyepiece, drawn in the eye's own pass
static void VR_DrawScopeLens( void )
{
	float proj[16], view[16], r = vr_scope_radius.value;
	vec3_t c, a, b;
	int saved_tmu, unit, i;

	if( !vr_scope_ready || !vr_scope_tex || r <= 0.0f )
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

	GL_SelectTexture( 0 );
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

	// the black rim of the eyepiece
	VectorMA( vr_scope_pos, 0.03f, vr_scope_f, c );
	pglColor4f( 0.0f, 0.0f, 0.0f, 1.0f );
	pglBegin( GL_TRIANGLE_FAN );
	pglVertex3fv( c );

	for( i = 0; i <= VR_SCOPE_SEGS; i++ )
	{
		float ang = (float)i * ( 2.0f * M_PI_F / VR_SCOPE_SEGS ), ca = cosf( ang ) * r * 1.3f, sa = sinf( ang ) * r * 1.3f;

		VectorMA( c, ca, vr_scope_l, a );
		VectorMA( a, sa, vr_scope_u, b );
		pglVertex3fv( b );
	}

	pglEnd();

	// the picture
	pglEnable( GL_TEXTURE_2D );
	pglBindTexture( GL_TEXTURE_2D, vr_scope_tex );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	pglBegin( GL_TRIANGLE_FAN );
	pglTexCoord2f( 0.5f, 0.5f );
	pglVertex3fv( vr_scope_pos );

	for( i = 0; i <= VR_SCOPE_SEGS; i++ )
	{
		float ang = (float)i * ( 2.0f * M_PI_F / VR_SCOPE_SEGS ), ca = cosf( ang ), sa = sinf( ang );

		// left of the weapon is the left of the picture
		VectorMA( vr_scope_pos, ca * r, vr_scope_l, a );
		VectorMA( a, sa * r, vr_scope_u, b );
		pglTexCoord2f( 0.5f - ca * vr_scope_half_w * 0.98f, 0.5f + sa * 0.49f );
		pglVertex3fv( b );
	}

	pglEnd();
	pglDisable( GL_TEXTURE_2D );

	// crosshair
	VectorMA( vr_scope_pos, -0.03f, vr_scope_f, c );
	pglColor4f( 0.0f, 0.0f, 0.0f, 1.0f );
	pglLineWidth( bound( 0.5f, vr_scope_line.value, 6.0f ));
	pglBegin( GL_LINES );
	VectorMA( c, r * 0.95f, vr_scope_l, a ); pglVertex3fv( a );
	VectorMA( c, -r * 0.95f, vr_scope_l, a ); pglVertex3fv( a );
	VectorMA( c, r * 0.95f, vr_scope_u, a ); pglVertex3fv( a );
	VectorMA( c, -r * 0.95f, vr_scope_u, a ); pglVertex3fv( a );
	pglEnd();

	pglMatrixMode( GL_MODELVIEW );
	pglPopMatrix();
	pglMatrixMode( GL_PROJECTION );
	pglPopMatrix();
	pglPopAttrib();

	GL_SelectTexture( saved_tmu );
	VR_ForgetTextureBinding();
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

	if( !vr_draw_hands.value || ( !vr_draw_axes.value && !vr_draw_laser.value && !VR_GripDebug( )) || ( !vr.hand[0].valid && !vr.hand[1].valid ))
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
		if( vr_draw_axes.value )
		{
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
		}

		if( h == 1 && vr_draw_laser.value )
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

	VR_DrawGripSphere();

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
	if( vr_eye_flush.value >= 2.0f ) pglFinish();
	else if( vr_eye_flush.value >= 1.0f ) pglFlush();

	int w, h;

	vr.in_eye_pass = false;

	if( vr_debug.value )
		pglGetIntegerv( GL_VIEWPORT, vr.dbg_viewport[eye & 1] );

	VR_DrawTestScene();
	VR_DrawScopeLens();
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

		if( vr_sym_proj.value && !pure && vr.sym_tan_x > 0.0f && vr.sym_tan_y > 0.0f )
		{
			bounds.uMin = bound( 0.0f, vr.eye_bounds[eye][0], 1.0f );
			bounds.uMax = bound( 0.0f, vr.eye_bounds[eye][1], 1.0f );
			bounds.vMin = bound( 0.0f, vr.eye_bounds[eye][2], 1.0f );
			bounds.vMax = bound( 0.0f, vr.eye_bounds[eye][3], 1.0f );
		}

		err = vr.compositor->Submit( eye, &tex, &bounds, VR_SUBMIT_DEFAULT );

		if( err != VR_COMPOSITOR_OK && vr.err_prints++ < 10 )
			gEngfuncs.Con_Printf( S_ERROR "VR: Submit failed for eye %d (code %d)\n", eye, (int)err );
	}

	pglFlush();

	vr.frame_done = true;
	VR_DrainGLErrors();

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
