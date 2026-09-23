/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_QCA83XX_H
#define _LINUX_QCA83XX_H

#include <linux/bits.h>

/* QCA8K switch-to-PHY flags, carried through phy_device::dev_flags. */
#define QCA8K_DEVFLAGS_REVISION_MASK	GENMASK(2, 0)
#define QCA8K_DEVFLAGS_NO_SMARTSPEED	BIT(3)

#endif
