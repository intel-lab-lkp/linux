/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DEVICE_LINK_H_
#define _DEVICE_LINK_H_

#include <linux/bits.h>
#include <linux/kref.h>
#include <linux/refcount_types.h>
#include <linux/types.h>
#include <linux/workqueue_types.h>

#include "types.h"

/**
 * enum device_link_state - Device link states.
 * @DL_STATE_NONE: The presence of the drivers is not being tracked.
 * @DL_STATE_DORMANT: None of the supplier/consumer drivers is present.
 * @DL_STATE_AVAILABLE: The supplier driver is present, but the consumer is not.
 * @DL_STATE_CONSUMER_PROBE: The consumer is probing (supplier driver present).
 * @DL_STATE_ACTIVE: Both the supplier and consumer drivers are present.
 * @DL_STATE_SUPPLIER_UNBIND: The supplier driver is unbinding.
 */
enum device_link_state {
	DL_STATE_NONE = -1,
	DL_STATE_DORMANT = 0,
	DL_STATE_AVAILABLE,
	DL_STATE_CONSUMER_PROBE,
	DL_STATE_ACTIVE,
	DL_STATE_SUPPLIER_UNBIND,
};

/*
 * Device link flags.
 *
 * STATELESS: The core will not remove this link automatically.
 * AUTOREMOVE_CONSUMER: Remove the link automatically on consumer driver unbind.
 * PM_RUNTIME: If set, the runtime PM framework will use this link.
 * RPM_ACTIVE: Run pm_runtime_get_sync() on the supplier during link creation.
 * AUTOREMOVE_SUPPLIER: Remove the link automatically on supplier driver unbind.
 * AUTOPROBE_CONSUMER: Probe consumer driver automatically after supplier binds.
 * MANAGED: The core tracks presence of supplier/consumer drivers (internal).
 * SYNC_STATE_ONLY: Link only affects sync_state() behavior.
 * INFERRED: Inferred from data (eg: firmware) and not from driver actions.
 */
#define DL_FLAG_STATELESS		BIT(0)
#define DL_FLAG_AUTOREMOVE_CONSUMER	BIT(1)
#define DL_FLAG_PM_RUNTIME		BIT(2)
#define DL_FLAG_RPM_ACTIVE		BIT(3)
#define DL_FLAG_AUTOREMOVE_SUPPLIER	BIT(4)
#define DL_FLAG_AUTOPROBE_CONSUMER	BIT(5)
#define DL_FLAG_MANAGED			BIT(6)
#define DL_FLAG_SYNC_STATE_ONLY		BIT(7)
#define DL_FLAG_INFERRED		BIT(8)
#define DL_FLAG_CYCLE			BIT(9)

/**
 * struct device_link - Device link representation.
 * @supplier: The device on the supplier end of the link.
 * @s_node: Hook to the supplier device's list of links to consumers.
 * @consumer: The device on the consumer end of the link.
 * @c_node: Hook to the consumer device's list of links to suppliers.
 * @link_dev: device used to expose link details in sysfs
 * @status: The state of the link (with respect to the presence of drivers).
 * @flags: Link flags.
 * @rpm_active: Whether or not the consumer device is runtime-PM-active.
 * @kref: Count repeated addition of the same link.
 * @rm_work: Work structure used for removing the link.
 * @supplier_preactivated: Supplier has been made active before consumer probe.
 */
struct device_link {
	struct device *supplier;
	struct list_head s_node;
	struct device *consumer;
	struct list_head c_node;
	struct device link_dev;
	enum device_link_state status;
	u32 flags;
	refcount_t rpm_active;
	struct kref kref;
	struct work_struct rm_work;
	bool supplier_preactivated; /* Owned by consumer probe. */
};

/* Device links interface. */
struct device_link *device_link_add(struct device *consumer,
				    struct device *supplier, u32 flags);
void device_link_del(struct device_link *link);
void device_link_remove(void *consumer, struct device *supplier);
void device_links_supplier_sync_state_pause(void);
void device_links_supplier_sync_state_resume(void);
void device_link_wait_removal(void);

static inline bool device_link_test(const struct device_link *link, u32 flags)
{
	return !!(link->flags & flags);
}

#endif /* _DEVICE_LINK_H_ */
