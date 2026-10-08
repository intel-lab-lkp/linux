/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#ifndef __PON_PON_H
#define __PON_PON_H

#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <net/pon.h>

#define PON_GEM_KIND		"gem"

extern struct xarray pon_devs;
extern struct mutex pon_devs_lock;

/**
 * struct pon_tcont - one T-CONT of an instance
 * @list:	entry in the instance's @tconts list
 * @cfg:	the configuration tcont-set last stored
 */
struct pon_tcont {
	struct list_head list;
	struct pon_tcont_cfg cfg;
};

/**
 * struct pon_gem - one GEM port of an instance
 * @list:	entry in the instance's @gems list
 * @cfg:	the configuration gem-new stored
 * @gem_netdev:	the GEM port's network device, or NULL when it has none
 */
struct pon_gem {
	struct list_head list;
	struct pon_gem_cfg cfg;
	struct net_device *gem_netdev;
};

/**
 * struct pon_gem_map - one upstream classifier rule
 * @list:	entry in the instance's @gem_maps list
 * @cfg:	what the rule matches on and the GEM port it selects
 *
 * One upstream classifier rule, kept for the dumps and the notifications.
 */
struct pon_gem_map {
	struct list_head list;
	struct pon_gem_map_cfg cfg;
};

struct pon_tcont *pon_tcont_find(struct pon_dev *pdev, u16 index);
struct pon_gem *pon_gem_find(struct pon_dev *pdev, u16 gem_id);
int pon_gem_channel(struct pon_dev *pdev, const struct pon_gem *gem);
struct pon_gem_map *pon_gem_map_find(struct pon_dev *pdev,
				     const struct pon_gem_map_cfg *cfg);
bool pon_tcont_in_use(struct pon_dev *pdev, u16 index);
bool pon_gems_full(struct pon_dev *pdev);
bool pon_tcont_alloc_taken(struct pon_dev *pdev, u16 index, u16 alloc_id);
int pon_tcont_gems_rebind(struct pon_dev *pdev, u16 index, u16 old_alloc_id,
			  u16 new_alloc_id, struct netlink_ext_ack *extack);

void pon_work_worker(struct work_struct *work);
void pon_work_drain(struct pon_dev *pdev);

void pon_nl_obj_gen_inc(void);
void pon_nl_notify_dev(struct pon_dev *pdev, u32 cmd);
void pon_nl_notify_ploam(struct pon_dev *pdev);
void pon_nl_notify_tcont(struct pon_dev *pdev, struct pon_tcont *tcont,
			 u32 cmd);
void pon_nl_notify_gem(struct pon_dev *pdev, struct pon_gem *gem, u32 cmd);
void pon_nl_notify_gem_map(struct pon_dev *pdev, struct pon_gem_map *map,
			   u32 cmd);

void pon_dev_carrier_update(struct pon_dev *pdev);

/* ITU-T G.988 clause 11.2.5 and Table 11.2-2: header and length are 10
 * bytes and a PDU is at most 1980 bytes including the 4 byte MIC.
 */
#define PON_OMCI_MIN_LEN	10
#define PON_OMCI_MAX_LEN	1976
#define PON_OMCI_MIC_LEN	4
#define PON_OMCI_QUEUE_MAX	512

void pon_omci_init(struct pon_dev *pdev);
void pon_omci_destroy(struct pon_dev *pdev);
int pon_omci_conduit_rx(struct pon_dev *pdev, struct sk_buff *skb,
			bool unverified);
int pon_omci_xmit(struct pon_dev *pdev, const void *pdu, unsigned int len,
		  struct netlink_ext_ack *extack);
int pon_omci_register(struct pon_dev *pdev, u32 portid);
int pon_omci_notifier_register(void);
void pon_omci_notifier_unregister(void);
int pon_nl_omci_ntf(struct pon_dev *pdev, const struct sk_buff *skb);

bool pon_conduit_attach(struct pon_dev *pdev);
void pon_conduit_detach(struct pon_dev *pdev);
void pon_conduit_sync(struct pon_dev *pdev);
bool pon_conduit_running(struct pon_dev *pdev);
void pon_log_init(struct pon_dev *pdev);
__printf(3, 4) void pon_dev_log_level(struct pon_dev *pdev, const char *level,
				      const char *fmt, ...);
int pon_conduit_notifier_register(void);
void pon_conduit_notifier_unregister(void);
struct net_device *pon_conduit_hold(struct pon_dev *pdev,
				    const struct pon_conduit_ops **ops,
				    netdevice_tracker *tracker);

int pon_gem_link_register(void);
void pon_gem_link_unregister(void);
void pon_gem_netdevs_unregister(struct pon_dev *pdev);
int pon_gem_netdev_id(const struct net_device *dev, u16 *gem_id);

/**
 * pon_dev_get() - take a reference to a PON device
 * @pdev:	PON device structure, on which the caller already holds a
 *		reference or which it found under pon_devs_lock
 *
 * The reference is dropped with pon_dev_put().
 *
 * Context: Any context.
 */
static inline void pon_dev_get(struct pon_dev *pdev)
{
	refcount_inc(&pdev->refcnt);
}

/**
 * pon_dev_tryget() - take a reference to a PON device that may be going
 * @pdev:	PON device structure, found under RCU
 *
 * Fails once the last reference is gone and the device waits to be freed.
 *
 * Context: Any context.
 * Return: true when the reference was taken, false otherwise.
 */
static inline bool pon_dev_tryget(struct pon_dev *pdev)
{
	return refcount_inc_not_zero(&pdev->refcnt);
}

/**
 * pon_dev_is_registered() - test that a PON device is still registered
 * @pdev:	PON device structure
 *
 * @pdev->ops survives until the end of pon_dev_unregister(), which frees the
 * instance's objects long before it. @pdev->going_away is set first, so both
 * are tested: a caller that only asked about @pdev->ops could act on a T-CONT
 * or a GEM port that has already been freed.
 *
 * Context: Called with @pdev->lock held.
 * Return: true while the driver is registered and pon_dev_unregister() has
 * not begun, false otherwise.
 */
static inline bool pon_dev_is_registered(struct pon_dev *pdev)
{
	lockdep_assert_held(&pdev->lock);
	return pdev->ops && !pdev->going_away;
}

#endif /* __PON_PON_H */
