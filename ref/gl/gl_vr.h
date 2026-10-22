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

void VR_InitCvars( void );
void VR_NewFrame( void );
qboolean VR_BeginMainPass( const ref_viewpass_t *game );
void VR_SetupEyeView( int eye, const ref_viewpass_t *game, ref_viewpass_t *out );
qboolean VR_GetEyeFrustum( float *left, float *right, float *up, float *down );
void VR_EndEyePass( int eye );
void VR_FinishFrame( void );

// controller input (gl_vr_input.c)
void *VR_GetSystemTable( void );
void VR_InputInitCvars( void );
void VR_InputPoll( float head_yaw_relative, float head_pitch, qboolean physical_crouch );

// helpers for the UI panel (gl_vr_ui.c)
qboolean VR_IsActive( void );
void *VR_GetInterface( const char *fntable_name );
void VR_GetHeadMatrix( float m[3][4] );
void VR_UIUpdate( void );
void VR_UIInitCvars( void );

#endif // GL_VR_H
