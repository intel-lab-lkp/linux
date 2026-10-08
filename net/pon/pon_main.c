// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <net/pon.h>

#include "pon.h"
#include "pon-nl-gen.h"

DEFINE_XARRAY_ALLOC1(pon_devs);
/* guards the pon_devs xarray, taken before any instance lock */
DEFINE_MUTEX(pon_devs_lock);

/**
 * DOC: PON locking
 *
 * pon_devs_lock protects the pon_devs xarray, the conduit list and the
 * pairing of a conduit with an instance.
 * Ordering is take the pon_devs_lock and then the instance lock.
 * rtnl is taken before them and never while an instance lock or pon_devs_lock
 * is held: the GEM netdevs are registered and unregistered outside them and
 * the conduit's address and MTU are set after a pairing once both are
 * dropped.
 * Each instance is protected by RCU and has a refcount.
 * When the driver unregisters, the instance gets flushed, but the struct
 * sticks around.
 *
 * The instance lock is also the activation lock. Every deferred report
 * reaches the state machine through pdev->wq, whose handler takes it. The
 * netlink handlers take the same lock in their pre_doit, so a userspace
 * transaction cannot interleave with a half run transition. pdev->work_lock
 * is the one lock below it, held only to add and remove work items from
 * hard interrupt context.
 */

/**
 * pon_tcont_find() - look up a T-CONT by its index
 * @pdev:	PON device structure
 * @index:	the T-CONT index
 *
 * Context: Called with @pdev->lock held.
 * Return: the T-CONT, or NULL when no T-CONT has @index.
 */
struct pon_tcont *pon_tcont_find(struct pon_dev *pdev, u16 index)
{
	struct pon_tcont *tcont;

	lockdep_assert_held(&pdev->lock);
	list_for_each_entry(tcont, &pdev->tconts, list)
		if (tcont->cfg.index == index)
			return tcont;
	return NULL;
}

/**
 * pon_gem_find() - look up a GEM port by its id
 * @pdev:	PON device structure
 * @gem_id:	the GEM port id
 *
 * Context: Called with @pdev->lock held.
 * Return: the GEM port, or NULL when no GEM port has @gem_id.
 */
struct pon_gem *pon_gem_find(struct pon_dev *pdev, u16 gem_id)
{
	struct pon_gem *gem;

	lockdep_assert_held(&pdev->lock);
	list_for_each_entry(gem, &pdev->gems, list)
		if (gem->cfg.id == gem_id)
			return gem;
	return NULL;
}

/**
 * pon_gem_channel() - the conduit transmit channel of a GEM port
 * @pdev:	PON device structure
 * @gem:	the GEM port
 *
 * Asks the driver's tcont_channel callback for the channel of the T-CONT
 * that @gem rides.
 *
 * Context: Called with @pdev->lock held.
 * Return: the channel, -ENOLINK when @gem rides no T-CONT, the T-CONT does
 * not exist, the driver has no tcont_channel callback or the T-CONT has no
 * channel yet, or another negative errno from the driver.
 */
int pon_gem_channel(struct pon_dev *pdev, const struct pon_gem *gem)
{
	struct pon_tcont *tcont;

	if (!gem->cfg.tcont_valid || !pdev->ops->tcont_channel)
		return -ENOLINK;

	tcont = pon_tcont_find(pdev, gem->cfg.tcont_index);
	if (!tcont)
		return -ENOLINK;

	return pdev->ops->tcont_channel(pdev, &tcont->cfg);
}

/**
 * pon_gem_map_same() - test whether two classifier rules are the same rule
 * @existing:	the rule the instance holds
 * @requested:	the rule a request names
 *
 * A rule is identified by everything it matches on, because two rules for one
 * GEM that differ only in their VLAN are different rules.
 *
 * Return: true when @existing and @requested match on the same fields for the
 * same GEM port.
 */
static bool pon_gem_map_same(const struct pon_gem_map_cfg *existing,
			     const struct pon_gem_map_cfg *requested)
{
	return existing->gem_id == requested->gem_id &&
	       existing->tag_valid == requested->tag_valid &&
	       existing->tagged == requested->tagged &&
	       existing->vid_valid == requested->vid_valid &&
	       existing->vid == requested->vid &&
	       existing->pbit_valid == requested->pbit_valid &&
	       existing->pbit == requested->pbit &&
	       existing->dscp_valid == requested->dscp_valid &&
	       existing->dscp == requested->dscp;
}

/**
 * pon_gem_map_find() - look up an upstream classifier rule
 * @pdev:	PON device structure
 * @cfg:	the rule to look for, compared with pon_gem_map_same()
 *
 * Context: Called with @pdev->lock held.
 * Return: the stored rule, or NULL when the instance holds no such rule.
 */
struct pon_gem_map *pon_gem_map_find(struct pon_dev *pdev,
				     const struct pon_gem_map_cfg *cfg)
{
	struct pon_gem_map *map;

	lockdep_assert_held(&pdev->lock);
	list_for_each_entry(map, &pdev->gem_maps, list)
		if (pon_gem_map_same(&map->cfg, cfg))
			return map;
	return NULL;
}

