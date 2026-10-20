/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*
*	This product contains software technology licensed from Id
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc.
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/

#ifndef EDICT_H
#define EDICT_H

#define MAX_ENT_LEAFS_32	24		// Orignally was 16
#define MAX_ENT_LEAFS_16	48

#include "progdefs.h"
#include "const.h"
#include <stddef.h>

struct edict_s
{
	qboolean		free;
	int		serialnumber;

	link_t		area;		// linked to a division node or leaf
	int		headnode;		// -1 to use normal leaf check

	int		num_leafs;
	union
	{
		int   leafnums32[MAX_ENT_LEAFS_32];
		short leafnums16[MAX_ENT_LEAFS_16];
	};

	float		freetime;		// sv.time when the object was freed

	void*		pvPrivateData;	// Alloced and freed by engine, used by DLLs
	entvars_t		v;		// C exported fields from progs

	// Cry of Fear (Steam): the retail DLLs step through edicts 0x32C bytes at a time,
	// which is 4 bytes more than entvars_t's own extra field accounts for.
	// The identity of this field is unknown. Layout found independently by
	// hajdawery/cof-enhanced (GPL-3.0) and by disassembly of hl.dll.
	int			cof_edict_tail;

	// other fields from progs come immediately after
};

// Compile-time checks: the retail game DLLs require exactly this layout.
#define XASH_COF_ASSERT( name, expr ) typedef char name[( expr ) ? 1 : -1]
XASH_COF_ASSERT( xash_cof_entvars_sequence_offset, offsetof( entvars_t, sequence ) == 0x12C );
XASH_COF_ASSERT( xash_cof_entvars_containing_offset, offsetof( entvars_t, pContainingEntity ) == 0x20C );
XASH_COF_ASSERT( xash_cof_edict_private_offset, offsetof( struct edict_s, pvPrivateData ) == 0x7C );
XASH_COF_ASSERT( xash_cof_edict_vars_offset, offsetof( struct edict_s, v ) == 0x80 );
XASH_COF_ASSERT( xash_cof_edict_stride, sizeof( struct edict_s ) == 0x32C );
#undef XASH_COF_ASSERT

#endif//EDICT_H
