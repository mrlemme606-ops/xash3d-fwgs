/*
gl_vr.h - OpenVR (SteamVR) stereo output for the GL renderer
Cry of Fear VR project, milestone 1: head-tracked stereo view.
Copyright (C) 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#ifndef GL_VR_H
#define GL_VR_H

#include "gl_local.h"
#include <stdint.h>

void VR_InitCvars( void );
void VR_NewFrame( void );
qboolean VR_BeginMainPass( const ref_viewpass_t *game );
void VR_SetupEyeView( int eye, const ref_viewpass_t *game, ref_viewpass_t *out );
qboolean VR_GetEyeFrustum( float *left, float *right, float *up, float *down );
void VR_EndEyePass( int eye );
void VR_FinishFrame( void );
qboolean VR_GetEyeTarget( GLuint *fbo, int *width, int *height );

// eye render targets, implemented in gl_fbo.c
qboolean GL_VRCreateEyeTargets( int width, int height );
void GL_VRFreeEyeTargets( void );
GLuint GL_VREyeFBO( int eye );
GLuint GL_VREyeTexture( int eye );

// controller input (gl_vr_input.c)
void *VR_GetSystemTable( void );
void VR_InputInitCvars( void );
void VR_InputPoll( float head_yaw_relative, float head_pitch, qboolean physical_crouch );
uint32_t VR_InputGetDevice( int hand );	// 0 = left, 1 = right, 0xFFFFFFFF = none

// helpers for the UI panel (gl_vr_ui.c)
qboolean VR_IsActive( void );
void *VR_GetInterface( const char *fntable_name );
void VR_GetHeadMatrix( float m[3][4] );
void VR_UIUpdate( void );
void VR_UIInitCvars( void );
void VR_UISetPointer( qboolean visible, float u, float v );
qboolean VR_UIRayHit( const float origin[3], const float dir[3], float *u, float *v, float *dist );
qboolean VR_GetViewModelPose( vec3_t origin, vec3_t angles );

float VR_GetViewModelScale( void );
int VR_ViewModelMeshMode( int mesh, const char *texname );	// 0 = draw, 1 = hide, 2 = draw cut off at the wrist
void VR_ViewModelClip( qboolean on );
void VR_DrawArmCap( void );
qboolean VR_ViewModelFrozen( void );
qboolean VR_WantBoneDump( void );
qboolean VR_ViewModelBoneHidden( const char *bonename );
qboolean VR_ViewModelLock( int *seq, float *frame );
qboolean VR_IsIdleLabel( const char *label );
void VR_DrainGLErrors( void );
void VR_DumpSequences( void );
qboolean VR_FrameDone( void );
qboolean VR_GetHandPose( int hand, float m[3][4], uint32_t *device );
void VR_HUDCapture( void );
qboolean VR_GetLeftHandPose( vec3_t origin, vec3_t angles );
float VR_GetLeftHandScale( void );
const char *VR_LeftHandModel( void );
void VR_SetHandPass( qboolean left );
qboolean VR_GetLeftHandPoseOfs( vec3_t origin, vec3_t angles, const float *ofs );
qboolean VR_GetHandBasis( int hand, vec3_t pos, vec3_t fwd, vec3_t left, vec3_t up );
void VR_GetHeadGame( vec3_t pos, float *yaw );

// manual reloading (gl_vr_reload.c)
typedef struct
{
	qboolean	hide;		// draw no left hand at all
	qboolean	weapon_model;	// draw the left arm of the weapon in hand instead of the universal hand
	int		seq;
	float		frame;
	float		scale;
	vec3_t		origin, angles;
} vr_rl_left_t;

void VR_ReloadInitCvars( void );
void VR_ReloadTick( void );
void VR_ReloadSetGunPose( const vec3_t origin, const vec3_t angles, float scale );
void VR_ReloadObserve( const char *label, float animtime );
int VR_ReloadBoneZone( const char *bonename );
void VR_ReloadZoneAccum( int zone, const float *pos );
void VR_ReloadZoneFlush( void );
qboolean VR_ReloadBoneHidden( qboolean left_pass, const char *bonename, qboolean base_hidden );
qboolean VR_ReloadIsBoltBone( const char *bonename );
qboolean VR_ReloadBoltOffset( float off[3] );
qboolean VR_ReloadWantsIdle( void );
qboolean VR_ReloadTwoHandAim( void );
qboolean VR_SkyModelsOn( void );
void VR_LightObserve( const char *label, float animtime );
qboolean VR_GetViewModelAxes( vec3_t f, vec3_t l, vec3_t u );
qboolean VR_ViewModelMirror( void );
int VR_EyePrepass( void );
qboolean VR_EntityHidden( const cl_entity_t *e );
float R_StudioModelRadius( model_t *mod );
qboolean VR_ScopePass( void );
qboolean VR_SetupScopeView( const ref_viewpass_t *game, ref_viewpass_t *out );
void VR_EndScopePass( void );
qboolean VR_CenterWanted( void );
void VR_CenterAccum( const float *p );
void VR_CenterFinish( void );
const float *VR_ReloadRightOffsets( void );
qboolean VR_ReloadLeftDraw( vr_rl_left_t *d );

#endif // GL_VR_H
