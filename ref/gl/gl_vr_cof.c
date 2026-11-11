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
static CVAR_DEFINE_AUTO( vr_cof_eyeangles, "1", FCVAR_ARCHIVE, "1 = give the client.dll the view angles of the eye being drawn (your head), not just the game's own. Its water reflections and effects are built from them" );
static CVAR_DEFINE_AUTO( vr_cof_eyeorigin, "1", FCVAR_ARCHIVE, "1 = centre the client.dll sky on the eye being drawn instead of the game's view origin" );
static CVAR_DEFINE_AUTO( vr_cof_depth, "0.8", 0, "far end of the depth range the world is drawn in while the game's client.dll is attached (it keeps 0.8 to 1.0 for its sky). 1 = off" );
static CVAR_DEFINE_AUTO( vr_cof_watersize, "1", FCVAR_ARCHIVE, "1 = tell the game's water effect the size of the eye picture (it copies the screen from the bottom left corner at the window size). Experimental" );
static CVAR_DEFINE_AUTO( vr_cof_watercomp, "1", FCVAR_ARCHIVE, "the game's own water reflection is a copy of the picture shifted 'up' on the screen. 1 = shift towards the real up of the world (stable when you tilt your head), -1 = the other way round, 0 = shift along the screen like the flat game" );
static CVAR_DEFINE_AUTO( vr_cof_waterscale, "1", FCVAR_ARCHIVE, "how far the game's water reflection is shifted (1 = like the flat game)" );
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
#define COF_DLIGHTS		0x104CC550u	// 32 lights of 31 floats (sub_1005C0F0)
#define COF_DLIGHT_KEY		0x104CC574u
#define COF_FN_DLIGHTALLOC	0x1005C0F0u
#define COF_SECONDARY		0x10556B48u	// HUD_DrawNormalTriangles draws the secondary view models (goggles, dual weapon...) when this is set
#define COF_DUAL_NAME		0x10545870u	// char[64] model of the weapon in the left hand (DualWield message)
#define COF_DUAL_MODE		0x105458B0u	// 3 = a weapon model
#define COF_VANGLES		0x1055962Cu	// 3 floats: view pitch (down positive), yaw, roll
#define COF_SLOT_NAMES	0x1018D024u	// the hotkey slots: three names of 128 bytes ("weapon_switchblade"...), written by the QSlotUpdate message
#define COF_ENT_A		0x10550E90u	// origin[3] then angles[3] of the entity the secondary weapon is drawn from (cl_entity_t.origin / angles)
#define COF_ENT_B		0x10550608u	// the same in its curstate (origin[3], angles[3])
#define COF_VORIGIN		0x10559638u	// 3 floats: view origin the secondary view models are drawn at
#define COF_FN_SECONDARY	0x1008C7E0u
#define COF_FN_WATER		0x10086D10u	// the water pass: scene copy, noise on the water quads, shader pass over the screen
#define COF_WATER_OFFSET	0x101511C0u	// float[4] TexOffset of water_fp.cg: shift = xy * noise.r + zw * noise.g, stock value 0 0.05 0 0.05
#define COF_WATER_OBJ		0x10545078u	// the water object: float +12 and +16 are the size of its textures (powers of two)
#define COF_SCREENSZ		0x1018F430u	// int width, height the water effect copies from the screen

