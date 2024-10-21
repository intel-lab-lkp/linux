// SPDX-License-Identifier: GPL-2.0-only
/* binder_genl.c
 *
 * Android IPC Subsystem
 *
 * Copyright (C) 2024 Google, Inc.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/skbuff.h>
#include <linux/string.h>
#include <net/sock.h>
#include <uapi/linux/android/binder.h>

#include "binder_internal.h"
#include "binder_trace.h"

/*
 * The default multicast group
 */
static const struct genl_multicast_group binder_genl_mcgrps[] = {
	{ .name = "binder_genl", },
};

/*
 * The policy to verify the type of the binder genl data
 */
static const struct nla_policy binder_report_policy[BINDER_GENL_ATTR_MAX + 1] = {
	[BINDER_GENL_ATTR_PID] = { .type = NLA_U32 },
	[BINDER_GENL_ATTR_FLAGS] = { .type = NLA_U32 },
};

/**
 * binder_genl_cmd_doit() - .doit handler for BINDER_GENL_CMD_SET_REPORT
 * @skb:	the metadata struct passed from netlink driver
 * @info:	the generic netlink struct passed from netlink driver
 *
 * Implements the .doit function to process binder genl commands.
 */
static int binder_genl_cmd_doit(struct sk_buff *skb, struct genl_info *info)
{
	int len;
	int portid;
	u32 pid;
	u32 flags;
	void *hdr;
	struct binder_context *context;

	/* Both attributes are required for BINDER_GENL_CMD_SET_REPORT */
	if (!info->attrs[BINDER_GENL_ATTR_PID] || !info->attrs[BINDER_GENL_ATTR_FLAGS]) {
		pr_err("Attributes not set\n");
		return -EINVAL;
	}

	portid = nlmsg_hdr(skb)->nlmsg_pid;
	pid = nla_get_u32(info->attrs[BINDER_GENL_ATTR_PID]);
	flags = nla_get_u32(info->attrs[BINDER_GENL_ATTR_FLAGS]);
	context = container_of(info->family, struct binder_context,
			       genl_family);

	if (context->report_portid && context->report_portid != portid) {
		pr_err("No permission to set report flags from %u\n", portid);
		return -EPERM;
	}

	if (binder_genl_set_report(context, pid, flags) < 0) {
		pr_err("Failed to set report flags %u for %u\n", flags, pid);
		return -EINVAL;
	}

	len = nla_total_size(sizeof(pid)) + nla_total_size(sizeof(flags));
	skb = genlmsg_new(len, GFP_KERNEL);
	if (!skb) {
		pr_err("Failed to alloc binder genl reply message\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put_reply(skb, info, info->family, 0,
				BINDER_GENL_CMD_REPLY);
	if (!hdr)
		goto free_skb;

	if (nla_put_u32(skb, BINDER_GENL_ATTR_PID, pid))
		goto cancel_skb;

	if (nla_put_u32(skb, BINDER_GENL_ATTR_FLAGS, flags))
		goto cancel_skb;

	genlmsg_end(skb, hdr);

	if (genlmsg_reply(skb, info)) {
		pr_err("Failed to send binder genl reply message\n");
		return -EFAULT;
	}

	if (!context->report_portid)
		context->report_portid = portid;

	return 0;

cancel_skb:
	pr_err("Failed to add genl header to reply message\n");
	genlmsg_cancel(skb, hdr);

free_skb:
	pr_err("Failed to add genl attribute to reply message\n");
	nlmsg_free(skb);
	return -EMSGSIZE;
}

/*
 * binder_genl_ops - the small version of generic netlink operations
 */
static struct genl_small_ops binder_genl_ops[] = {
	{
		.cmd = BINDER_GENL_CMD_SET_REPORT,
		.doit = binder_genl_cmd_doit,
	}
};

/**
 * binder_genl_init() - initialize binder generic netlink
 * @family:	the generic netlink family
 * @name:	the binder device name
 *
 * Registers the binder generic netlink family.
 */
int binder_genl_init(struct genl_family *family, const char *name)
{
	int ret;

	strscpy(family->name, name, GENL_NAMSIZ);
	family->version = BINDER_GENL_VERSION;
	family->maxattr = BINDER_GENL_ATTR_MAX;
	family->policy	= binder_report_policy;
	family->small_ops = binder_genl_ops;
	family->n_small_ops = ARRAY_SIZE(binder_genl_ops);
	family->mcgrps = binder_genl_mcgrps;
	family->n_mcgrps = ARRAY_SIZE(binder_genl_mcgrps);
	ret = genl_register_family(family);
	if (ret) {
		pr_err("Failed to register binder genl: %s\n", name);
		return ret;
	}

	return 0;
}

/**
 * binder_genl_set_report() - set binder report flags
 * @proc:	the binder_proc calling the ioctl
 * @pid:	the target process
 * @flags:	the flags to set
 *
 * If pid is 0, the flags are applied to the whole binder context.
 * Otherwise, the flags are applied to the specific process only.
 */
int binder_genl_set_report(struct binder_context *context, u32 pid, u32 flags)
{
	struct binder_proc *proc;

	if (flags != (flags & (BINDER_REPORT_ALL | BINDER_REPORT_OVERRIDE))) {
		pr_err("Invalid binder report flags: %u\n", flags);
		return -EINVAL;
	}

	if (!pid) {
		/* Set the global flags for the whole binder context */
		context->report_flags = flags;
	} else {
		/* Set the per-process flags */
		proc = binder_find_proc(pid);
		if (!proc) {
			pr_err("Invalid binder report pid %u\n", pid);
			return -EINVAL;
		}

		proc->report_flags = flags;
	}

	return 0;
}

/**
 * binder_genl_report_enabled() - check if binder genl reports are enabled
 * @proc:	the binder_proc to check
 * @mask:	the categories of binder genl reports
 *
 * Returns true if certain binder genl reports are enabled for this binder
 * proc (when per-process overriding takes effect) or context.
 */
inline bool binder_genl_report_enabled(struct binder_proc *proc, u32 mask)
{
	struct binder_context *context = proc->context;

	if (!context->report_portid)
		return false;

	if (proc->report_flags & BINDER_REPORT_OVERRIDE)
		return (proc->report_flags & mask) != 0;
	else
		return (context->report_flags & mask) != 0;
}

/**
 * binder_genl_send_report() - send one binder genl report
 * @context:	the binder context
 * @report:	the binder genl report to send
 * @len:	the length of the report data
 *
 * Packs the report data into a BINDER_GENL_ATTR_REPORT packet and send it.
 */
void binder_genl_send_report(struct binder_context *context,
			     struct binder_report *report, int len)
{
	int ret;
	struct sk_buff *skb;
	void *hdr;

	trace_binder_send_report(context->name, report, len);

	skb = genlmsg_new(nla_total_size(len), GFP_KERNEL);
	if (!skb) {
		pr_err("Failed to alloc binder genl message\n");
		return;
	}

	hdr = genlmsg_put(skb, 0, atomic_inc_return(&context->report_seq),
			  &context->genl_family, 0, BINDER_GENL_CMD_REPORT);
	if (!hdr) {
		pr_err("Failed to set binder genl header\n");
		kfree_skb(skb);
		return;
	}

	if (nla_put(skb, BINDER_GENL_ATTR_REPORT, len, report)) {
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return;
	}

	genlmsg_end(skb, hdr);

	ret = genlmsg_unicast(&init_net, skb, context->report_portid);
	if (ret < 0)
		pr_err("Failed to send binder genl message to %d: %d\n",
		       context->report_portid, ret);
}
