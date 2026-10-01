/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1992 - 1997, 1999 Silicon Graphics, Inc.
 * Copyright (C) 1999 by Ralf Baechle
 */
#ifndef _ASM_SN_SN0_HUB_H
#define _ASM_SN_SN0_HUB_H

/* The secret password; used to release protection */
#define HUB_PASSWORD		0x53474972756c6573ull

#define CHIPID_HUB		0
#define CHIPID_ROUTER		1

#define HUB_REV_1_0		1
#define HUB_REV_2_0		2
#define HUB_REV_2_1		3
#define HUB_REV_2_2		4
#define HUB_REV_2_3		5
#define HUB_REV_2_4		6

#define MAX_HUB_PATH		80

#include <asm/sn/sn0/addrs.h>
#include <asm/sn/sn0/hubpi.h>
#include <asm/sn/sn0/hubmd.h>
#include <asm/sn/sn0/hubio.h>
#include <asm/sn/sn0/hubni.h>
//#include <asm/sn/sn0/hubcore.h>

#include <asm/xtalk/xwidget.h>

/* Translation of uncached attributes */
#define UATTR_HSPEC	0
#define UATTR_IO	1
#define UATTR_MSPEC	2
#define UATTR_UNCAC	3

#ifdef __ASSEMBLER__
/*
 * Clears tmp on a Bedrock hub.  Nothing has read the hub's part number
 * this early, so take it from the widget ID.
 */
	.macro GET_HUB_GEN_ASM tmp
	dli	\tmp, LOCAL_HUB_ADDR(IIO_WID)
	ld	\tmp, (\tmp)
	dsrl	\tmp, WIDGET_PART_NUM_SHFT
	andi	\tmp, (WIDGET_PART_NUM >> WIDGET_PART_NUM_SHFT)
	xori	\tmp, WIDGET_BDRCK_PART_NUM
	.endm

/*
 * Returns the local nasid into res, clobbering tmp.
 */
	.macro GET_NASID_ASM res, tmp
	.set	push
	.set	reorder
	GET_HUB_GEN_ASM \tmp
	dli	\res, LOCAL_HUB_ADDR(NI_STATUS_REV_ID)
	ld	\res, (\res)
	beqz	\tmp, 1f
	and	\res, NSRI_NODEID_MASK
	dsrl	\res, NSRI_NODEID_SHFT
	b	2f
1:
	dsrl	\res, LRI_NODEID_SHFT_IP35
	andi	\res, LRI_NODEID_MASK_IP35
2:
	.set	pop
	.endm
#else

/*
 * get_nasid() returns the physical node id number of the caller.
 */
static inline nasid_t get_nasid(void)
{
	u64 id = LOCAL_HUB_L(NI_STATUS_REV_ID);

	if (system_is_ip35)
		return (nasid_t)((id >> LRI_NODEID_SHFT_IP35) &
				 LRI_NODEID_MASK_IP35);

	return (nasid_t)((id & NSRI_NODEID_MASK) >> NSRI_NODEID_SHFT);
}
#endif

#endif /* _ASM_SN_SN0_HUB_H */
