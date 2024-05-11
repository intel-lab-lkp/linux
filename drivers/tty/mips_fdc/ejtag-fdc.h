/* SPDX-License-Identifier: GPL-2.0
 *
 * TTY driver for MIPS EJTAG Fast Debug Channels.
 *
 * Copyright (C) 2007-2015 Imagination Technologies Ltd
 */

#ifndef __EJTAG_FDC_H
#define __EJTAG_FDC_H

#include <asm/cdmm.h>

/* Register offsets */
#define REG_FDACSR	0x00	/* FDC Access Control and Status Register */
#define REG_FDCFG	0x08	/* FDC Configuration Register */
#define REG_FDSTAT	0x10	/* FDC Status Register */
#define REG_FDRX	0x18	/* FDC Receive Register */
#define REG_FDTX(N)	(0x20+0x8*(N))	/* FDC Transmit Register n (0..15) */

/* Register fields */
#define REG_FDCFG_TXINTTHRES_SHIFT	18
#define REG_FDCFG_TXINTTHRES		(0x3 << REG_FDCFG_TXINTTHRES_SHIFT)
#define REG_FDCFG_TXINTTHRES_DISABLED	(0x0 << REG_FDCFG_TXINTTHRES_SHIFT)
#define REG_FDCFG_TXINTTHRES_EMPTY	(0x1 << REG_FDCFG_TXINTTHRES_SHIFT)
#define REG_FDCFG_TXINTTHRES_NOTFULL	(0x2 << REG_FDCFG_TXINTTHRES_SHIFT)
#define REG_FDCFG_TXINTTHRES_NEAREMPTY	(0x3 << REG_FDCFG_TXINTTHRES_SHIFT)
#define REG_FDCFG_RXINTTHRES_SHIFT	16
#define REG_FDCFG_RXINTTHRES		(0x3 << REG_FDCFG_RXINTTHRES_SHIFT)
#define REG_FDCFG_RXINTTHRES_DISABLED	(0x0 << REG_FDCFG_RXINTTHRES_SHIFT)
#define REG_FDCFG_RXINTTHRES_FULL	(0x1 << REG_FDCFG_RXINTTHRES_SHIFT)
#define REG_FDCFG_RXINTTHRES_NOTEMPTY	(0x2 << REG_FDCFG_RXINTTHRES_SHIFT)
#define REG_FDCFG_RXINTTHRES_NEARFULL	(0x3 << REG_FDCFG_RXINTTHRES_SHIFT)
#define REG_FDCFG_TXFIFOSIZE_SHIFT	8
#define REG_FDCFG_TXFIFOSIZE		(0xff << REG_FDCFG_TXFIFOSIZE_SHIFT)
#define REG_FDCFG_RXFIFOSIZE_SHIFT	0
#define REG_FDCFG_RXFIFOSIZE		(0xff << REG_FDCFG_RXFIFOSIZE_SHIFT)

#define REG_FDSTAT_TXCOUNT_SHIFT	24
#define REG_FDSTAT_TXCOUNT		(0xff << REG_FDSTAT_TXCOUNT_SHIFT)
#define REG_FDSTAT_RXCOUNT_SHIFT	16
#define REG_FDSTAT_RXCOUNT		(0xff << REG_FDSTAT_RXCOUNT_SHIFT)
#define REG_FDSTAT_RXCHAN_SHIFT		4
#define REG_FDSTAT_RXCHAN		(0xf << REG_FDSTAT_RXCHAN_SHIFT)
#define REG_FDSTAT_RXE			BIT(3)	/* Rx Empty */
#define REG_FDSTAT_RXF			BIT(2)	/* Rx Full */
#define REG_FDSTAT_TXE			BIT(1)	/* Tx Empty */
#define REG_FDSTAT_TXF			BIT(0)	/* Tx Full */

void __iomem *get_fdc_regs(void);
int is_fdc_interrupt(void);
int mips_ejtag_fdc_tty_probe(struct mips_cdmm_device *dev);
int mips_ejtag_fdc_tty_cpu_down(struct mips_cdmm_device *dev);
int mips_ejtag_fdc_tty_cpu_up(struct mips_cdmm_device *dev);

#endif /* __EJTAG_FDC_H */
