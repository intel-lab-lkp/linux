// SPDX-License-Identifier: GPL-2.0
/*
 * acpi_genl.c - accessing ACPI events and _DSM functions via netlink
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *
 */

#define pr_fmt(fmt) "ACPI: " fmt

#include <linux/spinlock.h>
#include <linux/export.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/gfp.h>
#include <linux/acpi.h>
#include <net/netlink.h>
#include <net/genetlink.h>

#include "internal.h"

/* ACPI notifier chain */
static BLOCKING_NOTIFIER_HEAD(acpi_chain_head);

int acpi_notifier_call_chain(struct acpi_device *dev, u32 type, u32 data)
{
	struct acpi_bus_event event;

	strscpy(event.device_class, dev->pnp.device_class);
	strscpy(event.bus_id, dev->pnp.bus_id);
	event.type = type;
	event.data = data;
	return (blocking_notifier_call_chain(&acpi_chain_head, 0, (void *)&event)
			== NOTIFY_BAD) ? -EINVAL : 0;
}
EXPORT_SYMBOL(acpi_notifier_call_chain);

int register_acpi_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&acpi_chain_head, nb);
}
EXPORT_SYMBOL(register_acpi_notifier);

int unregister_acpi_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&acpi_chain_head, nb);
}
EXPORT_SYMBOL(unregister_acpi_notifier);

#ifdef CONFIG_NET

int acpi_genl_dsm_invoke(struct sk_buff *skb, struct genl_info *info);

static unsigned int acpi_event_seqnum;

struct acpi_genl_event {
	acpi_device_class device_class;
	char bus_id[15];
	u32 type;
	u32 data;
};

/* attributes of acpi_genl_family */
enum acpi_genl_attr {
	ACPI_GENL_ATTR_UNSPEC,
	ACPI_GENL_ATTR_EVENT,	/* ACPI event info needed by user space */
	ACPI_GENL_ATTR_DSM,	/* User space _DSM execution requests */
	__ACPI_GENL_ATTR_MAX,
};
#define ACPI_GENL_ATTR_MAX (__ACPI_GENL_ATTR_MAX - 1)

/* commands supported by the acpi_genl_family */
enum acpi_genl_cmd {
	ACPI_GENL_CMD_UNSPEC,
	ACPI_GENL_CMD_EVENT,	/* kernel->user notifications for ACPI events */
	ACPI_GENL_CMD_DSM,	/* use<->kernel channel for _DSM execution */
	__ACPI_GENL_CMD_MAX,
};
#define ACPI_GENL_CMD_MAX (__ACPI_GENL_CMD_MAX - 1)

#define ACPI_GENL_FAMILY_NAME		"acpi_event"
#define ACPI_GENL_VERSION		0x01
#define ACPI_GENL_MCAST_GROUP_NAME 	"acpi_mc_group"


static const struct nla_policy acpi_nla_policy[__ACPI_GENL_ATTR_MAX + 1] = {
	[ACPI_GENL_ATTR_DSM] = {.type = NLA_BINARY},
};

static const struct genl_ops acpi_genl_ops[] = {
	{
		.cmd	= ACPI_GENL_CMD_DSM,
		.policy = acpi_nla_policy,
		.doit	= acpi_genl_dsm_invoke,
	},
};

static const struct genl_multicast_group acpi_event_mcgrps[] = {
	{ .name = ACPI_GENL_MCAST_GROUP_NAME, },
};

static struct genl_family acpi_event_genl_family __ro_after_init = {
	.module = THIS_MODULE,
	.name = ACPI_GENL_FAMILY_NAME,
	.version = ACPI_GENL_VERSION,
	.maxattr = ACPI_GENL_ATTR_MAX,
	.ops = acpi_genl_ops,
	.n_ops = ARRAY_SIZE(acpi_genl_ops),
	.mcgrps = acpi_event_mcgrps,
	.n_mcgrps = ARRAY_SIZE(acpi_event_mcgrps),
};

