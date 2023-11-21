// SPDX-License-Identifier: GPL-2.0

#include <net/netlink.h>
#include <net/genetlink.h>

#include "regnl.h"

#ifdef CONFIG_NET
static unsigned int reg_event_seqnum;
struct reg_genl_event {
	char reg_name[15];
	u64 event;
};

/* attributes of reg_genl_family */
enum {
	REG_GENL_ATTR_UNSPEC,
	REG_GENL_ATTR_EVENT,	/* reg event info needed by user space */
	__REG_GENL_ATTR_MAX,
};

#define REG_GENL_ATTR_MAX (__REG_GENL_ATTR_MAX - 1)

/* commands supported by the reg_genl_family */
enum {
	REG_GENL_CMD_UNSPEC,
	REG_GENL_CMD_EVENT,	/* kernel->user notifications for reg events */
	__REG_GENL_CMD_MAX,
};

#define REG_GENL_CMD_MAX (__REG_GENL_CMD_MAX - 1)

#define REG_GENL_FAMILY_NAME		"reg_event"
#define REG_GENL_VERSION		0x01
#define REG_GENL_MCAST_GROUP_NAME	"reg_mc_group"

static const struct genl_multicast_group reg_event_mcgrps[] = {
	{ .name = REG_GENL_MCAST_GROUP_NAME, },
};

static struct genl_family reg_event_genl_family __ro_after_init = {
	.module = THIS_MODULE,
	.name = REG_GENL_FAMILY_NAME,
	.version = REG_GENL_VERSION,
	.maxattr = REG_GENL_ATTR_MAX,
	.mcgrps = reg_event_mcgrps,
	.n_mcgrps = ARRAY_SIZE(reg_event_mcgrps),
};

int reg_generate_netlink_event(const char *reg_name, u64 event)
{
	struct sk_buff *skb;
	struct nlattr *attr;
	struct reg_genl_event *edata;
	void *msg_header;
	int size;

	/* allocate memory */
	size = nla_total_size(sizeof(struct reg_genl_event)) +
	    nla_total_size(0);

	skb = genlmsg_new(size, GFP_ATOMIC);
	if (!skb)
		return -ENOMEM;

	/* add the genetlink message header */
	msg_header = genlmsg_put(skb, 0, reg_event_seqnum++,
				 &reg_event_genl_family, 0,
				 REG_GENL_CMD_EVENT);
	if (!msg_header) {
		nlmsg_free(skb);
		return -ENOMEM;
	}

	/* fill the data */
	attr = nla_reserve(skb, REG_GENL_ATTR_EVENT, sizeof(struct reg_genl_event));
	if (!attr) {
		nlmsg_free(skb);
		return -EINVAL;
	}

	edata = nla_data(attr);
	memset(edata, 0, sizeof(struct reg_genl_event));

	strscpy(edata->reg_name, reg_name, sizeof(edata->reg_name));
	edata->event = event;

	/* send multicast genetlink message */
	genlmsg_end(skb, msg_header);
	size = genlmsg_multicast(&reg_event_genl_family, skb, 0, 0, GFP_ATOMIC);

	pr_debug("%s -> %llx , ret: %x", reg_name, event, size);
	if (size == -ESRCH)
		pr_debug("multicast message sent, but nobody was listening...\n");
	else if (size)
		pr_debug("failed to send multicast genl message\n");
	else
		pr_debug("multicast message sent %d\n", reg_event_seqnum);

	return 0;
}
EXPORT_SYMBOL(reg_generate_netlink_event);

static int __init reg_event_genetlink_init(void)
{
	return genl_register_family(&reg_event_genl_family);
}

#else
int reg_generate_netlink_event(const char *reg_name, u64 event)
{
	return 0;
}
EXPORT_SYMBOL(reg_generate_netlink_event);

static int reg_event_genetlink_init(void)
{
	return -ENODEV;
}
#endif

static int __init reg_event_init(void)
{
	int error;

	/* create genetlink for acpi event */
	error = reg_event_genetlink_init();
	if (error)
		pr_warn("Failed to create genetlink family for reg event\n");

	return 0;
}

fs_initcall(reg_event_init);
