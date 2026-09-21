/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MMC_CORE_SDIO_CDEV_H
#define _MMC_CORE_SDIO_CDEV_H

#include <linux/mmc/sdio_func.h>

#ifdef CONFIG_MMC_SDIO_CDEV
int sdio_cdev_create(struct sdio_func *func);
void sdio_cdev_destroy(struct sdio_func *func);
#else
static inline int sdio_cdev_create(struct sdio_func *func)
{
	return 0;
}

static inline void sdio_cdev_destroy(struct sdio_func *func)
{
}
#endif

#endif /* _MMC_CORE_SDIO_CDEV_H */
