/*
gl_vr_cof.c - keeps Cry of Fear's own client.dll renderer working in stereo VR
Cry of Fear VR project.
Copyright (C) 2026 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Why this file exists (from the client.dll decompile, see the reverse-engineering notes):

client.dll is a complete custom renderer. Once per game frame it "arms" a one-shot gate
(sub_1005FE90 -> dword_104CDD64 = 1, and sub_1005F3A0 -> dword_104CDD60 = 1). The first
HUD_DrawNormalTriangles call, or the first studio model drawn, runs the gated pass
(sub_1005FF50) and disarms it again. The gated pass is the DLL's world renderer (lighting, bump
maps) and its sky cube. In VR the engine draws the scene once per eye (and once for the scope),
so only the first picture got the DLL's pass: the other eye got the plain engine world.
That is the "black in one eye", "sky only in one eye / follows one eye" and flicker.

Three more things in the same pass depend on the game's single view:
  * the sky entity list (dword_104CE078) is emptied by the first pass,
  * the sky cube is centred on the game's view origin (qword_105595CC), not on the eye,
    so with the eyes 3 units apart the 10 unit sky cube is seen from the wrong place,
  * the DLL culls world surfaces against its own 4 plane frustum, built once per frame from the
    game's view angles and fov_x with a 4:3 shape (sub_10084C30), not from the headset. Everything
    outside that narrow pyramid is simply not drawn by the DLL.

So before every render pass this file puts the DLL's per-frame state back, hands it the real
eye origin and the real eye frustum. Nothing in the DLL file is changed on disk. Every address is
checked against code bytes first; if client.dll is not the build the notes were made from, nothing
is touched and vr_cof_status says why.
*/
#include "gl_local.h"
#include "gl_vr.h"

typedef char vr_cof_plane_size_check[( sizeof( mplane_t ) == 20 ) ? 1 : -1];

static CVAR_DEFINE_AUTO( vr_cof_rearm, "1", FCVAR_ARCHIVE, "1 = give the game's client.dll renderer (world lighting, sky) its per-frame pass again for every eye and scope picture" );
static CVAR_DEFINE_AUTO( vr_cof_frustum, "1", FCVAR_ARCHIVE, "1 = replace the client.dll culling frustum with the real eye frustum (stops surfaces at the edge of the view being skipped)" );
static CVAR_DEFINE_AUTO( vr_cof_eyeorigin, "1", FCVAR_ARCHIVE, "1 = centre the client.dll sky on the eye being drawn instead of the game's view origin" );
static CVAR_DEFINE_AUTO( vr_cof_prepass, "0", 0, "1 = still honour vr_eye_prepass while the client.dll re-arm is working (normally not needed)" );

#if defined( _WIN32 )
#include <windows.h>

// addresses from the decompile (image base 0x10000000)
#define COF_BASE		0x10000000u
#define COF_RVA( a )		(( a ) - COF_BASE )

#define COF_FN_FRAMESTART	0x1005FE90u	// ++visframe; skydone = 0; arm = 1; call frame walk
#define COF_FN_GATEPASS		0x1005FF50u	// if( arm ) { ...pass...; skycount = 0; arm = 0; }
#define COF_ARM			0x104CDD64u	// one-shot gate
#define COF_INNER		0x104CDD60u	// inner gate of the world pass (set by the frame walk)
#define COF_SKYDONE		0x104CDD70u	// sky already handled this frame
#define COF_VISFRAME		0x104CDD44u
#define COF_SKYCOUNT		0x104CE078u	// number of sky entities waiting to be drawn
#define COF_VIEWORG		0x105595CCu	// float[3] view origin used by the sky and glows
#define COF_FRUSTUM		0x104CDDD8u	// 4 * mplane_t (20 bytes each)

static struct
{
	HMODULE		hmod;
	qboolean	ok;
	char		why[160];
	uintptr_t	base;
	volatile int	*arm, *inner, *skydone, *skycount;
	float		*vieworg;
	mplane_t	*frustum;

	qboolean	in_frame;
	int		s_arm, s_inner, s_skydone, s_skycount;
	float		s_org[3];

	int		frames, passes, skipped;
	int		last_pass_arm, last_pass_inner;
} cof;

static qboolean COF_Readable( const void *p, size_t len, qboolean write )
{
	MEMORY_BASIC_INFORMATION mbi;
	const byte *cur = (const byte *)p, *end = cur + len;

	while( cur < end )
	{
		DWORD ok = write ? ( PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY )
				 : ( PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY );

		if( !VirtualQuery( cur, &mbi, sizeof( mbi )) || mbi.State != MEM_COMMIT )
			return false;
		if( !( mbi.Protect & ok ) || ( mbi.Protect & PAGE_GUARD ))
			return false;
		cur = (const byte *)mbi.BaseAddress + mbi.RegionSize;
	}
	return true;
}

