/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _NET_SHAPER_H_
#define _NET_SHAPER_H_

#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>

/**
 * enum net_shaper_metric - the metric of the shaper
 * @NET_SHAPER_METRIC_PPS: Shaper operates on a packets per second basis
 * @NET_SHAPER_METRIC_BPS: Shaper operates on a bits per second basis
 */
enum net_shaper_metric {
	NET_SHAPER_METRIC_PPS,
	NET_SHAPER_METRIC_BPS
};

/**
 * struct net_shaper_info - represents a shaping node on the NIC H/W
 * @metric: Specify if the bw limits refers to PPS or BPS
 * @bw_min: Minimum guaranteed rate for this shaper
 * @bw_max: Maximum peak bw allowed for this shaper
 * @burst: Maximum burst for the peek rate of this shaper
 * @priority: Scheduling priority for this shaper
 * @weight: Scheduling weight for this shaper
 */
struct net_shaper_info {
	enum net_shaper_metric metric;
	u64 bw_min;	/* minimum guaranteed bandwidth, according to metric */
	u64 bw_max;	/* maximum allowed bandwidth */
	u32 burst;	/* maximum burst in bytes for bw_max */
	u32 priority;	/* scheduling strict priority */
	u32 weight;	/* scheduling WRR weight*/
};

/**
 * enum net_shaper_scope - the different scopes where a shaper could be attached
 * @NET_SHAPER_SCOPE_PORT:   The root shaper for the whole H/W.
 * @NET_SHAPER_SCOPE_NETDEV: The main shaper for the given network device.
 * @NET_SHAPER_SCOPE_VF:     The shaper is attached to the given virtual
 * function.
 * @NET_SHAPER_SCOPE_QUEUE_GROUP: The shaper groups multiple queues under the
 * same device.
 * @NET_SHAPER_SCOPE_QUEUE:  The shaper is attached to the given device queue.
 *
 * NET_SHAPER_SCOPE_PORT and NET_SHAPER_SCOPE_VF are only available on
 * PF devices, usually inside the host/hypervisor.
 * NET_SHAPER_SCOPE_NETDEV, NET_SHAPER_SCOPE_QUEUE_GROUP and
 * NET_SHAPER_SCOPE_QUEUE are available on both PFs and VFs devices.
 */
enum net_shaper_scope {
	NET_SHAPER_SCOPE_PORT,
	NET_SHAPER_SCOPE_NETDEV,
	NET_SHAPER_SCOPE_VF,
	NET_SHAPER_SCOPE_QUEUE_GROUP,
	NET_SHAPER_SCOPE_QUEUE,
};

/**
 * struct net_shaper_ops - Operations on device H/W shapers
 * @add: Creates a new shaper in the specified scope.
 * @set: Modify the existing shaper.
 * @delete: Delete the specified shaper.
 * @move: Move an existing shaper under a different parent.
 *
 * The initial shaping configuration ad device initialization is empty/
 * a no-op/does not constraint the b/w in any way.
 * The network core keeps track of the applied user-configuration in
 * per device storage.
 *
 * Each shaper is uniquely identified within the device with an 'handle',
 * dependent on the shaper scope and other data, see @shaper_make_handle()
 */
struct net_shaper_ops {
	/** add - Add a shaper inside the shaper hierarchy
	 * @dev: netdevice to operate on
	 * @handle: the shaper indetifier
	 * @shaper: configuration of shaper
	 * @extack: Netlink extended ACK for reporting errors.
	 *
	 * Return:
	 * * 0 on success
	 * * %-EOPNOTSUPP - Operation is not supported by hardware, driver,
	 *                  or core for any reason. @extack should be set to
	 *                  text describing the reason.
	 * * Other negative error values on failure.
	 *
	 * Examples or reasons this operation may fail include:
	 * * H/W resources limits.
	 * * Can’t respect the requested bw limits.
	 */
	int (*add)(struct net_device *dev, u32 handle,
		   const struct net_shaper_info *shaper,
		   struct netlink_ext_ack *extack);

