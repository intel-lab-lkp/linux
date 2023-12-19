// SPDX-License-Identifier: GPL-2.0
/*
 * sun4m irq support
 *
 *  djhr: Hacked out of irq.c into a CPU dependent version.
 *
 *  Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 *  Copyright (C) 1995 Miguel de Icaza (miguel@nuclecu.unam.mx)
 *  Copyright (C) 1995 Pete A. Zaitcev (zaitcev@yahoo.com)
 *  Copyright (C) 1996 Dave Redman (djhr@tadpole.co.uk)
 */

#include <linux/slab.h>
#include <linux/sched/debug.h>
#include <linux/pgtable.h>

#include <asm/timer.h>
#include <asm/traps.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/cacheflush.h>

#include "irq.h"
#include "kernel.h"

/* Sample sun4m IRQ layout:
 *
 * 0x22 - Power
 * 0x24 - ESP SCSI
 * 0x26 - Lance ethernet
 * 0x2b - Floppy
 * 0x2c - Zilog uart
 * 0x32 - SBUS level 0
 * 0x33 - Parallel port, SBUS level 1
 * 0x35 - SBUS level 2
 * 0x37 - SBUS level 3
 * 0x39 - Audio, Graphics card, SBUS level 4
 * 0x3b - SBUS level 5
 * 0x3d - SBUS level 6
 *
 * Each interrupt source has a mask bit in the interrupt registers.
 * When the mask bit is set, this blocks interrupt deliver.  So you
 * clear the bit to enable the interrupt.
 *
 * Interrupts numbered less than 0x10 are software triggered interrupts
 * and unused by Linux.
 *
 * Interrupt level assignment on sun4m:
 *
 *	level		source
 * ------------------------------------------------------------
 *	  1		softint-1
 *	  2		softint-2, VME/SBUS level 1
 *	  3		softint-3, VME/SBUS level 2
 *	  4		softint-4, onboard SCSI
 *	  5		softint-5, VME/SBUS level 3
 *	  6		softint-6, onboard ETHERNET
 *	  7		softint-7, VME/SBUS level 4
 *	  8		softint-8, onboard VIDEO
 *	  9		softint-9, VME/SBUS level 5, Module Interrupt
 *	 10		softint-10, system counter/timer
 *	 11		softint-11, VME/SBUS level 6, Floppy
 *	 12		softint-12, Keyboard/Mouse, Serial
 *	 13		softint-13, VME/SBUS level 7, ISDN Audio
 *	 14		softint-14, per-processor counter/timer
 *	 15		softint-15, Asynchronous Errors (broadcast)
 *
 * Each interrupt source is masked distinctly in the sun4m interrupt
 * registers.  The PIL level alone is therefore ambiguous, since multiple
 * interrupt sources map to a single PIL.
 *
 * This ambiguity is resolved in the 'intr' property for device nodes
 * in the OF device tree.  Each 'intr' property entry is composed of
 * two 32-bit words.  The first word is the IRQ priority value, which
 * is what we're intersted in.  The second word is the IRQ vector, which
 * is unused.
 *
 * The low 4 bits of the IRQ priority indicate the PIL, and the upper
 * 4 bits indicate onboard vs. SBUS leveled vs. VME leveled.  0x20
 * means onboard, 0x30 means SBUS leveled, and 0x40 means VME leveled.
 *
 * For example, an 'intr' IRQ priority value of 0x24 is onboard SCSI
 * whereas a value of 0x33 is SBUS level 2.  Here are some sample
 * 'intr' property IRQ priority values from ss4, ss5, ss10, ss20, and
 * Tadpole S3 GX systems.
 *
 * esp:		0x24	onboard ESP SCSI
 * le:		0x26	onboard Lance ETHERNET
 * p9100:	0x32	SBUS level 1 P9100 video
 * bpp:		0x33	SBUS level 2 BPP parallel port device
 * DBRI:	0x39	SBUS level 5 DBRI ISDN audio
 * SUNW,leo:	0x39	SBUS level 5 LEO video
 * pcmcia:	0x3b	SBUS level 6 PCMCIA controller
 * uctrl:	0x3b	SBUS level 6 UCTRL device
 * modem:	0x3d	SBUS level 7 MODEM
 * zs:		0x2c	onboard keyboard/mouse/serial
 * floppy:	0x2b	onboard Floppy
 * power:	0x22	onboard power device (XXX unknown mask bit XXX)
 */


