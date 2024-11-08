// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * switchdev.c
 *
 *	Authors:
 *	Hans J. Schultz		<netdev@kapio-technology.com>
 *
 */

#include <net/switchdev.h>
#include "chip.h"
#include "global1.h"
#include "switchdev.h"

static int mv88e6xxx_find_vid(struct mv88e6xxx_chip *chip, u16 fid, u16 *vid)
{
	*vid = chip->vid_cache[fid];

	return *vid ? 0 : -ENOENT;
}

int mv88e6xxx_handle_miss_violation(struct mv88e6xxx_chip *chip, int port,
				    struct mv88e6xxx_atu_entry *entry, u16 fid)
{
	struct switchdev_notifier_fdb_info info = {
		.addr = entry->mac,
		.locked = true,
	};
	struct net_device *brport;
	struct dsa_port *dp;
	u16 vid;
	int err;

	err = mv88e6xxx_find_vid(chip, fid, &vid);
	if (err)
		return err;

	info.vid = vid;
	dp = dsa_to_port(chip->ds, port);

	rtnl_lock();
	brport = dsa_port_to_bridge_port(dp);
	if (!brport) {
		rtnl_unlock();
		return -ENODEV;
	}
	err = call_switchdev_notifiers(SWITCHDEV_FDB_ADD_TO_BRIDGE,
				       brport, &info.info, NULL);
	rtnl_unlock();

	return notifier_to_errno(err);
}

int mv88e6xxx_handle_member_violation(struct mv88e6xxx_chip *chip, int port,
				      struct mv88e6xxx_atu_entry *entry, u16 fid)
{
	struct switchdev_notifier_fdb_info info = {
		.addr = entry->mac,
	};
	struct net_device *brport;
	struct dsa_port *dp;
	u16 vid;
	int err;

	err = mv88e6xxx_find_vid(chip, fid, &vid);
	if (err)
		return err;

	info.vid = vid;
	dp = dsa_to_port(chip->ds, port);

	rtnl_lock();
	brport = dsa_port_to_bridge_port(dp);
	if (!brport) {
		rtnl_unlock();
		return -ENODEV;
	}
	err = call_switchdev_notifiers(SWITCHDEV_FDB_ADD_TO_BRIDGE,
				       brport, &info.info, NULL);
	rtnl_unlock();

	return notifier_to_errno(err);
}
