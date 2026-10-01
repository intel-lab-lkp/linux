/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2000 Silicon Graphics, Inc.
 * Copyright (C) 2005 Ralf Baechle <ralf@linux-mips.org>
 */
#ifndef __ASM_MACH_IP27_KERNEL_ENTRY_H
#define __ASM_MACH_IP27_KERNEL_ENTRY_H

#include <asm/sn/addrs.h>
#include <asm/sn/agent.h>
#include <asm/sn/klkernvars.h>

/*
 * TLB bits
 */
#define PAGE_GLOBAL		(1 << 6)
#define PAGE_VALID		(1 << 7)
#define PAGE_DIRTY		(1 << 8)
#define CACHE_CACHABLE_COW	(5 << 9)

	/*
	 * A Bedrock hub puts the nasid one bit higher in an address than a
	 * Hub does.  Returns the shift in \shft, clobbering \tmp.
	 */
	.macro	GET_NASID_SHFT shft, tmp
	.set	push
	.set	reorder
	GET_HUB_GEN_ASM \tmp
	li	\shft, SN1_NASID_SHFT
	beqz	\tmp, 1f
	li	\shft, SN0_NASID_SHFT
1:
	.set	pop
	.endm

	/*
	 * inputs are the text nasid in t1, data nasid in t2.
	 */
	.macro MAPPED_KERNEL_SETUP_TLB
#ifdef CONFIG_MAPPED_KERNEL
	/*
	 * This needs to read the nasid - assume 0 for now.
	 * Drop in 0xffffffffc0000000 in tlbhi, 0+VG in tlblo_0,
	 * 0+DVG in tlblo_1.
	 */
	dli	t0, 0xffffffffc0000000
	dmtc0	t0, CP0_ENTRYHI
	GET_NASID_SHFT	t3, t0
	li	t0, 0x1c000		# Offset of text into node memory
	dsllv	t1, t1, t3		# Shift text nasid into place
	dsllv	t2, t2, t3		# Same for data nasid
	or	t1, t1, t0		# Physical load address of kernel text
	or	t2, t2, t0		# Physical load address of kernel data
	dsrl	t1, 12			# 4K pfn
	dsrl	t2, 12			# 4K pfn
	dsll	t1, 6			# Get pfn into place
	dsll	t2, 6			# Get pfn into place
	li	t0, ((PAGE_GLOBAL | PAGE_VALID | CACHE_CACHABLE_COW) >> 6)
	or	t0, t0, t1
	mtc0	t0, CP0_ENTRYLO0	# physaddr, VG, cach exlwr
	li	t0, ((PAGE_GLOBAL | PAGE_VALID |  PAGE_DIRTY | CACHE_CACHABLE_COW) >> 6)
	or	t0, t0, t2
	mtc0	t0, CP0_ENTRYLO1	# physaddr, DVG, cach exlwr
	li	t0, 0x1ffe000		# MAPPED_KERN_TLBMASK, TLBPGMASK_16M
	mtc0	t0, CP0_PAGEMASK
	li	t0, 0			# KMAP_INX
	mtc0	t0, CP0_INDEX
	li	t0, 1
	mtc0	t0, CP0_WIRED
	tlbwi
#else
	mtc0	zero, CP0_WIRED
#endif
	.endm

/*
 * Intentionally empty macro, used in head.S. Override in
 * arch/mips/mach-xxx/kernel-entry-init.h when necessary.
 */
	.macro	kernel_entry_setup
#ifdef CONFIG_MAPPED_KERNEL
	GET_NASID_ASM	t1, t0
	move		t2, t1			# text and data are here
#endif
	MAPPED_KERNEL_SETUP_TLB
	.endm

/*
 * Do SMP slave processor setup necessary before we can safely execute C code.
 */
	.macro	smp_slave_setup
#ifdef CONFIG_MAPPED_KERNEL
	GET_NASID_ASM	t1, t0
	GET_NASID_SHFT	t3, t0
	dli	t0, KLDIR_OFFSET + (KLI_KERN_VARS * KLDIR_ENT_SIZE) + \
		    KLDIR_OFF_POINTER + CAC_BASE
	dsllv	t1, t1, t3
	or	t0, t0, t1
	ld	t0, 0(t0)			# t0 points to kern_vars struct
	lh	t1, KV_RO_NASID_OFFSET(t0)
	lh	t2, KV_RW_NASID_OFFSET(t0)
#endif
	MAPPED_KERNEL_SETUP_TLB

	/*
	 * We might not get launched at the address the kernel is linked to,
	 * so we jump there.
	 */
	PTR_LA	t0, 0f
	jr	t0
0:
	.endm

#endif /* __ASM_MACH_IP27_KERNEL_ENTRY_H */
