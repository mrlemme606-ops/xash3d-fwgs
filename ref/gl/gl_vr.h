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

#endif // GL_VR_H