/**
 * pon_dev_create() - create and register a PON device
 * @netdev:	the PON data network device, already registered, with
 *		per-CPU transmit and receive statistics
 *		(NETDEV_PCPU_STAT_TSTATS) for the conduit's receive path to
 *		count into and bound to its network namespace
 *		(netns_immutable) since before it was registered
 * @parent:	the MAC's device, which the interfaces the core creates
 *		parent onto and whose firmware node a conduit names to be
 *		paired with this instance
 * @ops:	driver callbacks
 * @caps:	device capabilities
 * @mode:	the mode the MAC is configured for, enum pon_mode
 * @priv_ptr:	back-pointer to driver private data
 *
 * Syncs the conduit when one is already registered, so it takes rtnl and must
 * not be called with rtnl held. The activation state starts unknown. The driver
 * reports it through pon_dev_state_report() from its own work.
 *
 * Return: pointer to the allocated PON device, or ERR_PTR.
 */
struct pon_dev *pon_dev_create(struct net_device *netdev,
			       struct device *parent,
			       const struct pon_dev_ops *ops,
			       const struct pon_dev_caps *caps,
			       enum pon_mode mode, void *priv_ptr)
{
	static u32 last_id;
	struct pon_dev *pdev;
	bool paired;
	int err;

	if (WARN_ON(!netdev || !parent || !ops || !caps ||
		    netdev->pcpu_stat_type != NETDEV_PCPU_STAT_TSTATS ||
		    !netdev->netns_immutable ||
		    !ops->tcont_set ||
		    !ops->tcont_clear ||
		    !ops->gem_add ||
		    !ops->gem_del ||
		    !ops->omci_xmit))
		return ERR_PTR(-EINVAL);

	pdev = kzalloc_obj(*pdev, GFP_KERNEL);
	if (!pdev)
		return ERR_PTR(-ENOMEM);

	pdev->main_netdev = netdev;
	pdev->parent = parent;
	pdev->ops = ops;
	pdev->caps = caps;
	pdev->mode = mode;
	pdev->drv_priv = priv_ptr;
	pdev->ploam = PON_PLOAM_STATE_UNKNOWN;

	mutex_init(&pdev->lock);
	spin_lock_init(&pdev->work_lock);
	INIT_LIST_HEAD(&pdev->tconts);
	INIT_LIST_HEAD(&pdev->gems);
	INIT_LIST_HEAD(&pdev->gem_maps);
	xa_init(&pdev->gem_netdevs);
	INIT_LIST_HEAD(&pdev->work_list);
	INIT_WORK(&pdev->work, pon_work_worker);
	pon_log_init(pdev);
	refcount_set(&pdev->refcnt, 1);

	/* Ordered, so that the work items run one at a time and in the order
	 * they were queued. High priority, because the activation and key
	 * exchange timers whose handlers a driver runs on it are short.
	 */
	pdev->wq = alloc_ordered_workqueue("pon-%s", WQ_HIGHPRI, netdev->name);
	if (!pdev->wq) {
		err = -ENOMEM;
		goto err_free;
	}

	pon_omci_init(pdev);

	mutex_lock(&pon_devs_lock);
	err = xa_alloc_cyclic(&pon_devs, &pdev->id, pdev, xa_limit_16b,
			      &last_id, GFP_KERNEL);
	if (err) {
		mutex_unlock(&pon_devs_lock);
		goto err_wq;
	}
	paired = pon_conduit_attach(pdev);
	mutex_lock(&pdev->lock);
	mutex_unlock(&pon_devs_lock);

	pon_nl_notify_dev(pdev, PON_CMD_DEV_ADD_NTF);

	rcu_assign_pointer(netdev->pon_dev, pdev);

	mutex_unlock(&pdev->lock);

	if (paired)
		pon_conduit_sync(pdev);

