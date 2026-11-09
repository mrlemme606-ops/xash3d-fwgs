/*
gl_vr_water.c - VR water reflections for Cry of Fear

Cry of Fear's own water (client.dll) is a screen space effect: it copies the finished picture and flips it around a
line on the screen. That only looks right for a camera that never tilts or rolls. In VR your head does both all the time,
and the reflection swims around.

This is a real planar reflection, made of the same ingredients as the original look: the scene itself is drawn a second
time from below the water (a mirrored camera, only what is above the water is kept), and that picture is laid over the
water surfaces of the engine, distorted by the scrolling ripples of textures/noise.tga (the file the original water
uses) and mixed with the water's own texture.

Per eye and frame it costs one more (smaller) scene pass, and only when water was in view a moment ago.
If anything fails (no GLSL, shader error) the game's own water effect is left switched on.
*/

#include "gl_local.h"
#include "gl_vr.h"
#include "xash3d_mathlib.h"

static CVAR_DEFINE_AUTO( vr_water, "1", FCVAR_ARCHIVE, "1 = real water reflections in VR (needs vr_sym_proj 1). 0 = the game's own water effect" );
static CVAR_DEFINE_AUTO( vr_water_strength, "0.75", FCVAR_ARCHIVE, "how much of the water is the reflection (0..1)" );
static CVAR_DEFINE_AUTO( vr_water_ripple, "0.02", FCVAR_ARCHIVE, "how much the ripples distort the reflection" );
static CVAR_DEFINE_AUTO( vr_water_res, "0.5", FCVAR_ARCHIVE, "size of the reflection picture compared to the eye (0.25..1). Smaller is faster" );
static CVAR_DEFINE_AUTO( vr_water_debug, "0", 0, "1 = print what the water code is doing once a second" );

#define WATER_MAX_CAND	8

static struct
{
	struct { float z; int n; } cand[WATER_MAX_CAND];
	int		ncand;

	float		z;		// height of the water plane (from the frame before)
	qboolean	valid;
	int		idle;		// frames since water was last seen

	qboolean	in_pass;
	int		pass_w, pass_h;
	float		eye_org[3];
	int		eye;

	GLuint		tex[2];
	int		tex_w[2], tex_h[2];
	qboolean	tex_ok[2];

	GLuint		prog;
	GLint		u_base, u_refl, u_noise, u_view, u_time, u_strength, u_ripple;
	qboolean	prog_tried, prog_ok;
	int		noise;

	qboolean	cg_overridden;
	float		cg_orig;

	int		passes, frames;
	double		next_print;
} water;

static const char *water_vs =
	"void main()\n"
	"{\n"
	"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
	"	gl_FrontColor = gl_Color;\n"
	"	gl_Position = ftransform();\n"
	"}\n";

// the reflection picture was drawn from the mirrored camera and is upside down, so it is read with the screen's height turned
static const char *water_fs =
	"uniform sampler2D uBase;\n"
	"uniform sampler2D uRefl;\n"
	"uniform sampler2D uNoise;\n"
	"uniform vec2 uView;\n"
	"uniform float uTime;\n"
	"uniform float uStrength;\n"
	"uniform float uRipple;\n"
	"void main()\n"
	"{\n"
	"	vec2 sc = gl_FragCoord.xy / uView;\n"
	"	vec2 st = gl_TexCoord[0].st;\n"
	"	vec2 n1 = texture2D( uNoise, st * 0.5 + vec2( uTime * 0.030, uTime * 0.020 )).rg;\n"
	"	vec2 n2 = texture2D( uNoise, st * 0.37 - vec2( uTime * 0.020, -uTime * 0.030 )).rg;\n"
	"	vec2 n = n1 + n2 - vec2( 1.0 );\n"
	"	vec2 ruv = vec2( sc.x, 1.0 - sc.y ) + n * uRipple;\n"
	"	vec3 refl = texture2D( uRefl, clamp( ruv, vec2( 0.003 ), vec2( 0.997 ))).rgb;\n"
	"	vec4 base = texture2D( uBase, st ) * gl_Color;\n"
	"	gl_FragColor = vec4( mix( base.rgb, refl, uStrength ), base.a );\n"
	"}\n";

static GLuint Water_Compile( GLenum type, const char *src )
{
	GLuint sh = pglCreateShaderObjectARB( type );
	GLint ok = 0;

	pglShaderSourceARB( sh, 1, &src, NULL );
	pglCompileShaderARB( sh );
	pglGetObjectParameterivARB( sh, 0x8B81, &ok );	// GL_COMPILE_STATUS

	if( !ok )
	{
		char log[512] = { 0 };

		pglGetInfoLogARB( sh, sizeof( log ) - 1, NULL, log );
		gEngfuncs.Con_Printf( S_ERROR "VR water: shader error: %s\n", log );
		pglDeleteObjectARB( sh );
		return 0;
	}

	return sh;
}

