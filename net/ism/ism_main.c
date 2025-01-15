// SPDX-License-Identifier: GPL-2.0
/*
 *  Internal Shared Memory
 *
 *  Implementation of the ISM class module
 *
 *  Copyright IBM Corp. 2024
 */
#define KMSG_COMPONENT "ism"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/ism.h>

MODULE_DESCRIPTION("Internal Shared Memory class");
MODULE_LICENSE("GPL");

static struct ism_client *clients[MAX_CLIENTS];	/* use an array rather than */
						/* a list for fast mapping  */
static u8 max_client;
static DEFINE_MUTEX(clients_lock);
struct ism_dev_list {
	struct list_head list;
	struct mutex mutex; /* protects ism device list */
};

static struct ism_dev_list ism_dev_list = {
	.list = LIST_HEAD_INIT(ism_dev_list.list),
	.mutex = __MUTEX_INITIALIZER(ism_dev_list.mutex),
};

static void ism_setup_forwarding(struct ism_client *client, struct ism_dev *ism)
{
	unsigned long flags;

	spin_lock_irqsave(&ism->lock, flags);
	ism->subs[client->id] = client;
	spin_unlock_irqrestore(&ism->lock, flags);
}

int ism_register_client(struct ism_client *client)
{
	struct ism_dev *ism;
	int i, rc = -ENOSPC;

	mutex_lock(&ism_dev_list.mutex);
	mutex_lock(&clients_lock);
	for (i = 0; i < MAX_CLIENTS; ++i) {
		if (!clients[i]) {
			clients[i] = client;
			client->id = i;
			if (i == max_client)
				max_client++;
			rc = 0;
			break;
		}
	}
	mutex_unlock(&clients_lock);

	if (i < MAX_CLIENTS) {
		/* initialize with all devices that we got so far */
		list_for_each_entry(ism, &ism_dev_list.list, list) {
			ism->priv[i] = NULL;
			client->add(ism);
			ism_setup_forwarding(client, ism);
		}
	}
	mutex_unlock(&ism_dev_list.mutex);

	return rc;
}
EXPORT_SYMBOL_GPL(ism_register_client);

int ism_unregister_client(struct ism_client *client)
{
	struct ism_dev *ism;
	unsigned long flags;
	int rc = 0;

	mutex_lock(&ism_dev_list.mutex);
	list_for_each_entry(ism, &ism_dev_list.list, list) {
		spin_lock_irqsave(&ism->lock, flags);
		/* Stop forwarding IRQs and events */
		ism->subs[client->id] = NULL;
		for (int i = 0; i < ISM_NR_DMBS; ++i) {
			if (ism->sba_client_arr[i] == client->id) {
				WARN(1, "%s: attempt to unregister '%s' with registered dmb(s)\n",
				     __func__, client->name);
				rc = -EBUSY;
				goto err_reg_dmb;
			}
		}
		spin_unlock_irqrestore(&ism->lock, flags);
	}
	mutex_unlock(&ism_dev_list.mutex);

	mutex_lock(&clients_lock);
	clients[client->id] = NULL;
	if (client->id + 1 == max_client)
		max_client--;
	mutex_unlock(&clients_lock);
	return rc;

err_reg_dmb:
	spin_unlock_irqrestore(&ism->lock, flags);
	mutex_unlock(&ism_dev_list.mutex);
	return rc;
}
EXPORT_SYMBOL_GPL(ism_unregister_client);

int ism_dev_register(struct ism_dev *ism)
{
	int i;

	mutex_lock(&ism_dev_list.mutex);
	mutex_lock(&clients_lock);
	for (i = 0; i < max_client; ++i) {
		if (clients[i]) {
			clients[i]->add(ism);
			ism_setup_forwarding(clients[i], ism);
		}
	}
	mutex_unlock(&clients_lock);
	list_add(&ism->list, &ism_dev_list.list);
	mutex_unlock(&ism_dev_list.mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(ism_dev_register);

void ism_dev_unregister(struct ism_dev *ism)
{
	int i;

	mutex_lock(&ism_dev_list.mutex);
	mutex_lock(&clients_lock);
	for (i = 0; i < max_client; ++i) {
		if (clients[i])
			clients[i]->remove(ism);
	}
	mutex_unlock(&clients_lock);
	list_del_init(&ism->list);
	mutex_unlock(&ism_dev_list.mutex);
}
EXPORT_SYMBOL_GPL(ism_dev_unregister);

static int __init ism_init(void)
{
	memset(clients, 0, sizeof(clients));
	max_client = 0;

	return 0;
}

static void __exit ism_exit(void)
{
}

module_init(ism_init);
module_exit(ism_exit);
