/* SPDX-License-Identifier: GPL-2.0
 * Texas Instruments K3 Inter Core Virtual Ethernet Driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef __INTER_CORE_VIRT_ETH_H__
#define __INTER_CORE_VIRT_ETH_H__

#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rpmsg.h>
#include "icve_rpmsg_common.h"

struct icve_port {
	struct icve_common *common;
} __packed;

struct icve_common {
	struct rpmsg_device *rpdev;
	struct icve_port *port;
	struct device *dev;
} __packed;

#endif /* __INTER_CORE_VIRT_ETH_H__ */