// does the code at "fn" (len bytes) contain the 32 bit absolute address of every given variable?
// The loader applies relocations, so a correct build holds base + rva in the instruction bytes.
static qboolean COF_CodeRefs( uintptr_t base, unsigned fn, int len, const unsigned *vars, int nvars )
{
	const byte *code = (const byte *)( base + COF_RVA( fn ));
	int v, i;

	if( !COF_Readable( code, len, false ))
		return false;

	for( v = 0; v < nvars; v++ )
	{
		uint32_t want = (uint32_t)( base + COF_RVA( vars[v] ));
		qboolean found = false;

		for( i = 0; i + 4 <= len && !found; i++ )
		{
			uint32_t got;

			memcpy( &got, code + i, sizeof( got ));
			if( got == want )
				found = true;
		}

		if( !found )
			return false;
	}
	return true;
}

static void COF_Fail( const char *why )
{
	Q_strncpy( cof.why, why, sizeof( cof.why ));
	cof.ok = false;
}

static void COF_Resolve( void )
{
	HMODULE h = GetModuleHandleA( "client.dll" );
	const byte *pe;
	uint32_t lfanew, imagebase, imagesize;
	uintptr_t base;
	static const unsigned frame_vars[] = { COF_ARM, COF_SKYDONE, COF_VISFRAME };
	static const unsigned gate_vars[] = { COF_ARM, COF_SKYCOUNT };

	if( h == cof.hmod && ( cof.ok || cof.why[0] ))
		return;

	memset( &cof, 0, sizeof( cof ));
	cof.hmod = h;

	if( !h )
	{
		COF_Fail( "client.dll is not loaded (is this the Cry of Fear folder?)" );
		return;
	}

	base = (uintptr_t)h;
	if( !COF_Readable( (const void *)base, 0x400, false ))
	{
		COF_Fail( "cannot read the client.dll header" );
		return;
	}

	pe = (const byte *)base;
	if( pe[0] != 'M' || pe[1] != 'Z' )
	{
		COF_Fail( "client.dll has no DOS header" );
		return;
	}

	memcpy( &lfanew, pe + 0x3C, 4 );
	if( lfanew > 0x300 || memcmp( pe + lfanew, "PE\0\0", 4 ) || pe[lfanew + 24] != 0x0B || pe[lfanew + 25] != 0x01 )
	{
		COF_Fail( "client.dll is not a 32 bit image" );	// the engine must be the 32 bit build as well
		return;
	}

	memcpy( &imagebase, pe + lfanew + 24 + 28, 4 );
	memcpy( &imagesize, pe + lfanew + 24 + 56, 4 );

	if( imagebase != COF_BASE || imagesize < COF_RVA( 0x105595E0u ))
	{
		COF_Fail( "client.dll is a different build than the one the notes were made from" );
		return;
	}

	if( !COF_CodeRefs( base, COF_FN_FRAMESTART, 0x30, frame_vars, 3 ) || !COF_CodeRefs( base, COF_FN_GATEPASS, 0xA0, gate_vars, 2 ))
	{
		COF_Fail( "client.dll code does not match (different build): gate functions not found" );
		return;
	}

	cof.base = base;
	cof.arm = (volatile int *)( base + COF_RVA( COF_ARM ));
	cof.inner = (volatile int *)( base + COF_RVA( COF_INNER ));
	cof.skydone = (volatile int *)( base + COF_RVA( COF_SKYDONE ));
	cof.skycount = (volatile int *)( base + COF_RVA( COF_SKYCOUNT ));
	cof.vieworg = (float *)( base + COF_RVA( COF_VIEWORG ));
	cof.frustum = (mplane_t *)( base + COF_RVA( COF_FRUSTUM ));

	if( !COF_Readable( (const void *)cof.arm, 4, true ) || !COF_Readable( (const void *)cof.inner, 4, true )
		|| !COF_Readable( (const void *)cof.skydone, 4, true ) || !COF_Readable( (const void *)cof.skycount, 4, true )
		|| !COF_Readable( cof.vieworg, 12, true ) || !COF_Readable( cof.frustum, sizeof( mplane_t ) * 4, true ))
	{
		COF_Fail( "client.dll data is not writable where expected" );
		return;
	}

	cof.ok = true;
	Q_strncpy( cof.why, "ok", sizeof( cof.why ));
	gEngfuncs.Con_Printf( "VR: client.dll renderer found at %p, per-eye re-arm is available (vr_cof_status)\n", (void *)base );
}

