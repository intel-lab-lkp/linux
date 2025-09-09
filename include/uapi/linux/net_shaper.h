/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/net_shaper.yaml */
/* YNL-GEN uapi header */

#ifndef _UAPI_LINUX_NET_SHAPER_H
#define _UAPI_LINUX_NET_SHAPER_H

#define NET_SHAPER_FAMILY_NAME		"net-shaper"
#define NET_SHAPER_FAMILY_VERSION	1

/**
 * enum net_shaper_scope - Defines the shaper @id interpretation.
 * @NET_SHAPER_SCOPE_UNSPEC: The scope is not specified.
 * @NET_SHAPER_SCOPE_NETDEV: The main shaper for the given network device.
 * @NET_SHAPER_SCOPE_QUEUE: The shaper is attached to the given device queue,
 *   the @id represents the queue number.
 * @NET_SHAPER_SCOPE_NODE: The shaper allows grouping of queues or other node
 *   shapers; can be nested in either @netdev shapers or other @node shapers,
 *   allowing placement in any location of the scheduling tree, except leaves
 *   and root.
 */
enum net_shaper_scope {
	NET_SHAPER_SCOPE_UNSPEC,
	NET_SHAPER_SCOPE_NETDEV,
	NET_SHAPER_SCOPE_QUEUE,
	NET_SHAPER_SCOPE_NODE,

	/* private: */
	__NET_SHAPER_SCOPE_MAX,
	NET_SHAPER_SCOPE_MAX = (__NET_SHAPER_SCOPE_MAX - 1)
};

/**
 * enum net_shaper_metric - Different metric supported by the shaper.
 * @NET_SHAPER_METRIC_BPS: Shaper operates on a bits per second basis.
 * @NET_SHAPER_METRIC_PPS: Shaper operates on a packets per second basis.
 */
enum net_shaper_metric {
	NET_SHAPER_METRIC_BPS,
	NET_SHAPER_METRIC_PPS,
};

/**
 * enum net_shaper_net_shaper
 * @NET_SHAPER_A_HANDLE: Unique identifier for the given shaper inside the
 *   owning device.
 * @NET_SHAPER_A_METRIC: Metric used by the given shaper for bw-min, bw-max and
 *   burst.
 * @NET_SHAPER_A_BW_MIN: Guaranteed bandwidth for the given shaper.
 * @NET_SHAPER_A_BW_MAX: Maximum bandwidth for the given shaper or 0 when
 *   unlimited.
 * @NET_SHAPER_A_BURST: Maximum burst-size for shaping. Should not be
 *   interpreted as a quantum.
 * @NET_SHAPER_A_PRIORITY: Scheduling priority for the given shaper. The
 *   priority scheduling is applied to sibling shapers.
 * @NET_SHAPER_A_WEIGHT: Relative weight for round robin scheduling of the
 *   given shaper. The scheduling is applied to all sibling shapers with the
 *   same priority.
 * @NET_SHAPER_A_IFINDEX: Interface index owning the specified shaper.
 * @NET_SHAPER_A_PARENT: Identifier for the parent of the affected shaper. Only
 *   needed for @group operation.
 * @NET_SHAPER_A_LEAVES: Describes a set of leaves shapers for a @group
 *   operation.
 */
enum {
	NET_SHAPER_A_HANDLE = 1,
	NET_SHAPER_A_METRIC,
	NET_SHAPER_A_BW_MIN,
	NET_SHAPER_A_BW_MAX,
	NET_SHAPER_A_BURST,
	NET_SHAPER_A_PRIORITY,
	NET_SHAPER_A_WEIGHT,
	NET_SHAPER_A_IFINDEX,
	NET_SHAPER_A_PARENT,
	NET_SHAPER_A_LEAVES,

	__NET_SHAPER_A_MAX,
	NET_SHAPER_A_MAX = (__NET_SHAPER_A_MAX - 1)
};

/**
 * enum net_shaper_handle
 * @NET_SHAPER_A_HANDLE_SCOPE: Defines the shaper @id interpretation.
 * @NET_SHAPER_A_HANDLE_ID: Numeric identifier of a shaper. The id semantic
 *   depends on the scope. For @queue scope it's the queue id and for @node
 *   scope it's the node identifier.
 */
enum {
	NET_SHAPER_A_HANDLE_SCOPE = 1,
	NET_SHAPER_A_HANDLE_ID,

	__NET_SHAPER_A_HANDLE_MAX,
	NET_SHAPER_A_HANDLE_MAX = (__NET_SHAPER_A_HANDLE_MAX - 1)
};

/**
 * enum net_shaper_caps
 * @NET_SHAPER_A_CAPS_IFINDEX: Interface index queried for shapers
 *   capabilities.
 * @NET_SHAPER_A_CAPS_SCOPE: The scope to which the queried capabilities apply.
 * @NET_SHAPER_A_CAPS_SUPPORT_METRIC_BPS: The device accepts 'bps' metric for
 *   bw-min, bw-max and burst.
 * @NET_SHAPER_A_CAPS_SUPPORT_METRIC_PPS: The device accepts 'pps' metric for
 *   bw-min, bw-max and burst.
 * @NET_SHAPER_A_CAPS_SUPPORT_NESTING: The device supports nesting shaper
 *   belonging to this scope below 'node' scoped shapers. Only 'queue' and
 *   'node' scope can have flag 'support-nesting'.
 * @NET_SHAPER_A_CAPS_SUPPORT_BW_MIN: The device supports a minimum guaranteed
 *   B/W.
 * @NET_SHAPER_A_CAPS_SUPPORT_BW_MAX: The device supports maximum B/W shaping.
 * @NET_SHAPER_A_CAPS_SUPPORT_BURST: The device supports a maximum burst size.
 * @NET_SHAPER_A_CAPS_SUPPORT_PRIORITY: The device supports priority
 *   scheduling.
 * @NET_SHAPER_A_CAPS_SUPPORT_WEIGHT: The device supports weighted round robin
 *   scheduling.
 */
enum {
	NET_SHAPER_A_CAPS_IFINDEX = 1,
	NET_SHAPER_A_CAPS_SCOPE,
	NET_SHAPER_A_CAPS_SUPPORT_METRIC_BPS,
	NET_SHAPER_A_CAPS_SUPPORT_METRIC_PPS,
	NET_SHAPER_A_CAPS_SUPPORT_NESTING,
	NET_SHAPER_A_CAPS_SUPPORT_BW_MIN,
	NET_SHAPER_A_CAPS_SUPPORT_BW_MAX,
	NET_SHAPER_A_CAPS_SUPPORT_BURST,
	NET_SHAPER_A_CAPS_SUPPORT_PRIORITY,
	NET_SHAPER_A_CAPS_SUPPORT_WEIGHT,

	__NET_SHAPER_A_CAPS_MAX,
	NET_SHAPER_A_CAPS_MAX = (__NET_SHAPER_A_CAPS_MAX - 1)
};

enum {
	NET_SHAPER_CMD_GET = 1,
	NET_SHAPER_CMD_SET,
	NET_SHAPER_CMD_DELETE,
	NET_SHAPER_CMD_GROUP,
	NET_SHAPER_CMD_CAP_GET,

	__NET_SHAPER_CMD_MAX,
	NET_SHAPER_CMD_MAX = (__NET_SHAPER_CMD_MAX - 1)
};

#endif /* _UAPI_LINUX_NET_SHAPER_H */
