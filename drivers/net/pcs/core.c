// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022, 2025 Sean Anderson <sean.anderson@seco.com>
 */

#define pr_fmt(fmt) "pcs-core: " fmt

#include <linux/fwnode.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pcs.h>
#include <linux/phylink.h>
#include <linux/property.h>
#include <linux/rcupdate.h>

static LIST_HEAD(pcs_wrappers);
static DEFINE_MUTEX(pcs_mutex);

/**
 * struct pcs_wrapper - Wrapper for a registered PCS
 * @pcs: the wrapping PCS
 * @ssp: SRCU protecting @wrapped
 * @refcnt: refcount for the wrapper
 * @list: list head for pcs_wrappers
 * @dev: the device associated with this PCS
 * @wrapped: the backing PCS
 */
struct pcs_wrapper {
	struct phylink_pcs pcs;
	struct srcu_struct ssp;
	refcount_t refcnt;
	struct list_head list;
	struct device *dev;
	struct phylink_pcs *wrapped;
};

static const struct phylink_pcs_ops pcs_ops;

static struct pcs_wrapper *pcs_to_wrapper(struct phylink_pcs *pcs)
{
	WARN_ON(pcs->ops != &pcs_ops);
	return container_of(pcs, struct pcs_wrapper, pcs);
}

static int pcs_validate(struct phylink_pcs *pcs, unsigned long *supported,
			const struct phylink_link_state *state)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int ret, idx;

	if (!wrapper)
		return 0;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped) {
		if (wrapped->ops->pcs_validate)
			ret = wrapped->ops->pcs_validate(wrapped, supported,
							 state);
		else
			ret = 0;
	} else {
		ret = -ENODEV;
	}

	srcu_read_unlock(&wrapper->ssp, idx);
	return ret;
}

static unsigned int pcs_inband_caps(struct phylink_pcs *pcs,
				    phy_interface_t interface)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int ret, idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped && wrapped->ops->pcs_inband_caps)
		ret = wrapped->ops->pcs_inband_caps(wrapped, interface);
	else
		ret = 0;

	srcu_read_unlock(&wrapper->ssp, idx);
	return ret;
}

static int pcs_enable(struct phylink_pcs *pcs)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int ret, idx;

	if (!wrapper)
		return 0;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped) {
		if (wrapped->ops->pcs_enable)
			ret = wrapped->ops->pcs_enable(wrapped);
		else
			ret = 0;
	} else {
		ret = -ENODEV;
	}

	srcu_read_unlock(&wrapper->ssp, idx);
	return ret;
}

static void pcs_disable(struct phylink_pcs *pcs)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped && wrapped->ops->pcs_disable)
		wrapped->ops->pcs_disable(wrapped);

	srcu_read_unlock(&wrapper->ssp, idx);
}

static void pcs_get_state(struct phylink_pcs *pcs, unsigned int neg_mode,
			  struct phylink_link_state *state)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped)
		wrapped->ops->pcs_get_state(wrapped, neg_mode, state);
	else
		state->link = 0;

	srcu_read_unlock(&wrapper->ssp, idx);
}

static void pcs_pre_config(struct phylink_pcs *pcs,
			   phy_interface_t interface)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped && wrapped->ops->pcs_pre_config)
		wrapped->ops->pcs_pre_config(wrapped, interface);

	srcu_read_unlock(&wrapper->ssp, idx);
}

static int pcs_post_config(struct phylink_pcs *pcs,
			   phy_interface_t interface)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int ret, idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (pcs && wrapped->ops->pcs_post_config)
		ret = wrapped->ops->pcs_post_config(wrapped, interface);
	else
		ret = 0;

	srcu_read_unlock(&wrapper->ssp, idx);
	return ret;
}

static int pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
		      phy_interface_t interface,
		      const unsigned long *advertising,
		      bool permit_pause_to_mac)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int ret, idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped)
		ret = wrapped->ops->pcs_config(wrapped, neg_mode, interface,
					   advertising, permit_pause_to_mac);
	else
		ret = -ENODEV;

	srcu_read_unlock(&wrapper->ssp, idx);
	return ret;
}

