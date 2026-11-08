/*
gl_vr_reload.c - manual (physical) reloading for the VR weapons
Cry of Fear VR project

Reloading works like this:
  B (eject)           the magazine leaves the gun (it is hidden), or pull it out with the left trigger on the gun
  left trigger        at your hip: a fresh magazine appears in your left hand
  move it into gun    the magazine goes in
  left trigger        on the slide / bolt: pull it back and let go (only needed when the gun was empty)
Once the sequence is complete, the real reload key is pressed for you, so the game does the ammo book keeping,
while the gun and your hands stay in the pose we draw. Nothing here changes ammo by itself.

Left grip near the gun: two-handed hold (the gun's own left arm is shown, the free left hand is hidden).

Everything that depends on a weapon model is in the table below (bone names, animation frames).
Positions of the grab zones are taken from the real geometry of the magazine and slide meshes.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/
#include "gl_vr.h"
#include "xash3d_mathlib.h"

// bit layout of vr_in_buttons / vr_in_mask (must match gl_vr_input.c and engine/client/input/in_vr.c)
enum
{
	RLB_RTRIGGER = 0, RLB_LTRIGGER, RLB_RGRIP, RLB_LGRIP, RLB_A, RLB_B
};

enum
{
	RULE_NONE = 0,	// no bolt to work
	RULE_RACK,	// when the gun was empty: pull the bolt back and let go after putting the magazine in
	RULE_LOCKED	// when the gun was empty: the bolt sits back by itself, grab it and let go
};

typedef struct
{
	const char	*model;		// file name without path and extension
	int		mag_seq, mag_frame;	// left hand holding the magazine (animation of the weapon model)
	int		bolt_seq, bolt_frame;	// left hand on the bolt. seq -1 = no animation, the free left hand is used
	const char	*mag_bones;	// exact bone names of the magazine and its bullets, separated with +
	const char	*bolt_bone;	// exact bone name of the slide / bolt / charging handle
	const char	*left_bones;	// bones of the left arm in this model (^ at the start = starts with)
	float		travel;		// how far the bolt goes back, model units
	int		rule;
	qboolean	eject_finishes;	// the eject button finishes a reload that waits for the bolt (Ruger)
	qboolean	reload;		// manual reloading is supported (otherwise only the two-handed grip)
	qboolean	aim2h;		// the left hand steers the gun when held with two hands
	qboolean	force_rack;	// the bolt always has to be worked (the empty animation of this gun is not known)
} vr_rl_def_t;

static const vr_rl_def_t rl_defs[] =
{
	// model, mag seq/frame, bolt seq/frame, magazine bones, bolt bone, left arm bones, travel, rule, eject finishes, reload, aim2h, force_rack
	{ "v_glock", 5, 30, 5, 75, "mag+bull0t", "slaidlol", "^l_", 3.0f, RULE_LOCKED, false, true, false, false },
	{ "v_vp70", 7, 47, 7, 75, "magazine+bullet2+bullet1", "slide", "^l_", 3.0f, RULE_LOCKED, false, true, false, false },
	{ "v_p345", 5, 30, -1, 0, "p_mag+p_bullets", "p_slide", "^l_", 3.0f, RULE_LOCKED, true, true, false, false },
	{ "v_g43", 29, 50, 28, 133, "Mag+rounds", "Bolt", "^l_", 3.0f, RULE_LOCKED, false, true, true, false },
	{ "v_mp5", 3, 55, 3, 15, "bone_mag", "bone_bolt", "^l_+lhand_sleevedummy+upperarm_dummyL", 3.0f, RULE_RACK, false, true, true, false },
	{ "v_m16", 4, 40, -1, 0, "houseofbullets+bulatz", "cockpuller", "^l_", 2.5f, RULE_RACK, false, true, true, false },
	// two-handed grip only, for now
	{ "v_rifle", -1, 0, -1, 0, "", "", "^l_", 0.0f, RULE_NONE, false, false, true, false },
	{ "v_shotgun", -1, 0, -1, 0, "", "", "^l_", 0.0f, RULE_NONE, false, false, true, false },
	{ "v_revolver", -1, 0, -1, 0, "", "", "^l_", 0.0f, RULE_NONE, false, false, false, false },
	{ "v_sledgehammer", -1, 0, -1, 0, "", "", "^l_", 0.0f, RULE_NONE, false, false, false, false },
	{ "v_camera", -1, 0, -1, 0, "", "", "^l_", 0.0f, RULE_NONE, false, false, false, false },
};

#define RL_DEF_COUNT	( (int)( sizeof( rl_defs ) / sizeof( rl_defs[0] )))

enum { RLE_EJECT = 0, RLE_MAGGRAB, RLE_MAGIN, RLE_BOLTBACK, RLE_BOLTFWD, RLE_COUNT };
static const char *rl_event_names[RLE_COUNT] = { "eject", "maggrab", "magin", "boltback", "boltfwd" };

typedef enum
{
	RL_READY = 0,	// normal play. the bolt can still be worked for fun
	RL_NOMAG,	// the magazine is out, get a new one from your hip
	RL_HOLDMAG,	// the left hand holds a magazine
	RL_MAGIN,	// the magazine is in, the bolt may still be needed
	RL_BOLT,	// the left hand works the bolt
	RL_GAME		// the sequence is done, the game does its own reload (we keep the pose still)
} rl_state_t;

static const char *rl_state_names[] = { "ready", "no magazine", "holding magazine", "magazine in", "working the bolt", "game reload" };

enum { RLZ_NONE = 0, RLZ_MAG, RLZ_BOLT };

static CVAR_DEFINE_AUTO( vr_rl_manual, "0", FCVAR_ARCHIVE, "1 = reload by hand (eject, magazine from your hip, bolt). 0 = the reload button just reloads, like the base game" );
static CVAR_DEFINE_AUTO( vr_rl_debug, "0", 0, "print what the reload system is doing, and how far your left hand is from each grab zone" );
static CVAR_DEFINE_AUTO( vr_rl_pouch_down, "26", FCVAR_ARCHIVE, "where the magazine pouch is: units below your head" );
static CVAR_DEFINE_AUTO( vr_rl_pouch_side, "9", FCVAR_ARCHIVE, "where the magazine pouch is: units to your left (negative = right)" );
static CVAR_DEFINE_AUTO( vr_rl_pouch_fwd, "3", FCVAR_ARCHIVE, "where the magazine pouch is: units in front of you" );
static CVAR_DEFINE_AUTO( vr_rl_pouch_radius, "14", FCVAR_ARCHIVE, "how close your left hand has to be to the pouch to take a magazine" );
static CVAR_DEFINE_AUTO( vr_rl_mag_radius, "10", FCVAR_ARCHIVE, "how close the magazine has to be to the magazine well to go in (also to pull it out by hand)" );
static CVAR_DEFINE_AUTO( vr_rl_bolt_radius, "9", FCVAR_ARCHIVE, "how close your left hand has to be to the slide / bolt to grab it" );
CVAR_DEFINE_AUTO( vr_rl_grip_x, "6", FCVAR_ARCHIVE, "centre of the grip sphere: along the weapon, from the weapon model's own origin (saved per weapon)" );
CVAR_DEFINE_AUTO( vr_rl_grip_y, "0", FCVAR_ARCHIVE, "centre of the grip sphere: sideways" );
CVAR_DEFINE_AUTO( vr_rl_grip_z, "0", FCVAR_ARCHIVE, "centre of the grip sphere: up" );
static CVAR_DEFINE_AUTO( vr_rl_grip_debug, "0", 0, "1 = show the grip sphere (a yellow ball, green while your left hand is inside it) to position it" );
static vec3_t rl_grip_center;
static qboolean rl_grip_valid, rl_grip_inside;
CVAR_DEFINE_AUTO( vr_rl_grip_radius, "12", FCVAR_ARCHIVE, "how close your left hand has to be to the gun for the left grip to hold it with two hands" );
static CVAR_DEFINE_AUTO( vr_rl_travel_scale, "1", FCVAR_ARCHIVE, "multiplies how far slides and bolts travel (1 = table value)" );
static CVAR_DEFINE_AUTO( vr_rl_always_rack, "0", FCVAR_ARCHIVE, "1 = the bolt always has to be worked after a reload, not only when the gun was empty" );
static CVAR_DEFINE_AUTO( vr_rl_mute, "reload+clip+slide+bolt+rack+cock+charge+chamber+eject+insert+magin+magout", FCVAR_ARCHIVE, "sounds of the player's weapon whose file name contains one of these parts are muted while you reload by hand" );
static CVAR_DEFINE_AUTO( vr_rl_mute_on, "0", 0, "(internal) the game's reload sounds are muted right now" );
static CVAR_DEFINE_AUTO( vr_rl_ownsound, "", 0, "(internal) the reload sound we are playing, never muted" );
static CVAR_DEFINE_AUTO( vr_rl_tune, "-1", 0, "position tuning: 0 = left hand with magazine, 1 = left hand on the bolt, 2 = right hand while reloading. -1 = off. Then change vr_rl_x/y/z/p/yw/r and run vr_rl_save" );
static CVAR_DEFINE_AUTO( vr_rl_x, "0", 0, "tuning: forward (same directions as vr_vm_grip_x)" );
static CVAR_DEFINE_AUTO( vr_rl_y, "0", 0, "tuning: left" );
static CVAR_DEFINE_AUTO( vr_rl_z, "0", 0, "tuning: up" );
static CVAR_DEFINE_AUTO( vr_rl_p, "0", 0, "tuning: pitch degrees" );
static CVAR_DEFINE_AUTO( vr_rl_yw, "0", 0, "tuning: yaw degrees" );
static CVAR_DEFINE_AUTO( vr_rl_r, "0", 0, "tuning: roll degrees" );

#define RL_TUNE_SLOTS	3

static struct
{
	const vr_rl_def_t *def;
	char		base[48];
	rl_state_t	state;
	qboolean	was_empty;	// the gun was empty when the magazine came out
	qboolean	twohand;
	qboolean	fun_bolt;	// the bolt is worked without a reload going on
	float		bolt_off;	// how far back the bolt is, model units
	float		bolt_start;	// bolt_off when the grab began
	vec3_t		grab_pos;	// left hand position when the grab began

	double		game_start, ignore_until;
	int		pulse_frames;
	qboolean	bolt_back_seen;
	qboolean	need_release;	// the left trigger must be let go before it can grab again
	qboolean	mag_clear;	// the hand has left the magazine well since it took the magazine out
	double		mute_until, eject_time;
	float		obs_anim;
	int		shot_serial;
	qboolean	mute_sent;
	char		snd[RLE_COUNT][64];
	qboolean	seen_reload;
	double		unseen_since;

	char		obs[40];	// animation of the real viewmodel as the game plays it
	double		obs_time, gun_time;

	vec3_t		gun_org, gun_ang, gun_fwd, gun_left, gun_up;
	float		gun_scale;

	vec3_t		zone[3];
	qboolean	zone_have[3];
	vec3_t		acc[3];
	int		acc_n[3];

	float		pouch_yaw;
	qboolean	pouch_init;

	float		ofs[RL_TUNE_SLOTS][6];
	int		tune_prev;
	int		prev_buttons;
	int		left_mode;	// what the left hand draw asked for last: 0 universal, 1 weapon with magazine, 2 weapon arm only
	double		dbg_next;
	qboolean	active;
} rl;

static const float rl_zero[6];

// ---------------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------------

// does the name match one entry of a '+' list? a leading ^ means "starts with", otherwise the whole name has to match
static qboolean RL_InList( const char *list, const char *name )
{
	const char *p = list;

	while( p && *p )
	{
		char tok[48];
		int n = 0;

		while( *p == '+' ) p++;
		while( *p && *p != '+' && n < 47 ) tok[n++] = *p++;
		tok[n] = 0;

		if( !n )
			continue;

		if( tok[0] == '^' )
		{
			if( n > 1 && !Q_strnicmp( name, tok + 1, n - 1 ))
				return true;
		}
		else if( !Q_stricmp( name, tok ))
		{
			return true;
		}
	}

	return false;
}

static qboolean RL_LabelHas( const char *label, const char *what )
{
	size_t n = Q_strlen( what ), i;

	for( ; *label; label++ )
	{
		for( i = 0; i < n && label[i]; i++ )
		{
			if( tolower( (unsigned char)label[i] ) != tolower( (unsigned char)what[i] ))
				break;
		}

		if( i == n )
			return true;
	}

	return false;
}

static void RL_BaseName( const char *modelname, char *out, size_t size )
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

static const char *RL_File( char *out, size_t size )
{
	Q_snprintf( out, size, "vr_reload_%s.cfg", rl.base );
	return out;
}

static float RL_Travel( void )
{
	return rl.def ? rl.def->travel * bound( 0.1f, vr_rl_travel_scale.value, 5.0f ) : 0.0f;
}

static void RL_SetState( rl_state_t s )
{
	if( rl.state == s )
		return;

	if( vr_rl_debug.value )
		gEngfuncs.Con_Printf( "VR reload: %s -> %s\n", rl_state_names[rl.state], rl_state_names[s] );

	rl.state = s;
}

static void RL_Pulse( qboolean on )
{
	gEngfuncs.Cvar_SetValue( "vr_in_reload", on ? 1.0f : 0.0f );
}

// the game's own reload starts when the magazine comes out, so it runs while you do the manual part
static void RL_StartGameReload( void )
{
	rl.pulse_frames = 12;
	RL_Pulse( true );
}

// a sound for a phase of the reload: set per weapon with vr_rl_sound <event> <path under sound/>
static void RL_Sound( int ev )
{
	char cmd[96];

	if( rl.snd[ev][0] )
	{
		gEngfuncs.Cvar_Set( "vr_rl_ownsound", rl.snd[ev] );
		Q_snprintf( cmd, sizeof( cmd ), "play %s\n", rl.snd[ev] );
		gEngfuncs.Cbuf_AddText( cmd );
	}
}

static void RL_ForgetWeapon( void )
{
	rl.def = NULL;
	rl.state = RL_READY;
	rl.twohand = false;
	rl.fun_bolt = false;
	rl.bolt_off = 0.0f;
	rl.pulse_frames = 0;
	memset( rl.snd, 0, sizeof( rl.snd ));
	memset( rl.zone_have, 0, sizeof( rl.zone_have ));
	memset( rl.acc_n, 0, sizeof( rl.acc_n ));
	memset( rl.ofs, 0, sizeof( rl.ofs ));
	RL_Pulse( false );
}

// ---------------------------------------------------------------------------------------------
// tuning of hand positions per phase, saved per weapon
// ---------------------------------------------------------------------------------------------
static void RL_Set_f( void )
{
	int slot, i;

	if( gEngfuncs.Cmd_Argc() < 8 )
	{
		gEngfuncs.Con_Printf( "usage: vr_rl_set <0-2> <x> <y> <z> <pitch> <yaw> <roll>\n" );
		return;
	}

	slot = Q_atoi( gEngfuncs.Cmd_Argv( 1 ));

	if( slot < 0 || slot >= RL_TUNE_SLOTS )
		return;

	for( i = 0; i < 6; i++ )
		rl.ofs[slot][i] = Q_atof( gEngfuncs.Cmd_Argv( 2 + i ));

	rl.tune_prev = -2;	// reload the tuning cvars from the table
}

static void RL_Sound_f( void )
{
	int i;

	if( gEngfuncs.Cmd_Argc() < 2 )
	{
		gEngfuncs.Con_Printf( "usage: vr_rl_sound <eject|maggrab|magin|boltback|boltfwd> [sound path under sound/, empty = none]\n" );
		return;
	}

	for( i = 0; i < RLE_COUNT; i++ )
	{
		if( !Q_stricmp( rl_event_names[i], gEngfuncs.Cmd_Argv( 1 )))
		{
			Q_strncpy( rl.snd[i], gEngfuncs.Cmd_Argc() > 2 ? gEngfuncs.Cmd_Argv( 2 ) : "", sizeof( rl.snd[i] ));
			return;
		}
	}

	gEngfuncs.Con_Printf( "unknown reload event '%s'\n", gEngfuncs.Cmd_Argv( 1 ));
}

static void RL_Save_f( void )
{
	char text[1024], file[96];
	int slot, len = 0;

	if( !rl.def )
	{
		gEngfuncs.Con_Printf( "VR reload: hold a weapon that has manual reloading first\n" );
		return;
	}

	for( slot = 0; slot < RL_TUNE_SLOTS; slot++ )
	{
		const float *o = rl.ofs[slot];

		len += Q_snprintf( text + len, sizeof( text ) - len, "vr_rl_set %d %g %g %g %g %g %g\n", slot, o[0], o[1], o[2], o[3], o[4], o[5] );
	}

	{
		int e;

		for( e = 0; e < RLE_COUNT; e++ )
		{
			if( rl.snd[e][0] )
				len += Q_snprintf( text + len, sizeof( text ) - len, "vr_rl_sound %s \"%s\"\n", rl_event_names[e], rl.snd[e] );
		}
	}

	RL_File( file, sizeof( file ));

	if( gEngfuncs.fsapi->WriteFile( file, text, len ))
		gEngfuncs.Con_Printf( "VR reload: saved hand positions for %s to %s\n", rl.base, file );
	else gEngfuncs.Con_Printf( S_ERROR "VR reload: can't write %s\n", file );
}

static void RL_UpdateTuning( void )
{
	int t = (int)vr_rl_tune.value;
	float *o;

	if( t < 0 || t >= RL_TUNE_SLOTS )
	{
		rl.tune_prev = -1;
		return;
	}

	o = rl.ofs[t];

	if( t != rl.tune_prev )
	{
		// a new slot was picked: show its saved values
		rl.tune_prev = t;
		gEngfuncs.Cvar_SetValue( "vr_rl_x", o[0] );
		gEngfuncs.Cvar_SetValue( "vr_rl_y", o[1] );
		gEngfuncs.Cvar_SetValue( "vr_rl_z", o[2] );
		gEngfuncs.Cvar_SetValue( "vr_rl_p", o[3] );
		gEngfuncs.Cvar_SetValue( "vr_rl_yw", o[4] );
		gEngfuncs.Cvar_SetValue( "vr_rl_r", o[5] );
		return;
	}

	o[0] = vr_rl_x.value; o[1] = vr_rl_y.value; o[2] = vr_rl_z.value;
	o[3] = vr_rl_p.value; o[4] = vr_rl_yw.value; o[5] = vr_rl_r.value;
}

// ---------------------------------------------------------------------------------------------
// weapon changes
// ---------------------------------------------------------------------------------------------
static void RL_CheckWeapon( void )
{
	char base[48], file[96], cmd[128];
	int i;

	if( !tr.viewent || !tr.viewent->model )
	{
		if( rl.def || rl.base[0] )
		{
			RL_ForgetWeapon();
			rl.base[0] = 0;
		}

		return;
	}

	RL_BaseName( tr.viewent->model->name, base, sizeof( base ));

	if( !Q_strcmp( base, rl.base ))
		return;

	RL_ForgetWeapon();
	Q_strncpy( rl.base, base, sizeof( rl.base ));

	for( i = 0; i < RL_DEF_COUNT; i++ )
	{
		if( !Q_stricmp( rl_defs[i].model, base ))
		{
			rl.def = &rl_defs[i];
			break;
		}
	}

	if( !rl.def )
		return;

	RL_File( file, sizeof( file ));

	if( gEngfuncs.fsapi->FileExists( file, false ))
	{
		Q_snprintf( cmd, sizeof( cmd ), "exec %s\n", file );
		gEngfuncs.Cbuf_AddText( cmd );
	}

	if( vr_rl_debug.value )
		gEngfuncs.Con_Printf( "VR reload: manual reloading is on for %s\n", base );
}

// ---------------------------------------------------------------------------------------------
// called by the studio renderer while the weapon is drawn
// ---------------------------------------------------------------------------------------------

// the pose the weapon is drawn at (the entity's angles are stored with the pitch negated)
void VR_ReloadSetGunPose( const vec3_t origin, const vec3_t angles, float scale )
{
	vec3_t a, fwd, right, up;

	a[PITCH] = -angles[PITCH];
	a[YAW] = angles[YAW];
	a[ROLL] = angles[ROLL];
	AngleVectors( a, fwd, right, up );

	VectorCopy( origin, rl.gun_org );
	VectorCopy( angles, rl.gun_ang );
	VectorCopy( fwd, rl.gun_fwd );
	VectorScale( right, -1.0f, rl.gun_left );
	VectorCopy( up, rl.gun_up );
	rl.gun_scale = scale;
	rl.gun_time = gp_cl->time;
}

// the animation the game itself plays on the viewmodel (before we replace it)
void VR_ReloadObserve( const char *label, float animtime )
{
	if( !label )
		label = "";

	// every new shooting animation is one shot fired (the engine puts a bullet hole where you aimed)
	if( animtime != rl.obs_anim )
	{
		rl.obs_anim = animtime;

		if(( RL_LabelHas( label, "shoot" ) || RL_LabelHas( label, "fire" ))
			&& !RL_LabelHas( label, "empty" ) && !RL_LabelHas( label, "dry" ) && !RL_LabelHas( label, "noshot" ))
		{
			rl.shot_serial++;
			gEngfuncs.Cvar_SetValue( "vr_in_shot", (float)rl.shot_serial );
		}
	}

	Q_strncpy( rl.obs, label, sizeof( rl.obs ));
	rl.obs_time = gp_cl->time;
}

// 0 = not a zone bone, 1 = magazine, 2 = bolt
int VR_ReloadBoneZone( const char *bonename )
{
	if( !rl.active || !rl.def || !rl.def->reload || !vr_rl_manual.value )
		return RLZ_NONE;

	if( RL_InList( rl.def->mag_bones, bonename ))
		return RLZ_MAG;

	if( !Q_stricmp( rl.def->bolt_bone, bonename ))
		return RLZ_BOLT;

	return RLZ_NONE;
}

void VR_ReloadZoneAccum( int zone, const float *pos )
{
	if( zone <= RLZ_NONE || zone > RLZ_BOLT )
		return;

	VectorAdd( rl.acc[zone], pos, rl.acc[zone] );
	rl.acc_n[zone]++;
}

// one weapon model has been drawn, turn the summed vertices into zone centers. then start over
void VR_ReloadZoneFlush( void )
{
	int z;

	for( z = RLZ_MAG; z <= RLZ_BOLT; z++ )
	{
		if( rl.acc_n[z] > 0 )
		{
			VectorScale( rl.acc[z], 1.0f / rl.acc_n[z], rl.zone[z] );
			rl.zone_have[z] = true;
		}

		VectorClear( rl.acc[z] );
		rl.acc_n[z] = 0;
	}
}

qboolean VR_ReloadBoneHidden( qboolean left_pass, const char *bonename, qboolean base_hidden )
{
	if( !rl.active || !rl.def )
		return base_hidden;

	if( left_pass )
	{
		// the left hand drawn from the weapon model: only the left arm (and the magazine in it)
		if( rl.left_mode == 1 )
			return !( RL_InList( rl.def->left_bones, bonename ) || RL_InList( rl.def->mag_bones, bonename ));

		if( rl.left_mode == 2 )
			return !RL_InList( rl.def->left_bones, bonename );

		return base_hidden;
	}

	// the weapon in the right hand
	if( rl.twohand && RL_InList( rl.def->left_bones, bonename ))
		return false;

	if(( rl.state == RL_NOMAG || rl.state == RL_HOLDMAG ) && RL_InList( rl.def->mag_bones, bonename ))
		return true;

	return base_hidden;
}

qboolean VR_ReloadIsBoltBone( const char *bonename )
{
	return rl.active && rl.def && !Q_stricmp( rl.def->bolt_bone, bonename );
}

// how far the slide / bolt is pulled back, as a shift in model axes (the barrel points along +X)
qboolean VR_ReloadBoltOffset( float off[3] )
{
	if( !rl.active || !rl.def || rl.bolt_off < 0.001f )
		return false;

	off[0] = -rl.bolt_off;
	off[1] = off[2] = 0.0f;
	return true;
}

// is the left hand holding the weapon right now? (every vibration meant for the right hand then also goes to the left)
qboolean VR_LeftGripping( void )
{
	return rl.active && rl.twohand;
}

// the left hand steers the gun: the weapon points from the right hand to the left hand
qboolean VR_ReloadTwoHandAim( void )
{
	return rl.active && rl.def && rl.def->aim2h && rl.twohand;
}

// the right hand weapon shows its idle pose while a manual reload is going on, so only what we move moves
qboolean VR_ReloadWantsIdle( void )
{
	if( !rl.active || !rl.def )
		return false;

	return rl.state != RL_READY || (int)vr_rl_tune.value == 2;
}

const float *VR_ReloadRightOffsets( void )
{
	if( !rl.active || !rl.def )
		return rl_zero;

	if( rl.state != RL_READY || (int)vr_rl_tune.value == 2 )
		return rl.ofs[2];

	return rl_zero;
}

// ---------------------------------------------------------------------------------------------
// the left hand
// ---------------------------------------------------------------------------------------------

// what the left hand should look like. returns false when the normal universal left hand is right
qboolean VR_ReloadLeftDraw( vr_rl_left_t *d )
{
	int mode = 0, tune = (int)vr_rl_tune.value;
	const float *o;

	rl.left_mode = 0;

	if( !rl.active || !rl.def )
		return false;

	if( tune == 0 )
		mode = 1;
	else if( tune == 1 && rl.def->bolt_seq >= 0 )
		mode = 2;
	else if( rl.twohand )
		mode = 3;
	else if( rl.state == RL_HOLDMAG )
		mode = 1;
	else if( rl.state == RL_BOLT && rl.def->bolt_seq >= 0 )
		mode = 2;

	if( !mode )
		return false;

	memset( d, 0, sizeof( *d ));

	if( mode == 3 )
	{
		d->hide = true;
		return true;
	}

	d->weapon_model = true;
	d->scale = VR_GetViewModelScale();
	rl.left_mode = mode;

	if( mode == 1 )
	{
		// free in the air, held at the left controller
		o = rl.ofs[0];
		d->seq = rl.def->mag_seq;
		d->frame = (float)rl.def->mag_frame;

		if( !VR_GetLeftHandPoseOfs( d->origin, d->angles, o ))
			d->hide = true;

		return true;
	}

	// glued to the weapon, on the bolt. the bolt itself is moved by VR_ReloadBoltOffset
	o = rl.ofs[1];
	d->seq = rl.def->bolt_seq;
	d->frame = (float)rl.def->bolt_frame;

	{
		vec3_t a;
		vec3_t org;
		float shift = ( rl.bolt_off - RL_Travel() ) * rl.gun_scale;

		// bolt animation shows the hand with the bolt all the way back, so at "forward" the hand sits further forward
		VectorMA( rl.gun_org, -shift, rl.gun_fwd, org );
		VectorMA( org, -o[0], rl.gun_fwd, org );
		VectorMA( org, -o[1], rl.gun_left, org );
		VectorMA( org, -o[2], rl.gun_up, org );
		VectorCopy( org, d->origin );

		VectorCopy( rl.gun_ang, a );
		a[PITCH] -= o[3];
		a[YAW] += o[4];
		a[ROLL] += o[5];
		VectorCopy( a, d->angles );
	}

	return true;
}

// ---------------------------------------------------------------------------------------------
// the state machine, once per frame
// ---------------------------------------------------------------------------------------------
// the manual part is done (or cancelled). if the game is still busy with its own reload we keep the pose until it is done
static void RL_Finish( void )
{
	if( vr_rl_debug.value )
		gEngfuncs.Con_Printf( "VR reload: manual part took %.1f s, the game's own reload is %s\n", gp_cl->time - rl.eject_time,
			RL_LabelHas( rl.obs, "reload" ) ? "still running (that is the wait you feel)" : "already done" );

	rl.bolt_off = 0.0f;
	rl.fun_bolt = false;
	rl.unseen_since = 0.0;

	if( RL_LabelHas( rl.obs, "reload" ))
	{
		rl.game_start = gp_cl->time;
		rl.seen_reload = true;
		RL_SetState( RL_GAME );
	}
	else
	{
		RL_SetState( RL_READY );
	}
}

static float RL_Dist( const vec3_t a, const vec3_t b )
{
	vec3_t d;

	VectorSubtract( a, b, d );
	return VectorLength( d );
}

// the magazine comes out: remember if the gun was empty, start the game's own reload right away
static void RL_Eject( qboolean empty_now, rl_state_t next )
{
	rl.was_empty = empty_now || vr_rl_always_rack.value || rl.def->force_rack;
	rl.bolt_off = ( rl.was_empty && rl.def->rule == RULE_LOCKED ) ? RL_Travel() : 0.0f;
	rl.bolt_back_seen = false;
	rl.mag_clear = false;
	rl.eject_time = gp_cl->time;
	RL_StartGameReload();
	RL_Sound( RLE_EJECT );
	RL_SetState( next );
}

void VR_ReloadTick( void )
{
	static cvar_t *cv_buttons, *cv_menu;
	vec3_t lpos, lfwd, lleft, lup, head, pouch, grip_pt;
	float head_yaw;
	int buttons, pressed, released, mask = 0;
	qboolean noswing = false;
	qboolean in_menu, gun_ok, empty_now, near_gun = false;

	if( !cv_buttons )
	{
		cv_buttons = gEngfuncs.pfnGetCvarPointer( "vr_in_buttons" );
		cv_menu = gEngfuncs.pfnGetCvarPointer( "vr_in_menu" );
	}

	buttons = cv_buttons ? (int)cv_buttons->value : 0;
	in_menu = cv_menu && cv_menu->value > 0.5f;
	pressed = buttons & ~rl.prev_buttons;
	released = ~buttons & rl.prev_buttons;
	rl.prev_buttons = buttons;

	// the reload key press lasts a fixed number of frames, whatever else happens
	if( rl.pulse_frames > 0 && --rl.pulse_frames == 0 )
		RL_Pulse( false );

	RL_CheckWeapon();
	RL_UpdateTuning();

	// the weapon has to have been drawn very recently, otherwise it is not in your hand (dead, holstered, menu)
	gun_ok = ( gp_cl->time - rl.gun_time ) < 0.5;
	rl.active = rl.def && gun_ok && !in_menu;

	if( !rl.active )
	{
		if( rl.def && !gun_ok && rl.state != RL_READY )
			RL_SetState( RL_READY );

		rl.twohand = false;
		rl.bolt_off = 0.0f;

		if( rl.mute_sent )
		{
			rl.mute_sent = false;
			gEngfuncs.Cvar_SetValue( "vr_rl_mute_on", 0.0f );
		}

		gEngfuncs.Cvar_SetValue( "vr_in_mask", 0.0f );
		gEngfuncs.Cvar_SetValue( "vr_in_noswing", 0.0f );
		return;
	}

	if( !VR_GetHandBasis( 0, lpos, lfwd, lleft, lup ))
	{
		gEngfuncs.Cvar_SetValue( "vr_in_mask", 0.0f );
		return;
	}

	empty_now = RL_LabelHas( rl.obs, "noshot" ) || RL_LabelHas( rl.obs, "empty" ) || RL_LabelHas( rl.obs, "dry" ) || RL_LabelHas( rl.obs, "nomag" );

	// two hands on the gun
	VectorMA( rl.gun_org, vr_rl_grip_x.value, rl.gun_fwd, grip_pt );
	VectorMA( grip_pt, vr_rl_grip_y.value, rl.gun_left, grip_pt );
	VectorMA( grip_pt, vr_rl_grip_z.value, rl.gun_up, grip_pt );
	VectorCopy( grip_pt, rl_grip_center );
	rl_grip_valid = true;
	rl_grip_inside = RL_Dist( lpos, grip_pt ) < vr_rl_grip_radius.value;

	if( rl.state == RL_READY || rl.state == RL_GAME )
	{
		qboolean close = RL_Dist( lpos, grip_pt ) < vr_rl_grip_radius.value;

		if( rl.twohand )
		{
			if( !( buttons & ( 1 << RLB_LGRIP )))
				rl.twohand = false;
		}
		else if(( pressed & ( 1 << RLB_LGRIP )) && close )
		{
			rl.twohand = true;

			// the left hand feels the gun: one short, light pulse
			gEngfuncs.Cvar_SetValue( "vr_in_haptic_len", 900.0f );
			gEngfuncs.Cvar_SetValue( "vr_in_haptic", 1.0f );	// 100 * 0 (left hand) + 1 short pulse
		}

		near_gun = close || rl.twohand;
	}
	else
	{
		rl.twohand = false;
	}

	// weapons without manual reloading, or manual reloading switched off: only the two-handed grip
	if( !rl.def->reload || !vr_rl_manual.value )
	{
		if( rl.state != RL_READY )
			RL_SetState( RL_READY );

		rl.bolt_off = 0.0f;

		if( rl.mute_sent )
		{
			rl.mute_sent = false;
			gEngfuncs.Cvar_SetValue( "vr_rl_mute_on", 0.0f );
		}

		if( near_gun )
			mask |= 1 << RLB_LGRIP;

		gEngfuncs.Cvar_SetValue( "vr_in_mask", (float)mask );
		gEngfuncs.Cvar_SetValue( "vr_in_noswing", 0.0f );
		return;
	}

	// where the pouch is: below the head, turning slowly with it
	VR_GetHeadGame( head, &head_yaw );

	if( !rl.pouch_init )
	{
		rl.pouch_yaw = head_yaw;
		rl.pouch_init = true;
	}
	else
	{
		float diff = head_yaw - rl.pouch_yaw;

		while( diff > 180.0f ) diff -= 360.0f;
		while( diff < -180.0f ) diff += 360.0f;
		rl.pouch_yaw += diff * 0.05f;
	}

	{
		float c = cos( DEG2RAD( rl.pouch_yaw )), s = sin( DEG2RAD( rl.pouch_yaw ));

		pouch[0] = head[0] + c * vr_rl_pouch_fwd.value - s * vr_rl_pouch_side.value;
		pouch[1] = head[1] + s * vr_rl_pouch_fwd.value + c * vr_rl_pouch_side.value;
		pouch[2] = head[2] - vr_rl_pouch_down.value;
	}

	// the game may be doing a reload on its own (an empty gun, a keyboard key): hold the pose still for it
	// the game plays a different reload animation when the gun was empty: that tells us even if the shooting
	// animations of this gun have no telltale name
	if(( rl.state == RL_NOMAG || rl.state == RL_HOLDMAG ) && RL_LabelHas( rl.obs, "reload" ) && !rl.was_empty
		&& ( RL_LabelHas( rl.obs, "noshot" ) || RL_LabelHas( rl.obs, "empty" ) || RL_LabelHas( rl.obs, "dry" ) || RL_LabelHas( rl.obs, "nomag" )))
	{
		rl.was_empty = true;

		if( rl.def->rule == RULE_LOCKED )
			rl.bolt_off = RL_Travel();

		if( vr_rl_debug.value )
			gEngfuncs.Con_Printf( "VR reload: the game's reload animation '%s' says the gun was empty\n", rl.obs );
	}

	if( rl.state == RL_READY && gp_cl->time > rl.ignore_until && RL_LabelHas( rl.obs, "reload" ))
	{
		rl.twohand = false;
		rl.game_start = gp_cl->time;
		rl.seen_reload = true;
		rl.unseen_since = 0.0;
		RL_SetState( RL_GAME );
	}

	if( !( buttons & ( 1 << RLB_LTRIGGER )))
		rl.need_release = false;

	switch( rl.state )
	{
	case RL_READY:
		// the eject button takes the magazine out
		if( pressed & ( 1 << RLB_B ))
		{
			RL_Eject( empty_now, RL_NOMAG );
			break;
		}

		// or reach for the magazine or the slide, whichever is closer. holding the trigger while you get there works too
		if(( buttons & ( 1 << RLB_LTRIGGER )) && !rl.need_release )
		{
			float dm = rl.zone_have[RLZ_MAG] ? RL_Dist( lpos, rl.zone[RLZ_MAG] ) : 9999.0f;
			float db = ( rl.def->rule != RULE_NONE && rl.zone_have[RLZ_BOLT] ) ? RL_Dist( lpos, rl.zone[RLZ_BOLT] ) : 9999.0f;
			qboolean take_mag = dm < vr_rl_mag_radius.value && dm <= db;
			qboolean take_bolt = db < vr_rl_bolt_radius.value && !take_mag;

			if( take_mag )
			{
				RL_Eject( empty_now, RL_HOLDMAG );
				break;
			}

			if( !take_bolt )
				break;


			rl.fun_bolt = true;
			rl.bolt_start = 0.0f;
			rl.bolt_back_seen = false;
			VectorCopy( lpos, rl.grab_pos );
			RL_SetState( RL_BOLT );
		}
		break;

	case RL_NOMAG:
		if(( pressed & ( 1 << RLB_LTRIGGER )) && RL_Dist( lpos, pouch ) < vr_rl_pouch_radius.value )
		{
			rl.mag_clear = true;
			RL_Sound( RLE_MAGGRAB );
			RL_SetState( RL_HOLDMAG );
		}
		break;

	case RL_HOLDMAG:
	{
		qboolean holding = ( buttons & ( 1 << RLB_LTRIGGER )) != 0;
		qboolean inside = rl.zone_have[RLZ_MAG] && RL_Dist( lpos, rl.zone[RLZ_MAG] ) < vr_rl_mag_radius.value;

		// a magazine you just pulled out of the gun can only go back in after your hand has left the magazine well
		if( !rl.mag_clear && ( !rl.zone_have[RLZ_MAG] || RL_Dist( lpos, rl.zone[RLZ_MAG] ) > vr_rl_mag_radius.value * 1.6f ))
			rl.mag_clear = true;

		if( !rl.mag_clear )
			inside = false;

		if( inside && ( holding || ( released & ( 1 << RLB_LTRIGGER ))))
		{
			// it goes in
			RL_Sound( RLE_MAGIN );
			rl.need_release = true;

			if( !rl.was_empty || rl.def->rule == RULE_NONE )
			{
				RL_Finish();
			}
			else
			{
				rl.bolt_off = rl.def->rule == RULE_LOCKED ? RL_Travel() : 0.0f;
				RL_SetState( RL_MAGIN );
			}
		}
		else if( !holding )
		{
			// let go in the air: it is gone, take another
			RL_SetState( RL_NOMAG );
		}
		break;
	}

	case RL_MAGIN:
		if( rl.def->eject_finishes && ( pressed & ( 1 << RLB_B )))
		{
			// the Ruger: the eject button lets the slide go
			RL_Sound( RLE_BOLTFWD );
			RL_Finish();
			break;
		}

		if(( buttons & ( 1 << RLB_LTRIGGER )) && !rl.need_release && rl.zone_have[RLZ_BOLT] && RL_Dist( lpos, rl.zone[RLZ_BOLT] ) < vr_rl_bolt_radius.value )
		{
			rl.fun_bolt = false;
			rl.bolt_start = rl.bolt_off;
			rl.bolt_back_seen = rl.bolt_off >= RL_Travel() * 0.85f;
			VectorCopy( lpos, rl.grab_pos );
			RL_SetState( RL_BOLT );
		}
		break;

	case RL_BOLT:
	{
		float travel = RL_Travel(), back;
		vec3_t d;

		// how far has the hand moved backwards along the barrel since the grab
		VectorSubtract( lpos, rl.grab_pos, d );
		back = -DotProduct( d, rl.gun_fwd ) / Q_max( 0.1f, rl.gun_scale );
		rl.bolt_off = bound( 0.0f, rl.bolt_start + back, travel );

		if( !rl.bolt_back_seen && rl.bolt_off >= travel * 0.85f )
		{
			rl.bolt_back_seen = true;
			RL_Sound( RLE_BOLTBACK );
		}

		if( !( buttons & ( 1 << RLB_LTRIGGER )))
		{
			if( rl.fun_bolt )
			{
				rl.bolt_off = 0.0f;
				rl.fun_bolt = false;
				RL_Sound( RLE_BOLTFWD );
				RL_SetState( RL_READY );
			}
			else if( rl.def->rule == RULE_LOCKED || rl.bolt_off >= travel * 0.85f )
			{
				RL_Sound( RLE_BOLTFWD );
				RL_Finish();
			}
			else
			{
				rl.bolt_off = 0.0f;
				RL_SetState( RL_MAGIN );
			}
		}
		break;
	}

	case RL_GAME:
		if( RL_LabelHas( rl.obs, "reload" ))
		{
			rl.unseen_since = 0.0;
		}
		else
		{
			if( !rl.unseen_since )
				rl.unseen_since = gp_cl->time;

			if( gp_cl->time - rl.unseen_since > 0.15 )
				RL_SetState( RL_READY );
		}

		// never hold the weapon longer than a reload can take, and don't jump straight back in
		if( gp_cl->time - rl.game_start > 4.0 )
		{
			rl.ignore_until = gp_cl->time + 3.0;
			RL_SetState( RL_READY );
		}
		break;
	}

	// the game's own reload sounds are hidden while we are busy, and for a moment after
	if( rl.state != RL_READY )
		rl.mute_until = gp_cl->time + 0.6;

	{
		qboolean mute = gp_cl->time < rl.mute_until;

		if( mute != rl.mute_sent )
		{
			rl.mute_sent = mute;
			gEngfuncs.Cvar_SetValue( "vr_rl_mute_on", mute ? 1.0f : 0.0f );
		}
	}

	// what the game must not see
	if( rl.state == RL_READY )
	{
		mask |= 1 << RLB_B;
	}
	else
	{
		mask |= ( 1 << RLB_RTRIGGER ) | ( 1 << RLB_B );
		noswing = true;
	}

	if( near_gun || rl.twohand )
		mask |= 1 << RLB_LGRIP;

	gEngfuncs.Cvar_SetValue( "vr_in_mask", (float)mask );
	gEngfuncs.Cvar_SetValue( "vr_in_noswing", noswing ? 1.0f : 0.0f );

	if( vr_rl_debug.value && gp_cl->time > rl.dbg_next )
	{
		rl.dbg_next = gp_cl->time + 0.5;
		gEngfuncs.Con_Printf( "VR reload: %s%s | left hand to magazine well %.1f, bolt %.1f, pouch %.1f | empty %d | bolt %.1f/%.1f | game anim '%s'\n",
			rl_state_names[rl.state], rl.twohand ? " (two hands)" : "",
			rl.zone_have[RLZ_MAG] ? RL_Dist( lpos, rl.zone[RLZ_MAG] ) : -1.0f,
			rl.zone_have[RLZ_BOLT] ? RL_Dist( lpos, rl.zone[RLZ_BOLT] ) : -1.0f,
			RL_Dist( lpos, pouch ), rl.was_empty, rl.bolt_off, RL_Travel(), rl.obs );
	}
}

void VR_ReloadInitCvars( void )
{
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_manual );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_debug );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_pouch_down );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_pouch_side );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_pouch_fwd );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_pouch_radius );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_mag_radius );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_bolt_radius );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_grip_radius );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_grip_x );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_grip_y );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_grip_z );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_grip_debug );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_travel_scale );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_always_rack );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_mute );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_mute_on );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_ownsound );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_tune );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_x );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_y );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_z );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_p );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_yw );
	gEngfuncs.Cvar_RegisterVariable( &vr_rl_r );
	gEngfuncs.Cmd_AddCommand( "vr_rl_save", RL_Save_f, "save the reload hand positions (vr_rl_tune) for the weapon you hold" );
	gEngfuncs.Cmd_AddCommand( "vr_rl_sound", RL_Sound_f, "vr_rl_sound <eject|maggrab|magin|boltback|boltfwd> <sound path>: sound for a phase of the reload (saved with vr_rl_save)" );
	gEngfuncs.Cmd_AddCommand( "vr_rl_set", RL_Set_f, "(used by the saved file) vr_rl_set <slot> x y z pitch yaw roll" );
}

// the grip sphere, as three circles (called while the controllers' debug lines are drawn)
qboolean VR_GripDebug( void )
{
	return vr_rl_grip_debug.value != 0.0f && rl_grip_valid && rl.active;
}

void VR_DrawGripSphere( void )
{
	int axis, i;
	float r = vr_rl_grip_radius.value;

	if( !VR_GripDebug())
		return;

	pglLineWidth( 2.0f );
	pglColor4f( rl_grip_inside ? 0.2f : 1.0f, 1.0f, 0.1f, 1.0f );

	for( axis = 0; axis < 3; axis++ )
	{
		pglBegin( GL_LINE_LOOP );

		for( i = 0; i < 32; i++ )
		{
			float a = i * ( M_PI_F * 2.0f / 32.0f ), c = cos( a ) * r, s = sin( a ) * r;
			vec3_t p;

			VectorCopy( rl_grip_center, p );
			p[axis] += c;
			p[( axis + 1 ) % 3] += s;
			pglVertex3fv( p );
		}

		pglEnd();
	}
}
