/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2017 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
 * Copyright 2021 NXP
 */

#ifndef _IMX_RPROC_H
#define _IMX_RPROC_H

#include <linux/of_reserved_mem.h>

/* address translation table */
struct imx_rproc_att {
	u32 da;	/* device address (From Cortex M4 view)*/
	u32 sa;	/* system bus address */
	u32 size; /* size of reg range */
	int flags;
};

/* dcfg flags */
#define IMX_RPROC_NEED_SYSTEM_OFF	BIT(0)
#define IMX_RPROC_NEED_CLKS		BIT(1)

struct imx_rproc_plat_ops {
	int (*start)(struct rproc *rproc);
	int (*stop)(struct rproc *rproc);
	int (*detach)(struct rproc *rproc);
	int (*detect_mode)(struct rproc *rproc);
	int (*prepare)(struct rproc *rproc);
};

struct imx_rproc_dcfg {
	u32				src_reg;
	u32				src_mask;
	u32				src_start;
	u32				src_stop;
	u32				gpr_reg;
	u32				gpr_wait;
	const struct imx_rproc_att	*att;
	size_t				att_size;
	u32				flags;
	const struct imx_rproc_plat_ops	*ops;
	/* For System Manager(SM) based SoCs */
	u32				cpuid; /* ID of the remote core */
	u32				lmid;  /* ID of the Logcial Machine */
	/* reset_vector = elf_entry_addr & reset_vector_mask */
	u32				reset_vector_mask;
};

static inline int imx_rproc_rmem_to_resource(struct device_node *np,
					     int index,
					     struct resource *res)
{
	int ret;

	ret = of_reserved_mem_region_to_resource(np, index, res);
	if (ret)
		return ret;

	/* "memory-region-names" is optional */
	ret = of_property_read_string_index(np, "memory-region-names",
					    index, &res->name);
	if (ret == -EINVAL)
		return 0;

	return ret;
}

#endif /* _IMX_RPROC_H */