/* Code in entry.S needs to get at these register mappings.  */
struct sun4m_irq_percpu __iomem *sun4m_irq_percpu[SUN4M_NCPUS];
struct sun4m_irq_global __iomem *sun4m_irq_global;

struct sun4m_handler_data {
	bool    percpu;
	long    mask;
};

/* Dave Redman (djhr@tadpole.co.uk)
 * The sun4m interrupt registers.
 */
#define SUN4M_INT_ENABLE	0x80000000
#define SUN4M_INT_E14		0x00000080
#define SUN4M_INT_E10		0x00080000

#define	SUN4M_INT_MASKALL	0x80000000	  /* mask all interrupts */
#define	SUN4M_INT_MODULE_ERR	0x40000000	  /* module error */
#define	SUN4M_INT_M2S_WRITE_ERR	0x20000000	  /* write buffer error */
#define	SUN4M_INT_ECC_ERR	0x10000000	  /* ecc memory error */
#define	SUN4M_INT_VME_ERR	0x08000000	  /* vme async error */
#define	SUN4M_INT_FLOPPY	0x00400000	  /* floppy disk */
#define	SUN4M_INT_MODULE	0x00200000	  /* module interrupt */
#define	SUN4M_INT_VIDEO		0x00100000	  /* onboard video */
#define	SUN4M_INT_REALTIME	0x00080000	  /* system timer */
#define	SUN4M_INT_SCSI		0x00040000	  /* onboard scsi */
#define	SUN4M_INT_AUDIO		0x00020000	  /* audio/isdn */
#define	SUN4M_INT_ETHERNET	0x00010000	  /* onboard ethernet */
#define	SUN4M_INT_SERIAL	0x00008000	  /* serial ports */
#define	SUN4M_INT_KBDMS		0x00004000	  /* keyboard/mouse */
#define	SUN4M_INT_SBUSBITS	0x00003F80	  /* sbus int bits */
#define	SUN4M_INT_VMEBITS	0x0000007F	  /* vme int bits */

#define	SUN4M_INT_ERROR		(SUN4M_INT_MODULE_ERR |    \
				 SUN4M_INT_M2S_WRITE_ERR | \
				 SUN4M_INT_ECC_ERR |       \
				 SUN4M_INT_VME_ERR)

#define SUN4M_INT_SBUS(x)	(1 << (x+7))
#define SUN4M_INT_VME(x)	(1 << (x))

/* Interrupt levels used by OBP */
#define	OBP_INT_LEVEL_SOFT	0x10
#define	OBP_INT_LEVEL_ONBOARD	0x20
#define	OBP_INT_LEVEL_SBUS	0x30
#define	OBP_INT_LEVEL_VME	0x40

#define SUN4M_TIMER_IRQ         (OBP_INT_LEVEL_ONBOARD | 10)
#define SUN4M_PROFILE_IRQ       (OBP_INT_LEVEL_ONBOARD | 14)