	/** set - Update the specified shaper, if it exists
	 * @dev: Netdevice to operate on.
	 * @handle: the shaper identifier
	 * @shaper: Configuration of shaper.
	 * @extack: Netlink extended ACK for reporting errors.
	 *
	 * Return:
	 * * %0 - Success
	 * * %-EOPNOTSUPP - Operation is not supported by hardware, driver,
	 *                  or core for any reason. @extack should be set to
	 *                  text describing the reason.
	 * * Other negative error values on failure.
	 */
	int (*set)(struct net_device *dev, u32 handle,
		   const struct net_shaper_info *shaper,
		   struct netlink_ext_ack *extack);

	/** delete - Removes a shaper from the NIC
	 * @dev: netdevice to operate on.
	 * @handle: the shaper identifier
	 * @extack: Netlink extended ACK for reporting errors.
	 *
	 * Return:
	 * * %0 - Success
	 * * %-EOPNOTSUPP - Operation is not supported by hardware, driver,
	 *                  or core for any reason. @extack should be set to
	 *                  text describing the reason.
	 * * Other negative error value on failure.
	 */
	int (*delete)(struct net_device *dev, u32 handle,
		      struct netlink_ext_ack *extack);

	/** Move - change the parent id of the specified shaper
	 * @dev: netdevice to operate on.
	 * @handle: unique identifier for the shaper
	 * @new_parent_id: identifier of the new parent for this shaper
	 * @extack: Netlink extended ACK for reporting errors.
	 *
	 * Move the specified shaper in the hierarchy replacing its
	 * current parent shaper with @new_parent_id
	 *
	 * Return:
	 * * %0 - Success
	 * * %-EOPNOTSUPP - Operation is not supported by hardware, driver,
	 *                  or core for any reason. @extack should be set to
	 *                  text describing the reason.
	 * * Other negative error values on failure.
	 */
	int (*move)(struct net_device *dev, u32 handle,
		    u32 new_parent_handle, struct netlink_ext_ack *extack);
};

/**
 * net_shaper_make_handle - creates an unique shaper identifier
 * @scope: the shaper scope
 * @vf: virtual function number
 * @id: queue group or queue id
 *
 * Return: an unique identifier for the shaper
 *
 * Combines the specified arguments to create an unique identifier for
 * the shaper.
 * The virtual function number is only used within @NET_SHAPER_SCOPE_VF,
 * @NET_SHAPER_SCOPE_QUEUE_GROUP and @NET_SHAPER_SCOPE_QUEUE.
 * The @id number is only used for @NET_SHAPER_SCOPE_QUEUE_GROUP and
 * @NET_SHAPER_SCOPE_QUEUE, and must be, respectively, the queue group
 * identifier or the queue number.
 */
u32 net_shaper_make_handle(enum net_shaper_scope scope, int vf, int id);

/*
 * Examples:
 * - set shaping on a given queue
 *   struct shaper_info info = { }; // fill this
 *   u32 handle = shaper_make_handle(NET_SHAPER_SCOPE_QUEUE, 0, queue_id);
 *   dev->shaper_ops->add(dev, handle, &info, NULL);
 *
 * - create a queue group with a queue group shaping limits.
 *   Assuming the following topology already exists:
 *                       < netdev shaper  >
 *                        /              \
 *               <queue 0 shaper> . . .  <queue N shaper>
 *
 *   struct shaper_info ginfo = { }; // fill this
 *   u32 ghandle = shaper_make_handle(NET_SHAPER_SCOPE_QUEUE_GROUP, 0, 0);
 *   dev->shaper_ops->add(dev, ghandle, &ginfo);
 *
 *   // now topology is:
 *   //                              < netdev shaper  >
 *   //                             /         |          \
 *   //                            /          |       < newly created shaper  >
 *   //                           /           |
 *   //	<queue 0 shaper> . . .    <queue N shaper>
 *
 *   // move a shapers for queues 3..n out of such queue group
 *   for (i = 0; i <= 2; ++i) {
 *       u32 qhandle = net_shaper_make_handle(NET_SHAPER_SCOPE_QUEUE, 0, i);
 *       dev->netshaper_ops->move(dev, qhandle, ghandle, NULL);
 *   }
 *
 *   // now the topology is:
 *   //                                < netdev shaper  >
 *   //                                 /            \
 *   //               < newly created shaper>   <queue 3 shaper> .. <queue n shaper>
 *   //                /               \
 *   //	<queue 0 shaper> . . .    <queue 2 shaper>
 */
#endif