static void Water_BuildProgram( void )
{
	GLuint vs, fs;

	water.prog_tried = true;
	water.prog_ok = false;

	if( !GL_Support( GL_SHADER_GLSL100_EXT ) || !pglCreateProgramObjectARB || !pglUseProgramObjectARB || glConfig.max_texture_units < 3 )
	{
		gEngfuncs.Con_Printf( "VR water: no GLSL on this driver, the game's own water stays on\n" );
		return;
	}

	vs = Water_Compile( GL_VERTEX_SHADER_ARB, water_vs );
	fs = Water_Compile( GL_FRAGMENT_SHADER_ARB, water_fs );

	if( !vs || !fs )
		return;

	water.prog = pglCreateProgramObjectARB();
	pglAttachObjectARB( water.prog, vs );
	pglAttachObjectARB( water.prog, fs );
	pglLinkProgramARB( water.prog );

	water.u_base = pglGetUniformLocationARB( water.prog, "uBase" );
	water.u_refl = pglGetUniformLocationARB( water.prog, "uRefl" );
	water.u_noise = pglGetUniformLocationARB( water.prog, "uNoise" );
	water.u_view = pglGetUniformLocationARB( water.prog, "uView" );
	water.u_time = pglGetUniformLocationARB( water.prog, "uTime" );
	water.u_strength = pglGetUniformLocationARB( water.prog, "uStrength" );
	water.u_ripple = pglGetUniformLocationARB( water.prog, "uRipple" );

	// a program that did not link has no uniforms
	if( water.u_base < 0 || water.u_refl < 0 || water.u_noise < 0 || water.u_view < 0 )
	{
		gEngfuncs.Con_Printf( S_ERROR "VR water: the shader did not link, the game's own water stays on\n" );
		return;
	}

	water.noise = GL_LoadTexture( "textures/noise.tga", NULL, 0, 0 );
	water.prog_ok = true;
}

void VR_WaterInit( void )
{
	gEngfuncs.Cvar_RegisterVariable( &vr_water );
	gEngfuncs.Cvar_RegisterVariable( &vr_water_strength );
	gEngfuncs.Cvar_RegisterVariable( &vr_water_ripple );
	gEngfuncs.Cvar_RegisterVariable( &vr_water_res );
	gEngfuncs.Cvar_RegisterVariable( &vr_water_debug );
}

static qboolean Water_Wanted( void )
{
	return vr_water.value != 0.0f && VR_SymProjActive();
}

// the game's own water effect is only switched off while this one works
static void Water_ApplyGameWater( qboolean ours )
{
	cvar_t *cg = gEngfuncs.pfnGetCvarPointer( "cg_water" );

	if( !cg )
		return;

	if( ours && !water.cg_overridden )
	{
		water.cg_orig = cg->value;
		water.cg_overridden = true;
		gEngfuncs.Cvar_SetValue( "cg_water", 0.0f );
	}
	else if( !ours && water.cg_overridden )
	{
		gEngfuncs.Cvar_SetValue( "cg_water", water.cg_orig );
		water.cg_overridden = false;
	}
}

void VR_WaterShutdown( void )
{
	Water_ApplyGameWater( false );
}

// called by EmitWaterPolys for each water surface that is about to be drawn
void VR_WaterNote( const msurface_t *warp )
{
	float z;
	int i;

	if( water.in_pass || !VR_IsActive() || !warp->polys )
		return;

	if( warp->plane && fabs( warp->plane->normal[2] ) < 0.9f )
		return;

	z = warp->polys->verts[0][2];

	for( i = 0; i < water.ncand; i++ )
	{
		if( fabs( water.cand[i].z - z ) < 2.0f )
		{
			water.cand[i].n++;
			return;
		}
	}

	if( water.ncand < WATER_MAX_CAND )
	{
		water.cand[water.ncand].z = z;
		water.cand[water.ncand].n = 1;
		water.ncand++;
	}
}

// start of a VR frame: the water that was in view last frame decides what is drawn now
void VR_WaterFrameStart( void )
{
	int i, best = -1;

	if( !water.prog_tried )
		Water_BuildProgram();

	if( !Water_Wanted() || !water.prog_ok )
	{
		water.valid = false;
		Water_ApplyGameWater( false );
		water.ncand = 0;
		return;
	}

	for( i = 0; i < water.ncand; i++ )
	{
		if( best < 0 || water.cand[i].n > water.cand[best].n )
			best = i;
	}

	if( best >= 0 )
	{
		water.z = water.cand[best].z;
		water.valid = true;
		water.idle = 0;
	}
	else if( ++water.idle > 4 )
		water.valid = false;

	water.ncand = 0;
	Water_ApplyGameWater( true );

	if( vr_water_debug.value && gp_cl && gp_cl->time >= water.next_print )
	{
		gEngfuncs.Con_Printf( "VR water: plane %s at %.1f, %d reflection passes in the last second\n", water.valid ? "known" : "none", water.z, water.passes );
		water.passes = 0;
		water.next_print = gp_cl->time + 1.0;
	}
}