static void pcs_an_restart(struct phylink_pcs *pcs)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped)
		wrapped->ops->pcs_an_restart(wrapped);

	srcu_read_unlock(&wrapper->ssp, idx);
}

static void pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
			phy_interface_t interface, int speed, int duplex)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped && wrapped->ops->pcs_link_up)
		wrapped->ops->pcs_link_up(wrapped, neg_mode, interface, speed,
					  duplex);

	srcu_read_unlock(&wrapper->ssp, idx);
}

static void pcs_disable_eee(struct phylink_pcs *pcs)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped && wrapped->ops->pcs_disable_eee)
		wrapped->ops->pcs_disable_eee(wrapped);

	srcu_read_unlock(&wrapper->ssp, idx);
}

static void pcs_enable_eee(struct phylink_pcs *pcs)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped && wrapped->ops->pcs_enable_eee)
		wrapped->ops->pcs_enable_eee(wrapped);

	srcu_read_unlock(&wrapper->ssp, idx);
}

static int pcs_pre_init(struct phylink_pcs *pcs)
{
	struct pcs_wrapper *wrapper = pcs_to_wrapper(pcs);
	struct phylink_pcs *wrapped;
	int ret, idx;

	idx = srcu_read_lock(&wrapper->ssp);

	wrapped = srcu_dereference(wrapper->wrapped, &wrapper->ssp);
	if (wrapped) {
		wrapped->rxc_always_on = pcs->rxc_always_on;
		if (wrapped->ops->pcs_pre_init)
			ret = wrapped->ops->pcs_pre_init(wrapped);
		else
			ret = 0;
	} else {
		ret = -ENODEV;
	}

	srcu_read_unlock(&wrapper->ssp, idx);
	return ret;
}

static const struct phylink_pcs_ops pcs_ops = {
	.pcs_validate = pcs_validate,
	.pcs_inband_caps = pcs_inband_caps,
	.pcs_enable = pcs_enable,
	.pcs_disable = pcs_disable,
	.pcs_pre_config = pcs_pre_config,
	.pcs_post_config = pcs_post_config,
	.pcs_get_state = pcs_get_state,
	.pcs_config = pcs_config,
	.pcs_an_restart = pcs_an_restart,
	.pcs_link_up = pcs_link_up,
	.pcs_disable_eee = pcs_disable_eee,
	.pcs_enable_eee = pcs_enable_eee,
	.pcs_pre_init = pcs_pre_init,
};

static void pcs_change_callback(void *priv, bool up)
{
	struct pcs_wrapper *wrapper = priv;

	phylink_pcs_change(&wrapper->pcs, up);
}

/**
 * pcs_register() - register a new PCS
 * @dev: The device requesting the PCS
 * @pcs: The PCS to register
 *
 * Registers a new PCS which can be attached to a phylink.
 *
 * Return: 0 on success, or -errno on error
 */
