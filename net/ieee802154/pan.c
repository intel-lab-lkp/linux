// SPDX-License-Identifier: GPL-2.0
/*
 * IEEE 802.15.4 PAN management
 *
 * Copyright (C) 2021 Qorvo US, Inc
 * Authors:
 *   - David Girault <david.girault@qorvo.com>
 *   - Miquel Raynal <miquel.raynal@bootlin.com>
 */

#include <linux/kernel.h>
#include <net/cfg802154.h>
#include <net/af_ieee802154.h>

static bool cfg802154_same_addr(struct ieee802154_pan_device *a,
				struct ieee802154_addr *b)
{
	if (!a || !b)
		return false;

	switch (b->mode) {
	case IEEE802154_ADDR_SHORT:
		return a->short_addr == b->short_addr;
	case IEEE802154_ADDR_LONG:
		return a->extended_addr == b->extended_addr;
	default:
		return false;
	}
}

bool cfg802154_device_is_associated(struct wpan_dev *wpan_dev)
{
	bool is_assoc;

	mutex_lock(&wpan_dev->association_lock);
	is_assoc = !list_empty(&wpan_dev->children) || wpan_dev->parent;
	mutex_unlock(&wpan_dev->association_lock);

	return is_assoc;
}

bool cfg802154_device_is_parent(struct wpan_dev *wpan_dev,
				struct ieee802154_addr *target)
{
	lockdep_assert_held(&wpan_dev->association_lock);

	if (cfg802154_same_addr(wpan_dev->parent, target))
		return true;

	return false;
}
EXPORT_SYMBOL_GPL(cfg802154_device_is_parent);

struct ieee802154_pan_device *
cfg802154_device_is_child(struct wpan_dev *wpan_dev,
			  struct ieee802154_addr *target)
{
	struct ieee802154_pan_device *child;

	lockdep_assert_held(&wpan_dev->association_lock);

	list_for_each_entry(child, &wpan_dev->children, node)
		if (cfg802154_same_addr(child, target))
			return child;

	return NULL;
}
EXPORT_SYMBOL_GPL(cfg802154_device_is_child);

__le16 cfg802154_get_free_short_addr(struct wpan_dev *wpan_dev)
{
	struct ieee802154_pan_device *child;
	__le16 addr;

	lockdep_assert_held(&wpan_dev->association_lock);

	do {
		get_random_bytes(&addr, 2);
		if (addr == cpu_to_le16(IEEE802154_ADDR_SHORT_BROADCAST) ||
		    addr == cpu_to_le16(IEEE802154_ADDR_SHORT_UNSPEC))
			continue;

		if (wpan_dev->short_addr == addr)
			continue;

		if (wpan_dev->parent && wpan_dev->parent->short_addr == addr)
			continue;

		list_for_each_entry(child, &wpan_dev->children, node)
			if (child->short_addr == addr)
				continue;

		break;
	} while (1);

	return addr;
}
EXPORT_SYMBOL_GPL(cfg802154_get_free_short_addr);