	return pdev;

err_wq:
	destroy_workqueue(pdev->wq);
err_free:
	mutex_destroy(&pdev->lock);
	kfree(pdev);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(pon_dev_create);

/**
 * pon_dev_free() - free a PON device once its last reference is gone
 * @pdev:	PON device structure
 *
 * Releases the device's id, destroys its workqueue and frees the structure
 * after an RCU grace period.
 *
 * Context: Process context. Takes pon_devs_lock and may sleep.
 */
static void pon_dev_free(struct pon_dev *pdev)
{
	mutex_lock(&pon_devs_lock);
	xa_erase(&pon_devs, pdev->id);
	mutex_unlock(&pon_devs_lock);

	destroy_workqueue(pdev->wq);
	xa_destroy(&pdev->gem_netdevs);
	mutex_destroy(&pdev->lock);
	kfree_rcu(pdev, rcu);
}

/**
 * pon_dev_put() - drop a reference to a PON device
 * @pdev:	PON device structure
 *
 * A driver drops the reference pon_dev_create() returned, once, after
 * pon_dev_unregister() and after it has released everything of its own that
 * names the device: its interrupts, its timers and the data network device.
 */
void pon_dev_put(struct pon_dev *pdev)
{
	if (refcount_dec_and_test(&pdev->refcnt))
		pon_dev_free(pdev);
}
EXPORT_SYMBOL_GPL(pon_dev_put);

/**
 * pon_dev_unregister() - unregister a PON device
 * @pdev:	PON device structure
 *
 * Stops the upstream link through the driver's enable callback when it is
 * enabled, then withdraws the device. Once this returns no netlink request,
 * no work item and no network device of the core reaches the driver. The
 * lines that pon_dev_log() recorded until then are printed before it
 * returns.
 *
 * The device itself stays until the driver calls pon_dev_put(). What a
 * driver still reports to it in between, from an interrupt or from the data
 * network device, is refused.
 *
 * Unregisters network devices, so it takes rtnl and must not be called with
 * rtnl held.
 */
void pon_dev_unregister(struct pon_dev *pdev)
{
	struct pon_gem_map *map, *map_next;
	struct pon_tcont *tcont, *tcont_next;
	struct pon_gem *gem, *gem_next;

	mutex_lock(&pon_devs_lock);
	mutex_lock(&pdev->lock);

	/* Wait until pon_dev_free() to call xa_erase() so this id cannot be
	 * reused while references are still held. Storing NULL makes every
	 * netlink lookup fail from here on.
	 */
	xa_store(&pon_devs, pdev->id, NULL, GFP_KERNEL);
	pon_nl_obj_gen_inc();
	mutex_unlock(&pon_devs_lock);

	if (pdev->enabled) {
		pdev->ops->enable(pdev, false, NULL);
		pdev->enabled = false;
	}

	pon_nl_notify_dev(pdev, PON_CMD_DEV_DEL_NTF);

	WRITE_ONCE(pdev->going_away, true);
	mutex_unlock(&pdev->lock);

	/* First, so that no frame reaches the instance once its objects go. */
	pon_conduit_detach(pdev);

	/* Drop what is queued before stopping the worker, or an item requeued
	 * by the one still running would outlive the cancel.
	 */
	pon_work_drain(pdev);

	/* Each takes the instance lock, so none is waited for with it held. */
	disable_work_sync(&pdev->work);
	flush_work(&pdev->log_work);
	disable_work_sync(&pdev->log_work);

	pon_gem_netdevs_unregister(pdev);

	pon_omci_destroy(pdev);

	mutex_lock(&pdev->lock);

	list_for_each_entry_safe(tcont, tcont_next, &pdev->tconts, list) {
		list_del(&tcont->list);
		kfree(tcont);
	}
	list_for_each_entry_safe(gem, gem_next, &pdev->gems, list) {
		list_del(&gem->list);
		kfree(gem);
	}
	list_for_each_entry_safe(map, map_next, &pdev->gem_maps, list) {
		list_del(&map->list);
		kfree(map);
	}

	rcu_assign_pointer(pdev->main_netdev->pon_dev, NULL);

	WRITE_ONCE(pdev->ops, NULL);
	memzero_explicit(&pdev->identity, sizeof(pdev->identity));

	mutex_unlock(&pdev->lock);

	/* The GEM transmit path reads @ops under RCU. */
	synchronize_net();

	/*
	 * Only now: a transmit that loaded @ops before the store above is
	 * still inside its read side section and reaches the driver, which
	 * reads @drv_priv to find itself.
	 */
	pdev->drv_priv = NULL;
}
EXPORT_SYMBOL_GPL(pon_dev_unregister);

/**
 * pon_init() - register the PON core
 *
 * Registers the generic netlink family, the GEM link type, the netlink
 * notifier that releases the OMCI channels and the netdev notifier that
 * follows the conduits.
 *
 * Runs before the device drivers when built in, because an ethernet driver
 * registers its conduit from its own probe. A module carries the same
 * ordering through its dependencies.
 *
 * Return: 0, or a negative errno when a registration fails.
 */
static int __init pon_init(void)
{
	int err;

	err = genl_register_family(&pon_nl_family);
	if (err)
		return err;

	err = pon_gem_link_register();
	if (err)
		goto err_family;

	err = pon_omci_notifier_register();
	if (err)
		goto err_link;

	err = pon_conduit_notifier_register();
	if (err)
		goto err_omci;

	return 0;

err_omci:
	pon_omci_notifier_unregister();
err_link:
	pon_gem_link_unregister();
err_family:
	genl_unregister_family(&pon_nl_family);
	return err;
}
subsys_initcall(pon_init);

/**
 * pon_exit() - unregister the PON core
 *
 * Undoes pon_init() in the reverse order.
 */
static void __exit pon_exit(void)
{
	pon_conduit_notifier_unregister();
	pon_omci_notifier_unregister();
	pon_gem_link_unregister();
	genl_unregister_family(&pon_nl_family);
}
module_exit(pon_exit);

MODULE_ALIAS_GENL_FAMILY(PON_FAMILY_NAME);
MODULE_AUTHOR("John Crispin <john@phrozen.org>");
MODULE_DESCRIPTION("Passive optical network ONU support");
MODULE_LICENSE("GPL");
