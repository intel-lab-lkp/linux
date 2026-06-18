/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/include/linux/timeriomem-rng.h
 *
 * Copyright (c) 2009 Alexander Clouter <alex@digriz.org.uk>
 */

#ifndef _LINUX_TIMERIOMEM_RNG_H
#define _LINUX_TIMERIOMEM_RNG_H

struct timeriomem_rng_data {
	void __iomem		*address;

	/* measures in usecs */
	unsigned int		period;

	/* bits of entropy per 1024 bits read */
	unsigned int		quality;

	/* read width (1, 2, or 4 bytes), 0 means 4 */
	unsigned int		reg_io_width;

	/* set to true if reg-io-width is explicitly provided */
	bool			reg_io_width_set;

	/* mask applied to raw read value */
	u32			mask;

	/* set to true if mask is explicitly provided */
	bool			mask_set;
};

#endif /* _LINUX_TIMERIOMEM_RNG_H */