int pcs_register(struct device *dev, struct phylink_pcs *pcs)
{
	struct pcs_wrapper *wrapper;

	if (!dev || !pcs->ops)
		return -EINVAL;

	if (!pcs->ops->pcs_an_restart || !pcs->ops->pcs_config ||
	    !pcs->ops->pcs_get_state)
		return -EINVAL;

	wrapper = kzalloc(sizeof(*wrapper), GFP_KERNEL);
	if (!wrapper)
		return -ENOMEM;

	init_srcu_struct(&wrapper->ssp);
	refcount_set(&wrapper->refcnt, 1);
	INIT_LIST_HEAD(&wrapper->list);
	wrapper->dev = dev;
	RCU_INIT_POINTER(wrapper->wrapped, pcs);

	wrapper->pcs.ops = &pcs_ops;
	wrapper->pcs.poll = pcs->poll;
	bitmap_copy(wrapper->pcs.supported_interfaces, pcs->supported_interfaces,
		    PHY_INTERFACE_MODE_MAX);

	pcs->link_change = pcs_change_callback;
	pcs->link_change_priv = wrapper;

	mutex_lock(&pcs_mutex);
	list_add(&wrapper->list, &pcs_wrappers);
	mutex_unlock(&pcs_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(pcs_register);

static void pcs_destroy(struct pcs_wrapper *wrapper)
{
	cleanup_srcu_struct(&wrapper->ssp);
	kfree(wrapper);
}

/**
 * pcs_unregister() - unregister a PCS
 * @pcs: a PCS previously registered with pcs_register()
 */
void pcs_unregister(struct phylink_pcs *pcs)
{
	struct pcs_wrapper *wrapper;
	struct phylink_pcs *wrapped;

	mutex_lock(&pcs_mutex);
	list_for_each_entry(wrapper, &pcs_wrappers, list) {
		if (wrapper->wrapped == pcs)
			goto found;
	}

	mutex_unlock(&pcs_mutex);
	WARN(1, "trying to unregister an already-unregistered PCS\n");
	return;

found:
	list_del(&wrapper->list);
	wrapped = rcu_replace_pointer(wrapper->wrapped, NULL, true);
	mutex_unlock(&pcs_mutex);
	synchronize_srcu(&wrapper->ssp);

	if (!wrapper->pcs.poll)
		phylink_pcs_change(&wrapper->pcs, false);
	if (refcount_dec_and_test(&wrapper->refcnt))
		pcs_destroy(wrapper);
}
EXPORT_SYMBOL_GPL(pcs_unregister);

static void devm_pcs_release(struct device *dev, void *res)
{
	pcs_unregister(*(struct phylink_pcs **)res);
}

/**
 * devm_pcs_register - resource managed pcs_register()
 * @dev: device that is registering this PCS
 * @pcs: the PCS to register
 *
 * Managed pcs_register(). For PCSs registered by this function,
 * pcs_unregister() is automatically called on driver detach. See
 * pcs_register() for more information.
 *
 * Return: 0 on success, or -errno on failure
 */
int devm_pcs_register(struct device *dev, struct phylink_pcs *pcs)
{
	struct phylink_pcs **pcsp;
	int ret;

	pcsp = devres_alloc(devm_pcs_release, sizeof(*pcsp),
			    GFP_KERNEL);
	if (!pcsp)
		return -ENOMEM;

	ret = pcs_register(dev, pcs);
	if (ret) {
		devres_free(pcsp);
		return ret;
	}

	*pcsp = pcs;
	devres_add(dev, pcsp);

	return ret;
}
EXPORT_SYMBOL_GPL(devm_pcs_register);

/**
 * _pcs_get_tail() - Look up and request a PCS
 * @dev: The device requesting the PCS
 * @fwnode: The PCS's fwnode
 * @pcs_dev: The PCS's device
 *
 * Search PCSs registered with pcs_register() for one with a matching
 * fwnode or device. Either @fwnode or @pcs_dev may be %NULL if matching
 * against a fwnode or device is not desired (respectively).
 *
 * Once a PCS is found, perform common operations necessary when getting a PCS
 * (increment reference counts, etc).
 *
 * You should probably call one of the pcs_get* functions instead of this one.
 *
 * Return: A PCS, or an error pointer on failure. If both @fwnode and @pcs_dev are
 * *       %NULL, returns %NULL to allow easier chaining.
 */
struct phylink_pcs *_pcs_get_tail(struct device *dev,
				  const struct fwnode_handle *fwnode,
				  const struct device *pcs_dev)
{
	struct pcs_wrapper *wrapper;

	if (!fwnode && !pcs_dev)
		return NULL;

	pr_debug("looking for %pfwf or %s %s...\n", fwnode,
		 pcs_dev ? dev_driver_string(pcs_dev) : "(null)",
		 pcs_dev ? dev_name(pcs_dev) : "(null)");

	/* We need to hold this until we get to device_link_add. Otherwise,
	 * someone could unbind the PCS driver.
	 */
	mutex_lock(&pcs_mutex);
	list_for_each_entry(wrapper, &pcs_wrappers, list) {
		if (pcs_dev && wrapper->dev == pcs_dev)
			goto found;
		if (fwnode && wrapper->dev && wrapper->dev->fwnode == fwnode)
			goto found;
	}
	mutex_unlock(&pcs_mutex);
	pr_debug("...not found\n");
	return ERR_PTR(-EPROBE_DEFER);

found:
	pr_debug("...found\n");

	refcount_inc(&wrapper->refcnt);
	get_device(wrapper->dev);

	mutex_unlock(&pcs_mutex);
	return &wrapper->pcs;
}
EXPORT_SYMBOL_GPL(_pcs_get_tail);

/**
 * pcs_find_fwnode() - Find a PCS's fwnode
 * @mac_node: The fwnode referencing the PCS
 * @id: The name of the PCS to get. May be %NULL to get the first PCS.
 * @fallback: An optional fallback property to use if pcs-handle is absent
 * @optional: Whether the PCS is optional
 *
 * Find a PCS's fwnode, as referenced by @mac_node. This fwnode can later be
 * used with _pcs_get_tail() to get the actual PCS. ``pcs-handle-names`` is
 * used to match @id, then the fwnode is found using ``pcs-handle``.
 *
 * This function is internal to the PCS subsystem from a consumer
 * point-of-view. However, it may be used to implement fallbacks for legacy
 * behavior in PCS providers.
 *
 * Return: %NULL if @optional is set and the PCS cannot be found. Otherwise,
 * *       returns a PCS if found or an error pointer on failure.
 */
struct fwnode_handle *pcs_find_fwnode(const struct fwnode_handle *mac_node,
				      const char *id, const char *fallback,
				      bool optional)
{
	int index;
	struct fwnode_handle *pcs_fwnode;

	if (!mac_node)
		return optional ? NULL : ERR_PTR(-ENODEV);

	if (id)
		index = fwnode_property_match_string(mac_node,
						     "pcs-handle-names", id);
	else
		index = 0;

	if (index < 0) {
		if (optional && (index == -EINVAL || index == -ENODATA))
			return NULL;
		return ERR_PTR(index);
	}

	/* First try pcs-handle, and if that doesn't work try the fallback */
	pcs_fwnode = fwnode_find_reference(mac_node, "pcs-handle", index);
	if (PTR_ERR(pcs_fwnode) == -ENOENT && fallback)
		pcs_fwnode = fwnode_find_reference(mac_node, fallback, index);
	if (optional && !id && PTR_ERR(pcs_fwnode) == -ENOENT)
		return NULL;
	return pcs_fwnode;
}
EXPORT_SYMBOL_GPL(pcs_find_fwnode);

/**
 * _pcs_get() - Get a PCS from a fwnode property
 * @dev: The device to get a PCS for
 * @fwnode: The fwnode to find the PCS with
 * @id: The name of the PCS to get. May be %NULL to get the first PCS.
 * @fallback: An optional fallback property to use if pcs-handle is absent
 * @optional: Whether the PCS is optional
 *
 * Find a PCS referenced by @mac_node and return a reference to it. Every call
 * to _pcs_get_by_fwnode() must be balanced with one to pcs_put().
 *
 * Return: a PCS if found, %NULL if not, or an error pointer on failure
 */
struct phylink_pcs *_pcs_get(struct device *dev, struct fwnode_handle *fwnode,
			     const char *id, const char *fallback,
			     bool optional)
{
	struct fwnode_handle *pcs_fwnode;
	struct phylink_pcs *pcs;

	pcs_fwnode = pcs_find_fwnode(fwnode, id, fallback, optional);
	if (IS_ERR(pcs_fwnode))
		return ERR_CAST(pcs_fwnode);

	pcs = _pcs_get_tail(dev, pcs_fwnode, NULL);
	fwnode_handle_put(pcs_fwnode);
	return pcs;
}
EXPORT_SYMBOL_GPL(_pcs_get);

static __maybe_unused void of_changeset_cleanup(void *data)
{
	struct of_changeset *ocs = data;

	if (WARN(of_changeset_revert(ocs),
		 "could not revert changeset; leaking memory\n"))
		return;

	of_changeset_destroy(ocs);
	kfree(ocs);
}

/**
 * pcs_get_by_fwnode_compat() - Get a PCS with a compatibility fallback
 * @dev: The device requesting the PCS
 * @fwnode: The &struct fwnode_handle of the PCS itself
 * @fixup: Callback to fix up @fwnode for compatibility
 * @data: Passed to @fixup
 *
 * This function looks up a PCS and retries on failure after fixing up @fwnode.
 * It is intended to assist in backwards-compatible behavior for drivers that
 * used to create a PCS directly from a &struct device_node. This function
 * should NOT be used in new drivers.
 *
 * @fixup modifies a devicetree changeset to create any properties necessary to
 * bind the PCS's &struct device_node. At the very least, it should use
 * of_changeset_add_prop_string() to add a compatible property.
 *
 * Note that unlike pcs_get_by_fwnode, @fwnode is the &struct fwnode_handle of
 * the PCS itself, and not that of the requesting device. @fwnode could be
 * looked up with pcs_find_fwnode() or determined by some other means for
 * compatibility.
 *
 * Return: A PCS on success or an error pointer on failure
 */
struct phylink_pcs *
pcs_get_by_fwnode_compat(struct device *dev, struct fwnode_handle *fwnode,
			 int (*fixup)(struct of_changeset *ocs,
				      struct device_node *np, void *data),
			 void *data)
{
#ifdef CONFIG_OF_DYNAMIC
	struct mdio_device *mdiodev;
	struct of_changeset *ocs;
	struct phylink_pcs *pcs;
	struct device_node *np;
	struct device *pcsdev;
	int err;

	/* First attempt */
	pcs = _pcs_get_tail(dev, fwnode, NULL);
	if (PTR_ERR(pcs) != -EPROBE_DEFER)
		return pcs;

	/* No luck? Maybe there's no compatible... */
	np = to_of_node(fwnode);
	if (!np || of_property_present(np, "compatible"))
		return pcs;

	/* OK, let's try fixing things up */
	pr_warn("%pOF is missing a compatible\n", np);
	ocs = kmalloc(sizeof(*ocs), GFP_KERNEL);
	if (!ocs)
		return ERR_PTR(-ENOMEM);

	of_changeset_init(ocs);
	err = fixup(ocs, np, data);
	if (err)
		goto err_ocs;

	err = of_changeset_apply(ocs);
	if (err)
		goto err_ocs;

	err = devm_add_action_or_reset(dev, of_changeset_cleanup, ocs);
	if (err)
		return ERR_PTR(err);

	mdiodev = fwnode_mdio_find_device(fwnode);
	if (mdiodev) {
		/* Clear that pesky PHY flag so we can match PCS drivers */
		device_lock(&mdiodev->dev);
		mdiodev->flags &= ~MDIO_DEVICE_FLAG_PHY;
		device_unlock(&mdiodev->dev);
		pcsdev = &mdiodev->dev;
	} else {
		pcsdev = get_device(fwnode->dev);
		if (!pcsdev)
			return ERR_PTR(-EPROBE_DEFER);
	}

	err = device_reprobe(pcsdev);
	put_device(pcsdev);
	if (err)
		return ERR_PTR(err);

	return _pcs_get_tail(dev, fwnode, NULL);

err_ocs:
	of_changeset_destroy(ocs);
	kfree(ocs);
	return ERR_PTR(err);
#else
	return _pcs_get_tail(dev, fwnode, NULL);
#endif
}
EXPORT_SYMBOL_GPL(pcs_get_by_fwnode_compat);

/**
 * pcs_put() - Release a previously-acquired PCS
 * @dev: The device used to acquire the PCS
 * @pcs: The PCS to put
 *
 * This frees resources associated with the PCS which were acquired when it was
 * gotten.
 */
void pcs_put(struct device *dev, struct phylink_pcs *pcs)
{
	struct pcs_wrapper *wrapper;

	if (!pcs)
		return;

	wrapper = pcs_to_wrapper(pcs);
	put_device(wrapper->dev);
	if (refcount_dec_and_test(&wrapper->refcnt))
		pcs_destroy(wrapper);
}
EXPORT_SYMBOL_GPL(pcs_put);
