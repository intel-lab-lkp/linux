/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_CORESIGHT_STM_H_
#define __LINUX_CORESIGHT_STM_H_

#include <uapi/linux/coresight-stm.h>
#include <linux/coresight.h>
#include <linux/stm.h>

struct channel_space {
	void __iomem            *base;
	phys_addr_t             phys;
	unsigned long           *guaranteed;
};

/**
 * struct stm_drvdata - specifics associated to an STM component
 * @base:		memory mapped base address for this component.
 * @atclk:		optional clock for the core parts of the STM.
 * @pclk:		APB clock if present, otherwise NULL
 * @csdev:		component vitals needed by the framework.
 * @spinlock:		only one at a time pls.
 * @chs:		the channels accociated to this STM.
 * @stm:		structure associated to the generic STM interface.
 * @traceid:		value of the current ID for this component.
 * @write_bytes:	Maximus bytes this STM can write at a time.
 * @stmsper:		settings for register STMSPER.
 * @stmspscr:		settings for register STMSPSCR.
 * @numsp:		the total number of stimulus port support by this STM.
 * @stmheer:		settings for register STMHEER.
 * @stmheter:		settings for register STMHETER.
 * @stmhebsr:		settings for register STMHEBSR.
 */
struct stm_drvdata {
	void __iomem		*base;
	struct clk		*atclk;
	struct clk		*pclk;
	struct coresight_device	*csdev;
	spinlock_t		spinlock;
	struct channel_space	chs;
	struct stm_data		stm;
	u8			traceid;
	u32			write_bytes;
	u32			stmsper;
	u32			stmspscr;
	u32			numsp;
	u32			stmheer;
	u32			stmheter;
	u32			stmhebsr;
};

#endif
