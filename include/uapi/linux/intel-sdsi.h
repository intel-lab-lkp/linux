/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Intel On Demand SPDM Interface
 * Copyright (c) 2023, Intel Corporation.
 * All rights reserved.
 *
 * Author: David E. Box <david.e.box@linux.intel.com>
 */

#ifndef __SDSI_NL_H
#define __SDSI_NL_H

#define SDSI_FAMILY_NAME	"intel_sdsi"
#define SDSI_FAMILY_VERSION	1

enum {
	SDSI_GENL_ATTR_UNSPEC,
	SDSI_GENL_ATTR_DEVS,		/* nested */
	SDSI_GENL_ATTR_DEV_ID,		/* u32, device id */
	SDSI_GENL_ATTR_DEV_NAME,	/* string, device name */
	SDSI_GENL_ATTR_SPDM_REQ,	/* binary, SDPM request message */
	SDSI_GENL_ATTR_SPDM_RSP,	/* binary, SDPM response message */
	SDSI_GENL_ATTR_SPDM_REQ_SIZE,	/* u32, max SDPM request size */
	SDSI_GENL_ATTR_SPDM_RSP_SIZE,	/* u32, max SPDM response size */

	__SDSI_GENL_ATTR_MAX,
	SDSI_GENL_ATTR_MAX = (__SDSI_GENL_ATTR_MAX - 1)
};

enum {
	SDSI_GENL_CMD_UNSPEC,
	SDSI_GENL_CMD_GET_DEVS,		/* Get On Demand device list */
	SDSI_GENL_CMD_GET_INFO,		/* Get On Demand device info */
	SDSI_GENL_CMD_GET_SPDM,		/* Get SPDM response to SPDM request */

	__SDSI_GENL_CMD_MAX,
	SDSI_GENL_CMD_MAX = (__SDSI_GENL_CMD_MAX - 1)
};

#endif /* __SDSI_NL_H */
