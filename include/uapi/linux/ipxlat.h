/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ipxlat.yaml */
/* YNL-GEN uapi header */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#ifndef _UAPI_LINUX_IPXLAT_H
#define _UAPI_LINUX_IPXLAT_H

#define IPXLAT_FAMILY_NAME	"ipxlat"
#define IPXLAT_FAMILY_VERSION	1

#define IPXLAT_XLAT_PREFIX6_MAX_PREFIX_LEN	96

enum {
	IPXLAT_A_POOL_PREFIX = 1,
	IPXLAT_A_POOL_PREFIX_LEN,

	__IPXLAT_A_POOL_MAX,
	IPXLAT_A_POOL_MAX = (__IPXLAT_A_POOL_MAX - 1)
};

enum {
	IPXLAT_A_CFG_XLAT_PREFIX6 = 1,
	IPXLAT_A_CFG_LOWEST_IPV6_MTU,

	__IPXLAT_A_CFG_MAX,
	IPXLAT_A_CFG_MAX = (__IPXLAT_A_CFG_MAX - 1)
};

enum {
	IPXLAT_A_DEV_IFINDEX = 1,
	IPXLAT_A_DEV_NETNSID,
	IPXLAT_A_DEV_CONFIG,

	__IPXLAT_A_DEV_MAX,
	IPXLAT_A_DEV_MAX = (__IPXLAT_A_DEV_MAX - 1)
};

enum {
	IPXLAT_CMD_DEV_GET = 1,
	IPXLAT_CMD_DEV_SET,

	__IPXLAT_CMD_MAX,
	IPXLAT_CMD_MAX = (__IPXLAT_CMD_MAX - 1)
};

#endif /* _UAPI_LINUX_IPXLAT_H */