static struct
{
	HMODULE		hmod;
	qboolean	ok;
	char		why[160];
	uintptr_t	base;
	uint32_t	imagebase, imagesize;
	char		dump[100];
	volatile int	*arm, *inner, *skydone, *skycount;
	float		*vieworg;
	mplane_t	*frustum;
	float		*dlights;	// 32 * 31 floats
	int		*scrsize;
	float		*waterofs;
	const float	*waterobj;
	float		water_orig[4];
	qboolean	water_ok, water_patched;
	float		water_dir[2];
	volatile int	*secondary;
	char		*dualname;
	int		*dualmode;
	float		*vangles, *vorigin;
	float		*ent_a, *ent_b;
	char		*slots;
	qboolean	ent_ok;
	float		s_vo[3], s_va[3];
	int		hid_secondary;
	qboolean	dual_ok;
	int		s_scr[2];
	qboolean	scr_ok;
	qboolean	lights_ok;

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

static void COF_Dump( uintptr_t base, unsigned fn )
{
	const byte *code = (const byte *)( base + COF_RVA( fn ));
	int i;

	cof.dump[0] = 0;
	if( !COF_Readable( code, 24, false ))
		return;

	for( i = 0; i < 24; i++ )
		Q_snprintf( cof.dump + i * 3, sizeof( cof.dump ) - i * 3, "%02X ", code[i] );
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
	cof.imagebase = imagebase;
	cof.imagesize = imagesize;

	// the header values are only reported: the code byte check below is what decides
	if( !COF_CodeRefs( base, COF_FN_FRAMESTART, 0x40, frame_vars, 3 ))
	{
		COF_Dump( base, COF_FN_FRAMESTART );
		COF_Fail( "frame start function not found where expected (different client.dll build?)" );
		return;
	}

	if( !COF_CodeRefs( base, COF_FN_GATEPASS, 0xA0, gate_vars, 2 ))
	{
		COF_Dump( base, COF_FN_GATEPASS );
		COF_Fail( "gate pass function not found where expected (different client.dll build?)" );
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

	{
		static const unsigned dl_vars[] = { COF_DLIGHTS, COF_DLIGHT_KEY };

		cof.dlights = (float *)( base + COF_RVA( COF_DLIGHTS ));
		cof.lights_ok = COF_CodeRefs( base, COF_FN_DLIGHTALLOC, 0x90, dl_vars, 2 ) && COF_Readable( cof.dlights, 32 * 31 * 4, true );
	}

	{
		static const unsigned dual_vars[] = { COF_DUAL_MODE, COF_VORIGIN };

		cof.secondary = (volatile int *)( base + COF_RVA( COF_SECONDARY ));
		cof.dualname = (char *)( base + COF_RVA( COF_DUAL_NAME ));
		cof.dualmode = (int *)( base + COF_RVA( COF_DUAL_MODE ));
		cof.vangles = (float *)( base + COF_RVA( COF_VANGLES ));
		cof.vorigin = (float *)( base + COF_RVA( COF_VORIGIN ));
		{
			static const unsigned ent_vars[] = { COF_ENT_A, COF_ENT_B };

			cof.slots = (char *)( base + COF_RVA( COF_SLOT_NAMES ));
			cof.ent_a = (float *)( base + COF_RVA( COF_ENT_A ));
			cof.ent_b = (float *)( base + COF_RVA( COF_ENT_B ));
			cof.ent_ok = COF_CodeRefs( base, COF_FN_SECONDARY, 0x1000, ent_vars, 2 )
				&& COF_Readable( cof.ent_a, 24, true ) && COF_Readable( cof.ent_b, 24, true );
		}

		cof.dual_ok = COF_CodeRefs( base, COF_FN_SECONDARY, 0x900, dual_vars, 2 )
			&& COF_Readable( cof.dualname, 64, false ) && COF_Readable( cof.dualmode, 4, false )
			&& COF_Readable( cof.vangles, 12, true ) && COF_Readable( cof.vorigin, 12, true ) && COF_Readable( (void *)cof.secondary, 4, true );
	}

	cof.scrsize = (int *)( base + COF_RVA( COF_SCREENSZ ));
	cof.scr_ok = COF_Readable( cof.scrsize, 8, true );

	// the water shift constant. It sits in read only data, so it is only touched after its code and its value were checked
	{
		static const unsigned water_vars[] = { COF_WATER_OFFSET };
		static const uint32_t water_stock[4] = { 0x00000000u, 0x3D4CCCCDu, 0x00000000u, 0x3D4CCCCDu };

		cof.waterofs = (float *)( base + COF_RVA( COF_WATER_OFFSET ));
		cof.waterobj = (const float *)( base + COF_RVA( COF_WATER_OBJ ));
		cof.water_ok = false;

		if( COF_CodeRefs( base, COF_FN_WATER, 0x800, water_vars, 1 ) && COF_Readable( cof.waterofs, 16, false ) && !memcmp( cof.waterofs, water_stock, 16 ))
		{
			DWORD old = 0;

			if( VirtualProtect( cof.waterofs, 16, PAGE_EXECUTE_READWRITE, &old ))
			{
				memcpy( cof.water_orig, cof.waterofs, sizeof( cof.water_orig ));
				cof.water_ok = true;
			}
		}
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

static struct
{
	qboolean	valid, omni, phone;
	vec3_t		start, dir, spot;
} hl;

void VR_CofSetHandLight( qboolean valid, qboolean omni, qboolean phone, const float *start, const float *dir, const float *spot )
{
	hl.valid = valid && start && dir && spot;
	hl.omni = omni;
	hl.phone = phone;

	if( hl.valid )
	{
		VectorCopy( start, hl.start );
		VectorCopy( dir, hl.dir );
		VectorCopy( spot, hl.spot );
	}
}

// The game makes its own lights for a held phone, flashlight, flare or lantern (sub_10065260): a spotlight with a
// texture (flashlight2.tga) and small point lights, all at the player's body. Each is an entry in the DLL's
// light array (31 floats: origin 0-2, color 4-5, radius 3, ratio 6, die time 7, key 9, angles 10-12,
// texture name at byte 60). Move them to the hand, and aim the spotlight where the hand points.
static void COF_Relight( void )
{
	int i;
	float now = gp_cl ? (float)gp_cl->time : 0.0f;

	if( !cof.lights_ok || !hl.valid )
		return;

	for( i = 0; i < 32; i++ )
	{
		float *l = cof.dlights + i * 31;
		const char *tex = (const char *)( l + 15 );
		qboolean spot = Q_stristr( tex, "flashlight" ) != NULL;

		if( l[7] < now || l[3] <= 0.0f )
			continue;

		// the game leaves the phone's light on when you put the phone away. Nothing of it shines then: the spotlight
		// (gfx/flashlight.tga, the weapons' tacklights use flashlight2.tga) and the small lights around you
		if( !hl.phone && ( l[3] == 60.0f && !tex[0] || ( spot && Q_stristr( tex, "flashlight.tga" ))))
		{
			l[7] = 0.0f;
			l[3] = 0.0f;
			continue;
		}

		if( spot )
		{
			// phone, flashlight and weapon tacklights: their spotlight texture is gfx/flashlight.tga or flashlight2.tga
			VectorCopy( hl.start, l );
			l[10] = -asin( bound( -1.0f, hl.dir[2], 1.0f )) * ( 180.0f / M_PI_F );	// pitch, positive looks down
			l[11] = atan2( hl.dir[1], hl.dir[0] ) * ( 180.0f / M_PI_F );
			l[12] = 0.0f;
		}
		else if( !tex[0] )
		{
			// the glow of a held flare or lantern: the game puts it at the body, move it to the hand
			if( hl.omni )
			{
				vec3_t d;

				VectorSubtract( l, cof.s_org, d );
				if( DotProduct( d, d ) < 200.0f * 200.0f )
					VectorCopy( hl.start, l );
			}
			else if( l[3] == 60.0f )	// small point lights where a beam lands
			{
				VectorCopy( hl.spot, l );
			}
		}
	}
}

static void VR_CofLightDump_f( void )
{
	int i, n = 0;
	float now = gp_cl ? (float)gp_cl->time : 0.0f;

	COF_Resolve();

	if( !cof.ok || !cof.lights_ok )
	{
		gEngfuncs.Con_Printf( "light array not available\n" );
		return;
	}

	for( i = 0; i < 32; i++ )
	{
		const float *l = cof.dlights + i * 31;

		if( l[7] < now || l[3] <= 0.0f )
			continue;

		n++;
		gEngfuncs.Con_Printf( "light %d: origin %.0f %.0f %.0f radius %.0f ratio %.2f angles %.0f %.0f texture '%s'\n", i, l[0], l[1], l[2], l[3], l[6], l[10], l[11], (const char *)( l + 15 ));
	}

	gEngfuncs.Con_Printf( "%d active lights, hand light %s\n", n, hl.valid ? ( hl.omni ? "omni" : "spot" ) : "none" );
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
	if( cof.dual_ok )
	{
		memcpy( cof.s_vo, cof.vorigin, sizeof( cof.s_vo ));
		memcpy( cof.s_va, cof.vangles, sizeof( cof.s_va ));
	}
	if( cof.scr_ok )
	{
		cof.s_scr[0] = cof.scrsize[0];
		cof.s_scr[1] = cof.scrsize[1];
	}

	cof.in_frame = true;
	cof.frames++;
}

// The game's water is not a mirror. It copies the finished picture, paints the ripple texture (noise.tga) on the water
// quads, and then draws every water pixel again with the colour it finds a little way "up" the screen:
//   shift = TexOffset.xy * noise.r + TexOffset.zw * noise.g     (stock TexOffset = 0 0.05 0 0.05, so straight up the screen)
// That is fine for a camera that never rolls. A tilted head makes "up the screen" point sideways in the world, so the
// reflection swims. Here the same shift (same length, same ripples) is pointed at the real up of the world instead.
static void COF_WaterShift( const float *eyeangles )
{
	vec3_t f, r, u, tmp;
	float sx, sy, len, texw, texh, k;

	if( !cof.water_ok )
		return;

	if( !eyeangles || vr_cof_watercomp.value == 0.0f )
	{
		if( cof.water_patched )	// back to the stock value
		{
			memcpy( cof.waterofs, cof.water_orig, sizeof( cof.water_orig ));
			cof.water_patched = false;
		}
		return;
	}

	AngleVectors( eyeangles, f, r, u );
	sx = r[2];	// where the world's up (0 0 1) lands on the screen: along the screen's right ...
	sy = u[2];	// ... and along the screen's up
	len = sqrt( sx * sx + sy * sy );

	if( len < 0.25f )	// looking nearly straight up or down: only the roll tells where up is
	{
		VectorSet( tmp, 0.0f, eyeangles[YAW], eyeangles[ROLL] );
		AngleVectors( tmp, f, r, u );
		sx = r[2];
		sy = u[2];
		len = sqrt( sx * sx + sy * sy );
	}

	if( len < 0.01f )
	{
		sx = 0.0f;
		sy = 1.0f;
	}
	else
	{
		sx /= len;
		sy /= len;
	}

	if( vr_cof_watercomp.value < 0.0f )
		sx = -sx;

	// the shift is measured in the water textures' own units, which are a power of two bigger than the picture
	texw = cof.waterobj[3];
	texh = cof.waterobj[4];

	if( !( texw >= 16.0f && texw <= 16384.0f && texh >= 16.0f && texh <= 16384.0f ))
		texw = texh = 1.0f;

	k = 0.05f * bound( 0.0f, vr_cof_waterscale.value, 4.0f );

	// the same length in pixels in both directions
	cof.water_dir[0] = k * sx * texh / texw;
	cof.water_dir[1] = k * sy;

	cof.waterofs[0] = cof.water_dir[0];
	cof.waterofs[1] = cof.water_dir[1];
	cof.waterofs[2] = cof.water_dir[0];
	cof.waterofs[3] = cof.water_dir[1];
	cof.water_patched = true;
}

// before every R_RenderScene of a VR frame, after the engine built RI.frustum
void VR_CofBeforePass( const void *frustum_planes, const float *eyeorigin, const float *eyeangles )
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

	if( cof.dual_ok && vr_cof_eyeangles.value != 0.0f )
	{
		if( eyeorigin )
			memcpy( cof.vorigin, eyeorigin, sizeof( float ) * 3 );

		if( eyeangles )
			memcpy( cof.vangles, eyeangles, sizeof( float ) * 3 );
	}

	if( vr_cof_frustum.value != 0.0f && planes )
		memcpy( cof.frustum, planes, sizeof( mplane_t ) * 4 );	// left, right, bottom, top: the engine and the DLL both use inward normals

	COF_Relight();
	COF_WaterShift( eyeangles );

	// the water effect copies the screen from the bottom left corner, at this size
	if( cof.scr_ok && vr_cof_watersize.value != 0.0f && cof.s_scr[0] > 0 && cof.s_scr[1] > 0 )
	{
		int w, h;

		if( VR_GetPassSize( &w, &h ))
		{
			cof.scrsize[0] = Q_min( w, cof.s_scr[0] );
			cof.scrsize[1] = Q_min( h, cof.s_scr[1] );
		}
	}

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

	if( cof.in_frame && cof.ok && cof.dual_ok && vr_cof_eyeangles.value != 0.0f )
	{
		memcpy( cof.vorigin, cof.s_vo, sizeof( cof.s_vo ));
		memcpy( cof.vangles, cof.s_va, sizeof( cof.s_va ));
	}

	if( cof.water_patched )
	{
		memcpy( cof.waterofs, cof.water_orig, sizeof( cof.water_orig ));
		cof.water_patched = false;
	}

	if( cof.in_frame && cof.scr_ok && cof.s_scr[0] > 0 )
	{
		cof.scrsize[0] = cof.s_scr[0];
		cof.scrsize[1] = cof.s_scr[1];
	}

	cof.in_frame = false;
}

static void VR_CofStatus_f( void )
{
	COF_Resolve();

	gEngfuncs.Con_Printf( "client.dll renderer hook: %s\n", cof.why[0] ? cof.why : "not checked yet" );
	if( cof.hmod )
		gEngfuncs.Con_Printf( "  module %p, header image base 0x%08X, image size 0x%X\n", (void *)cof.hmod, cof.imagebase, cof.imagesize );
	if( !cof.ok )
	{
		if( cof.dump[0] )
			gEngfuncs.Con_Printf( "  first bytes of the function the notes expected: %s\n", cof.dump );
		return;
	}

	gEngfuncs.Con_Printf( "  dual weapon hook: %s\n", cof.dual_ok ? "found" : "NOT found" );
	gEngfuncs.Con_Printf( "  water shift hook: %s, last direction on screen %.3f %.3f\n", cof.water_ok ? "found" : "NOT found (different client.dll build)", cof.water_dir[0], cof.water_dir[1] );
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
void VR_CofBeforePass( const void *frustum_planes, const float *eyeorigin, const float *eyeangles ) { }
void VR_CofEndFrame( void ) { }
void VR_CofSetHandLight( qboolean valid, qboolean omni, qboolean phone, const float *start, const float *dir, const float *spot ) { }

static void VR_CofStatus_f( void )
{
	gEngfuncs.Con_Printf( "client.dll renderer hook: Windows only\n" );
}

static void VR_CofLightDump_f( void ) { }

#endif

// The DLL splits the depth buffer: world 0.0-0.8, sky entities 0.8-0.9, sky cube 0.9-1.0
// (glDepthRange calls in sub_1005F3A0 / sub_10063780). The stock engine draws the world over the whole
// 0-1 range, so every world surface farther than about 40 units is "behind" the sky cube and hidden by it.
// That is the radius around you in which textures show. Drawing the engine's world in 0-0.8 as well fixes it.
float VR_CofDepthMax( void )
{
	float v = vr_cof_depth.value;

#if defined( _WIN32 )
	COF_Resolve();
	if( !cof.ok )
		return 1.0f;
#else
	return 1.0f;
#endif
	return ( v > 0.1f && v < 1.0f ) ? v : 1.0f;
}

// while the game draws its own normal triangles in an eye pass, its secondary view (the dual weapon, always in front of the
// body and turning with the mouse) must not be drawn: the copy that follows your left hand is drawn by VR_CofDrawDual
void VR_CofHideSecondary( qboolean hide )
{
#if defined( _WIN32 )
	if( !cof.ok || !cof.dual_ok || *cof.dualmode != 3 )
	{
		cof.hid_secondary = -1;
		return;
	}

	if( hide )
	{
		cof.hid_secondary = *cof.secondary;
		*cof.secondary = 0;
	}
	else if( cof.hid_secondary >= 0 )
	{
		*cof.secondary = cof.hid_secondary;
		cof.hid_secondary = -1;
	}
#endif
}

// the weapon in the left hand of a dual wield: the game draws it itself, in its "secondary view" (see COF_SECONDARY)
qboolean VR_CofDualModel( char *out, int size )
{
#if defined( _WIN32 )
	COF_Resolve();

	if( !cof.ok || !cof.dual_ok || *cof.dualmode != 3 || Q_strlen( cof.dualname ) < 2 )
		return false;

	Q_strncpy( out, cof.dualname, size );
	return true;
#else
	return false;
#endif
}

// what is in hotkey slot 1..3 (the weapon's name without the "weapon_"), empty when nothing
qboolean VR_CofSlotName( int slot, char *out, size_t size )
{
	out[0] = 0;
#if defined( _WIN32 )
	if( !cof.ok || !cof.slots || slot < 1 || slot > 3 )
		return false;

	if( !COF_Readable( cof.slots + ( slot - 1 ) * 128, 128, false ))
		return false;

	Q_strncpy( out, cof.slots + ( slot - 1 ) * 128, Q_min( size, (size_t)127 ));
	out[Q_min( size, (size_t)127 ) - 1] = 0;

	if( !Q_strnicmp( out, "weapon_", 7 ))
		memmove( out, out + 7, strlen( out + 7 ) + 1 );

	return out[0] != 0;
#else
	return false;
#endif
}


// only shooting plays on the left weapon of a dual wield. Idle, draw, melee, reload hold the first idle pose.
// (The weapon is a plain cl_entity_t inside client.dll, so its animation can be set right here before it is drawn.)
static void COF_FreezeDualAnim( cl_entity_t *e )
{
	studiohdr_t *hdr;
	mstudioseqdesc_t *sd;
	int i, seq = e->curstate.sequence, idle = 0;

	if( !e->model || e->model->type != mod_studio )
		return;

	hdr = (studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, e->model );

	if( !hdr || seq < 0 || seq >= hdr->numseq )
		return;

	sd = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex );

	if( VR_DualAnimPlays( sd[seq].label ))
		return;

	for( i = 0; i < hdr->numseq; i++ )
	{
		if( VR_IsIdleLabel( sd[i].label ))
		{
			idle = i;
			break;
		}
	}

	e->curstate.sequence = idle;
	e->curstate.frame = 0.0f;
	e->curstate.framerate = 0.0f;
}

// draw that weapon at the left hand. In this mode the game does NOT use the view origin and angles for it: the weapon is a
// whole entity inside client.dll, and its position comes from the game's own view calculation, once per frame. So the
// entity's origin and angles are written here, right before it is drawn, and put back afterwards
qboolean VR_CofDrawDual( const float *origin, const float *pitch_yaw_roll )
{
	qboolean wrote = false;
#if defined( _WIN32 )
	float so[3], sa[3], ea[6], eb[6];
	int sflag;

	if( !cof.ok || !cof.dual_ok )
		return false;

	memcpy( so, cof.vorigin, sizeof( so ));
	memcpy( sa, cof.vangles, sizeof( sa ));
	sflag = *cof.secondary;

	memcpy( cof.vorigin, origin, sizeof( so ));
	memcpy( cof.vangles, pitch_yaw_roll, sizeof( sa ));
	*cof.secondary = 1;

	if( cof.ent_ok )
	{
		float pose[6] = { origin[0], origin[1], origin[2], -pitch_yaw_roll[0], pitch_yaw_roll[1], pitch_yaw_roll[2] };

		memcpy( ea, cof.ent_a, sizeof( ea ));
		memcpy( eb, cof.ent_b, sizeof( eb ));
		memcpy( cof.ent_a, pose, sizeof( pose ));
		memcpy( cof.ent_b, pose, sizeof( pose ));
		wrote = true;

		COF_FreezeDualAnim(( cl_entity_t * )((byte *)cof.ent_a - offsetof( cl_entity_t, origin )));
	}

	gEngfuncs.pfnDrawNormalTriangles();

	if( wrote )
	{
		memcpy( cof.ent_a, ea, sizeof( ea ));
		memcpy( cof.ent_b, eb, sizeof( eb ));
	}

	*cof.secondary = sflag;
	memcpy( cof.vorigin, so, sizeof( so ));
	memcpy( cof.vangles, sa, sizeof( sa ));
#endif
	return wrote;
}

qboolean VR_CofKeepPrepass( void )
{
	return vr_cof_prepass.value != 0.0f;
}

void VR_CofInitCvars( void )
{
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_rearm );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_frustum );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_eyeorigin );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_eyeangles );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_prepass );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_watersize );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_watercomp );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_waterscale );
	gEngfuncs.Cvar_RegisterVariable( &vr_cof_depth );
	gEngfuncs.Cmd_AddCommand( "vr_light_dump", VR_CofLightDump_f, "list the game's active lights (to tune phone / flashlight / flare / lantern lights)" );
	gEngfuncs.Cmd_AddCommand( "vr_cof_status", VR_CofStatus_f, "show whether the game's client.dll renderer is being kept in step with the VR eyes" );
}
