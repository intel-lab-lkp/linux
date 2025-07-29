// SPDX-License-Identifier: GPL-2.0
/*
 * Network device TX clock control framework
 * Simple sysfs interface for userspace TX clock management
 * Author: Arkadiusz Kubalewski <arkadiusz.kubalewski@intel.com>
 */

#include <linux/netdevice.h>
#include <linux/netdev_tx_clk.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/slab.h>

/* Simple clock entry structure */
struct tx_clk_entry {
	char name[32];
	const struct netdev_tx_clk_ops *ops;
	void *priv_data;
	struct kobj_attribute attr;
	struct list_head list;
};

static DEFINE_MUTEX(tx_clk_mutex);

static ssize_t tx_clk_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	struct tx_clk_entry *entry = container_of(attr, struct tx_clk_entry,
						  attr);
	int ret = entry->ops->is_enabled(entry->priv_data);

	if (ret != 0 && ret != 1)
		return ret;

	return sprintf(buf, "%d\n", ret);
}

static ssize_t tx_clk_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct tx_clk_entry *entry = container_of(attr, struct tx_clk_entry,
						  attr);
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	/* Cannot disable - one clock must always be active */
	if (val != 1)
		return -EINVAL;

	mutex_lock(&tx_clk_mutex);
	ret = entry->ops->enable(entry->priv_data);
	mutex_unlock(&tx_clk_mutex);

	return ret ? ret : count;
}

/**
 * netdev_tx_clk_register - register a TX clock for a network device
 * @ndev: network device
 * @clk_name: clock name (visible in sysfs)
 * @ops: clock operations
 * @priv_data: private data for callbacks
 *
 * Returns 0 on success, negative error code on failure
 */
int netdev_tx_clk_register(struct net_device *ndev, const char *clk_name,
			   const struct netdev_tx_clk_ops *ops,
			   void *priv_data)
{
	struct tx_clk_entry *entry;
	int ret;

	if (WARN_ON(!ndev || !clk_name || !ops))
		return -EINVAL;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	strscpy(entry->name, clk_name, sizeof(entry->name) - 1);
	entry->ops = ops;
	entry->priv_data = priv_data;
	INIT_LIST_HEAD(&entry->list);

	/* Setup sysfs attribute */
	entry->attr.attr.name = entry->name;
	entry->attr.attr.mode = 0644;
	entry->attr.show = tx_clk_show;
	entry->attr.store = tx_clk_store;

	mutex_lock(&tx_clk_mutex);

	if (!ndev->tx_clk_dir) {
		INIT_LIST_HEAD(&ndev->tx_clk_list);
		ndev->tx_clk_dir = kobject_create_and_add("tx_clk", &ndev->dev.kobj);
		if (!ndev->tx_clk_dir) {
			kfree(entry);
			mutex_unlock(&tx_clk_mutex);
			return -ENOMEM;
		}
	}

	/* Add to device's clock list */
	list_add_tail(&entry->list, &ndev->tx_clk_list);

	/* Create sysfs file */
	ret = sysfs_create_file(ndev->tx_clk_dir, &entry->attr.attr);
	if (ret) {
		list_del(&entry->list);
		kfree(entry);
		mutex_unlock(&tx_clk_mutex);
		return ret;
	}

	mutex_unlock(&tx_clk_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(netdev_tx_clk_register);

/**
 * netdev_tx_clk_cleanup - cleanup all TX clocks for a network device
 * @ndev: network device
 */
void netdev_tx_clk_cleanup(struct net_device *ndev)
{
	struct tx_clk_entry *entry, *tmp;

	if (!ndev)
		return;

	mutex_lock(&tx_clk_mutex);

	list_for_each_entry_safe(entry, tmp, &ndev->tx_clk_list, list) {
		sysfs_remove_file(ndev->tx_clk_dir, &entry->attr.attr);
		list_del(&entry->list);
		kfree(entry);
	}

	if (ndev->tx_clk_dir) {
		kobject_put(ndev->tx_clk_dir);
		ndev->tx_clk_dir = NULL;
	}

	mutex_unlock(&tx_clk_mutex);
}
EXPORT_SYMBOL_GPL(netdev_tx_clk_cleanup);