static unsigned long sun4m_imask[0x50] = {
	/* 0x00 - SMP */
	0,  SUN4M_SOFT_INT(1),
	SUN4M_SOFT_INT(2),  SUN4M_SOFT_INT(3),
	SUN4M_SOFT_INT(4),  SUN4M_SOFT_INT(5),
	SUN4M_SOFT_INT(6),  SUN4M_SOFT_INT(7),
	SUN4M_SOFT_INT(8),  SUN4M_SOFT_INT(9),
	SUN4M_SOFT_INT(10), SUN4M_SOFT_INT(11),
	SUN4M_SOFT_INT(12), SUN4M_SOFT_INT(13),
	SUN4M_SOFT_INT(14), SUN4M_SOFT_INT(15),
	/* 0x10 - soft */
	0,  SUN4M_SOFT_INT(1),
	SUN4M_SOFT_INT(2),  SUN4M_SOFT_INT(3),
	SUN4M_SOFT_INT(4),  SUN4M_SOFT_INT(5),
	SUN4M_SOFT_INT(6),  SUN4M_SOFT_INT(7),
	SUN4M_SOFT_INT(8),  SUN4M_SOFT_INT(9),
	SUN4M_SOFT_INT(10), SUN4M_SOFT_INT(11),
	SUN4M_SOFT_INT(12), SUN4M_SOFT_INT(13),
	SUN4M_SOFT_INT(14), SUN4M_SOFT_INT(15),
	/* 0x20 - onboard */
	0, 0, 0, 0,
	SUN4M_INT_SCSI,  0, SUN4M_INT_ETHERNET, 0,
	SUN4M_INT_VIDEO, SUN4M_INT_MODULE,
	SUN4M_INT_REALTIME, SUN4M_INT_FLOPPY,
	(SUN4M_INT_SERIAL | SUN4M_INT_KBDMS),
	SUN4M_INT_AUDIO, SUN4M_INT_E14, SUN4M_INT_MODULE_ERR,
	/* 0x30 - sbus */
	0, 0, SUN4M_INT_SBUS(0), SUN4M_INT_SBUS(1),
	0, SUN4M_INT_SBUS(2), 0, SUN4M_INT_SBUS(3),
	0, SUN4M_INT_SBUS(4), 0, SUN4M_INT_SBUS(5),
	0, SUN4M_INT_SBUS(6), 0, 0,
	/* 0x40 - vme */
	0, 0, SUN4M_INT_VME(0), SUN4M_INT_VME(1),
	0, SUN4M_INT_VME(2), 0, SUN4M_INT_VME(3),
	0, SUN4M_INT_VME(4), 0, SUN4M_INT_VME(5),
	0, SUN4M_INT_VME(6), 0, 0
};

struct sun4m_timer_percpu {
	u32		l14_limit;
	u32		l14_count;
	u32		l14_limit_noclear;
	u32		user_timer_start_stop;
};

static struct sun4m_timer_percpu __iomem *timers_percpu[SUN4M_NCPUS];

void sun4m_nmi(struct pt_regs *regs)
{
	unsigned long afsr, afar, si;

	printk(KERN_ERR "Aieee: sun4m NMI received!\n");
	/* XXX HyperSparc hack XXX */
	__asm__ __volatile__("mov 0x500, %%g1\n\t"
			     "lda [%%g1] 0x4, %0\n\t"
			     "mov 0x600, %%g1\n\t"
			     "lda [%%g1] 0x4, %1\n\t" :
			     "=r" (afsr), "=r" (afar));
	printk(KERN_ERR "afsr=%08lx afar=%08lx\n", afsr, afar);
	si = sbus_readl(&sun4m_irq_global->pending);
	printk(KERN_ERR "si=%08lx\n", si);
	if (si & SUN4M_INT_MODULE_ERR)
		printk(KERN_ERR "Module async error\n");
	if (si & SUN4M_INT_M2S_WRITE_ERR)
		printk(KERN_ERR "MBus/SBus async error\n");
	if (si & SUN4M_INT_ECC_ERR)
		printk(KERN_ERR "ECC memory error\n");
	if (si & SUN4M_INT_VME_ERR)
		printk(KERN_ERR "VME async error\n");
	printk(KERN_ERR "you lose buddy boy...\n");
	show_regs(regs);
	prom_halt();
}

void sun4m_unmask_profile_irq(void)
{
	unsigned long flags;

	local_irq_save(flags);
	sbus_writel(sun4m_imask[SUN4M_PROFILE_IRQ], &sun4m_irq_global->mask_clear);
	local_irq_restore(flags);
}

void sun4m_clear_profile_irq(int cpu)
{
	sbus_readl(&timers_percpu[cpu]->l14_limit);
}