int acpi_bus_generate_netlink_event(const char *device_class,
				      const char *bus_id,
				      u8 type, int data)
{
	struct sk_buff *skb;
	struct nlattr *attr;
	struct acpi_genl_event *event;
	void *msg_header;
	int size;

	/* allocate memory */
	size = nla_total_size(sizeof(struct acpi_genl_event)) +
	    nla_total_size(0);

	skb = genlmsg_new(size, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	/* add the genetlink message header */
	msg_header = genlmsg_put(skb, 0, acpi_event_seqnum++,
				 &acpi_event_genl_family, 0,
				 ACPI_GENL_CMD_EVENT);
	if (!msg_header) {
		nlmsg_free(skb);
		return -ENOMEM;
	}

	/* fill the data */
	attr =
	    nla_reserve(skb, ACPI_GENL_ATTR_EVENT,
			sizeof(struct acpi_genl_event));
	if (!attr) {
		nlmsg_free(skb);
		return -EINVAL;
	}

	event = nla_data(attr);
	memset(event, 0, sizeof(struct acpi_genl_event));

	strscpy(event->device_class, device_class, sizeof(event->device_class));
	strscpy(event->bus_id, bus_id, sizeof(event->bus_id));
	event->type = type;
	event->data = data;

	/* send multicast genetlink message */
	genlmsg_end(skb, msg_header);

	genlmsg_multicast(&acpi_event_genl_family, skb, 0, 0, GFP_ATOMIC);
	return 0;
}

EXPORT_SYMBOL(acpi_bus_generate_netlink_event);

int acpi_genl_dsm_invoke(struct sk_buff *skb, struct genl_info *info)
{
	int ret = 0;
	struct acpi_genl_dsm_id *dsm_arg = NULL, *dsm_ret = NULL;
	u16 dsm_arg_len = 0;
	struct acpi_genl_dsm_handle *handle;
	struct sk_buff *reply_skb;
	struct nlattr *attr;
	void *hdr;

	if (!info->attrs[ACPI_GENL_ATTR_DSM]) {
		ret = -EINVAL;
		goto out;
	}

	dsm_arg = (struct acpi_genl_dsm_id *)
		   nla_data(info->attrs[ACPI_GENL_ATTR_DSM]);

	/* Get the handle for the requested _DSM method. */
	handle = acpi_genl_dsm_get_handle(dsm_arg);
	if (!handle) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Make sure massage length matches the _DSM method argument size
	 * specified in the _DSM handle registered by the dirver.
	 */
	dsm_arg_len = nla_len(info->attrs[ACPI_GENL_ATTR_DSM]);
	if (dsm_arg_len != handle->arg_len) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Make sure the user-space caller has right capabilities to invoke this
	 * _DSM handle.
	 */
	if (!capable(handle->cap)) {
		ret = -EACCES;
		goto out;
	}

	/*
	 * Build a response of the size specified in the _DSM method handle.
	 */
	reply_skb = genlmsg_new(handle->ret_len, GFP_ATOMIC);
	if (!skb) {
		ret = -ENOMEM;
		goto out;
	}

	hdr = genlmsg_put_reply(reply_skb, info, &acpi_event_genl_family,
				0, ACPI_GENL_CMD_DSM);
	if (!hdr) {
		nlmsg_free(reply_skb);
		ret = -EMSGSIZE;
		goto out;
	}

	attr = nla_reserve(reply_skb, ACPI_GENL_ATTR_DSM, handle->ret_len);
	if (!attr) {
		nlmsg_free(reply_skb);
		ret = -EMSGSIZE;
		goto out;
	}

	dsm_ret = nla_data(attr);
	memset(dsm_ret, 0, handle->ret_len);

	/*
	 * Invoke the _DSM method via the driver provided callback and send
	 * response.
	 */
	handle->dsm_cb(dsm_arg, dsm_ret);
	genlmsg_end(reply_skb, hdr);
	ret = genlmsg_reply(reply_skb, info);
out:
	return ret;
}

static int __init acpi_event_genetlink_init(void)
{
	return genl_register_family(&acpi_event_genl_family);
}
#else
int acpi_bus_generate_netlink_event(const char *device_class,
				      const char *bus_id,
				      u8 type, int data)
{
	return 0;
}

EXPORT_SYMBOL(acpi_bus_generate_netlink_event);

static int acpi_event_genetlink_init(void)
{
	return -ENODEV;
}
#endif

static int __init acpi_event_init(void)
{
	int error;

	if (acpi_disabled)
		return 0;

	/* create genetlink for acpi event */
	error = acpi_event_genetlink_init();
	if (error)
		pr_warn("Failed to create genetlink family for ACPI event\n");

	return 0;
}

fs_initcall(acpi_event_init);