qboolean VR_CofActive( void )
{
	COF_Resolve();
	return cof.ok && vr_cof_rearm.value != 0.0f;
}

// once per game frame, before the first picture. client.dll has run its frame start by now.
void VR_CofBeginFrame( void )
{
	cof.in_frame = false;

	if( !VR_CofActive())
		return;

	// nothing armed: the frame start did not run (menu, loading, custom renderer off). Leave it alone.
	if( !*cof.arm && !*cof.inner )
	{
		cof.skipped++;
		return;
	}

	cof.s_arm = *cof.arm;
	cof.s_inner = *cof.inner;
	cof.s_skydone = *cof.skydone;
	cof.s_skycount = *cof.skycount;
	memcpy( cof.s_org, cof.vieworg, sizeof( cof.s_org ));

	cof.in_frame = true;
	cof.frames++;
}

// before every R_RenderScene of a VR frame, after the engine built RI.frustum
void VR_CofBeforePass( const void *frustum_planes, const float *eyeorigin )
{
	const mplane_t *planes = (const mplane_t *)frustum_planes;

	if( !cof.in_frame || !cof.ok )
		return;

	// arm the one-shot gate again, with exactly what the frame start left behind
	*cof.arm = cof.s_arm;
	*cof.inner = cof.s_inner;
	*cof.skydone = cof.s_skydone;
	*cof.skycount = cof.s_skycount;

	if( vr_cof_eyeorigin.value != 0.0f && eyeorigin )
		memcpy( cof.vieworg, eyeorigin, sizeof( float ) * 3 );

	if( vr_cof_frustum.value != 0.0f && planes )
		memcpy( cof.frustum, planes, sizeof( mplane_t ) * 4 );	// left, right, bottom, top: the engine and the DLL both use inward normals

	cof.last_pass_arm = *cof.arm;
	cof.last_pass_inner = *cof.inner;
	cof.passes++;
}

qboolean VR_CofFrameArmed( void )
{
	return cof.in_frame;
}

void VR_CofEndFrame( void )
{
	if( cof.in_frame && cof.ok && vr_cof_eyeorigin.value != 0.0f )
		memcpy( cof.vieworg, cof.s_org, sizeof( cof.s_org ));

	cof.in_frame = false;
}

static void VR_CofStatus_f( void )
{
	COF_Resolve();

	gEngfuncs.Con_Printf( "client.dll renderer hook: %s\n", cof.why[0] ? cof.why : "not checked yet" );
	if( !cof.ok )
		return;

	gEngfuncs.Con_Printf( "  base %p, re-arm %s, frustum %s, eye origin %s\n", (void *)cof.base,
		vr_cof_rearm.value ? "on" : "off", vr_cof_frustum.value ? "on" : "off", vr_cof_eyeorigin.value ? "on" : "off" );
	gEngfuncs.Con_Printf( "  frames armed %d, passes re-armed %d, frames skipped (nothing armed) %d\n", cof.frames, cof.passes, cof.skipped );
	gEngfuncs.Con_Printf( "  now: arm %d, inner %d, sky entities waiting %d, last snapshot arm %d inner %d skyentities %d\n",
		*cof.arm, *cof.inner, *cof.skycount, cof.s_arm, cof.s_inner, cof.s_skycount );
	if( cof.frames == 0 && cof.skipped > 10 )
		gEngfuncs.Con_Printf( "  the DLL never arms its pass before our picture is drawn: send me this line\n" );
}

#else

qboolean VR_CofActive( void ) { return false; }
void VR_CofBeginFrame( void ) { }
qboolean VR_CofFrameArmed( void ) { return false; }
void VR_CofBeforePass( const void *frustum_planes, const float *eyeorigin ) { }
void VR_CofEndFrame( void ) { }

static void VR_CofStatus_f( void )
{
	gEngfuncs.Con_Printf( "client.dll renderer hook: Windows only\n" );
}

#endif

qboolean VR_CofKeepPrepass( void )
{
	return vr_cof_prepass.value != 0.0f;
}

void VR_CofInitCvars( void )
{
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_rearm );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_frustum );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_eyeorigin );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_prepass );
	gEngfuncs.Cmd_AddCommand( "vr_cof_status", VR_CofStatus_f, "show whether the game's client.dll renderer is being kept in step with the VR eyes" );
}
