/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 */

#ifndef __BCM47XX_SPROM_H
#define __BCM47XX_SPROM_H

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

struct bcma_bus;
struct ssb_sprom;

#ifdef CONFIG_BCM47XX_SPROM
void bcm47xx_fill_sprom(struct ssb_sprom *sprom, const char *prefix,
			bool fallback);
int bcm47xx_sprom_register_fallbacks(void);
#else
static inline void bcm47xx_fill_sprom(struct ssb_sprom *sprom,
				      const char *prefix,
				      bool fallback)
{
}

static inline int bcm47xx_sprom_register_fallbacks(void)
{
	return -ENOTSUPP;
};
#endif

/*
 * Fill the NVRAM-only variables of a bcma bus whose SPROM was read from the
 * device. Like the SPROM fallback, only with bcma built in.
 */
#if defined(CONFIG_BCM47XX_SPROM) && IS_BUILTIN(CONFIG_BCMA)
int bcm47xx_sprom_fill_bcma_nvram(struct bcma_bus *bus, struct ssb_sprom *out);
#else
static inline int bcm47xx_sprom_fill_bcma_nvram(struct bcma_bus *bus,
						struct ssb_sprom *out)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __BCM47XX_SPROM_H */