// the mirrored view of an eye. Returns false when there is nothing to reflect (no water, or the eye is under it)
qboolean VR_WaterSetupPass( int eye, const ref_viewpass_t *eyepass, ref_viewpass_t *out )
{
	int w, h;
	float res = bound( 0.25f, vr_water_res.value, 1.0f );

	if( !water.valid || !water.prog_ok || !Water_Wanted( ))
		return false;

	if( eyepass->vieworigin[2] < water.z + 2.0f )
		return false;

	GL_GetSceneTargetSize( &w, &h );

	if( w < 64 || h < 64 )
		return false;

	*out = *eyepass;

	// the mirror image of the camera: below the water, looking the way the reflected rays go
	out->vieworigin[2] = 2.0f * water.z - eyepass->vieworigin[2];
	out->viewangles[PITCH] = -eyepass->viewangles[PITCH];
	out->viewangles[ROLL] = -eyepass->viewangles[ROLL];

	VectorCopy( eyepass->vieworigin, water.eye_org );
	water.pass_w = Q_max( 64, (int)( w * res ));
	water.pass_h = Q_max( 64, (int)( h * res ));
	water.eye = eye & 1;
	water.in_pass = true;
	return true;
}

qboolean VR_WaterPassActive( void )
{
	return water.in_pass;
}

// the leaf (water level, fog) is that of the real eye, not of the mirror image under the water
const float *VR_WaterEyeOrigin( void )
{
	return water.eye_org;
}

// right after the engine set up the projection and modelview for the mirrored pass
void VR_WaterSetupGL( void )
{
	double eq[4] = { 0.0, 0.0, 1.0, 0.0 };

	if( !water.in_pass )
		return;

	// small picture, and only what is above the water is kept
	pglViewport( 0, 0, water.pass_w, water.pass_h );
	eq[3] = -( water.z + 0.5f );
	pglClipPlane( GL_CLIP_PLANE0, eq );
	pglEnable( GL_CLIP_PLANE0 );
}

// the mirrored pass was drawn: keep it
void VR_WaterEndPass( void )
{
	int e = water.eye, w = water.pass_w, h = water.pass_h;

	if( !water.in_pass )
		return;

	water.in_pass = false;
	pglDisable( GL_CLIP_PLANE0 );

	if( !water.tex[e] || water.tex_w[e] != w || water.tex_h[e] != h )
	{
		if( water.tex[e] )
			pglDeleteTextures( 1, &water.tex[e] );

		pglGenTextures( 1, &water.tex[e] );
		pglBindTexture( GL_TEXTURE_2D, water.tex[e] );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		water.tex_w[e] = w;
		water.tex_h[e] = h;
	}
	else pglBindTexture( GL_TEXTURE_2D, water.tex[e] );

	pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h );
	water.tex_ok[e] = true;
	water.passes++;

	// we bound a texture behind the renderer's back
	glState.currentTextures[glState.activeTMU] = -1;
	glState.currentTexturesIndex[glState.activeTMU] = -1;
}

// around the polygons of a water surface (the water's own texture is already bound on unit 0)
qboolean VR_WaterBegin( void )
{
	int e, w, h, saved;

	if( water.in_pass || !water.prog_ok || !water.valid || !VR_IsActive( ) || !Water_Wanted( ))
		return false;

	e = VR_CurrentEye() & 1;

	if( !water.tex_ok[e] )
		return false;

	GL_GetSceneTargetSize( &w, &h );
	saved = glState.activeTMU;

	pglUseProgramObjectARB( water.prog );
	pglUniform1iARB( water.u_base, 0 );
	pglUniform1iARB( water.u_refl, 1 );
	pglUniform1iARB( water.u_noise, 2 );
	pglUniform2fARB( water.u_view, (float)w, (float)h );
	pglUniform1fARB( water.u_time, gp_cl ? (float)gp_cl->time : 0.0f );
	pglUniform1fARB( water.u_strength, bound( 0.0f, vr_water_strength.value, 1.0f ));
	pglUniform1fARB( water.u_ripple, bound( 0.0f, vr_water_ripple.value, 0.2f ));

	GL_SelectTexture( 1 );
	pglBindTexture( GL_TEXTURE_2D, water.tex[e] );
	GL_SelectTexture( 2 );
	GL_Bind( 2, water.noise );
	GL_SelectTexture( saved );
	return true;
}

void VR_WaterEnd( void )
{
	int saved = glState.activeTMU;

	pglUseProgramObjectARB( 0 );

	// units 1 and 2 were bound behind the renderer's back
	GL_SelectTexture( 1 );
	glState.currentTextures[1] = -1;
	glState.currentTexturesIndex[1] = -1;
	GL_SelectTexture( 2 );
	glState.currentTextures[2] = -1;
	glState.currentTexturesIndex[2] = -1;
	GL_SelectTexture( saved );
}
