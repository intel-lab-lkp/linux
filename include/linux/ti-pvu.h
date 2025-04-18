/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TI Peripheral Virtualization Unit driver for static DMA isolation
 *
 * Copyright (c) 2024, Siemens AG
 */

#ifndef _LINUX_TI_PVU_H
#define _LINUX_TI_PVU_H

#include <linux/ioport.h>

#if IS_ENABLED(CONFIG_TI_PVU)
int ti_pvu_create_region(unsigned int virt_id,
			 const struct resource *region);
int ti_pvu_remove_region(unsigned int virt_id,
			 const struct resource *region);
#else
static inline int ti_pvu_create_region(unsigned int virt_id,
				       const struct resource *region)
{
	return 0;
}

static inline int ti_pvu_remove_region(unsigned int virt_id,
				       const struct resource *region)
{
	return 0;
}
#endif

#endif /* _LINUX_TI_PVU_H */
