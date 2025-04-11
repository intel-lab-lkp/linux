/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define TRACE_NOC_CTRL      0x008
#define TRACE_NOC_XLD       0x010
#define TRACE_NOC_FREQVAL   0x018
#define TRACE_NOC_SYNCR     0x020

/* Enable generation of output ATB traffic.*/
#define TRACE_NOC_CTRL_PORTEN   BIT(0)
/* Sets the type of issued ATB FLAG packets.*/
#define TRACE_NOC_CTRL_FLAGTYPE BIT(7)
/* Sets the type of issued ATB FREQ packet*/
#define TRACE_NOC_CTRL_FREQTYPE BIT(8)

#define TRACE_NOC_SYN_VAL	0xFFFF

/*
 * struct trace_noc_drvdata - specifics associated to a trace noc component
 * @base:	memory mapped base address for this component.
 * @dev:	device node for trace_noc_drvdata.
 * @csdev:	component vitals needed by the framework.
 * @spinlock:	only one at a time pls.
 * @atid:	id for the trace packet.
 */
struct trace_noc_drvdata {
	void __iomem		*base;
	struct device		*dev;
	struct coresight_device	*csdev;
	spinlock_t		spinlock; /* lock for the drvdata. */
	u32			atid;
};
