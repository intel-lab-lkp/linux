// SPDX-License-Identifier: GPL-2.0-only
/* binder_genl.c
 *
 * Android IPC Subsystem
 *
 * Copyright (C) 2024 Google, Inc.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/genetlink.h>
#include <uapi/linux/android/binder.h>

#include "binder_genl.h"
#include "binder_trace.h"

/**
 * The registered process that would receive binder reports.
 */
static u32 binder_report_pid;

/**
 * The policy to verify the type of the binder genl data
 */
static const struct nla_policy binder_report_policy[BINDER_GENL_ATTR_MAX + 1] = {
	[BINDER_GENL_ATTR_PID] = { .type = NLA_U32 },
};

static struct genl_family binder_gnl_family;

/**
 * binder_genl_cmd_register() - register the pid to receive binder genl reports
 * @info:	the generic netlink struct passed from netlink driver
 * @pid:	the process to receive binder genl reports
 *
 * Called by .doit to register the pid, which is then replied to the caller to
 * complete the handshake.
 */
static int binder_genl_cmd_register(struct genl_info *info, u32 pid)
{
	int len;
	struct sk_buff *skb;
	void *hdr;

	len = sizeof(pid);
	skb = genlmsg_new(len, GFP_KERNEL);
	if (!skb) {
		pr_err("Failed to alloc binder genl message\n");
		return -ENOMEM;
	}

	hdr = genlmsg_put(skb, pid, 0, &binder_gnl_family, 0,
			  BINDER_GENL_CMD_REPLY);
	if (!hdr) {
		pr_err("Failed to set binder genl header\n");
		kfree_skb(skb);
		return -EMSGSIZE;
	}

	if (nla_put(skb, BINDER_GENL_ATTR_PID, len, &pid)) {
		genlmsg_cancel(skb, hdr);
		nlmsg_free(skb);
		return -EMSGSIZE;
	}

	genlmsg_end(skb, hdr);

	if (genlmsg_reply(skb, info)) {
		pr_err("Failed to send binder genl message\n");
		return -EFAULT;
	}

	binder_report_pid = pid;

	return 0;
}

/**
 * binder_genl_cmd_doit() - the .doit handler of binder genl commands
 * @skb:	the metadata struct passed from netlink driver
 * @info:	the generic netlink struct passed from netlink driver
 *
 * Implements the .doit function to process binder genl commands.
 */
static int binder_genl_cmd_doit(struct sk_buff *skb, struct genl_info *info)
{
	int pid;

	pid = nlmsg_hdr(skb)->nlmsg_pid;

	/* The only cmd is to register the process to receive binder reports */
	return binder_genl_cmd_register(info, pid);
}

/**
 * binder_genl_ops - the small version of generic netlink operations
 *
 * Supports only 1 command right now for registration handshake.
 */
static struct genl_small_ops binder_genl_ops[] = {
	{
		.cmd = BINDER_GENL_CMD_REGISTER,
		.doit = binder_genl_cmd_doit,
	}
};

/**
 * binder_gnl_family - the binder generic netlink family
 *
 * Defines policies and supported operations of binder genl
 */
static struct genl_family binder_gnl_family = {
	.name = BINDER_GENL_FAMILY_NAME,
	.version = BINDER_GENL_VERSION,
	.maxattr = BINDER_GENL_ATTR_MAX,
	.policy	= binder_report_policy,
	.small_ops = binder_genl_ops,
	.n_small_ops = ARRAY_SIZE(binder_genl_ops),
};

/**
 * binder_genl_init() - initialize binder generic netlink
 *
 * Registers the binder generic netlink family.
 */
int __init binder_genl_init(void)
{
	int ret = genl_register_family(&binder_gnl_family);

	if (ret) {
		pr_err("Failed to register binder genl\n");
		return ret;
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

	if (proc->report_flags & BINDER_REPORT_OVERRIDE)
		return (proc->report_flags & mask) != 0;
	else
		return (context->report_flags & mask) != 0;
}

/**
 * binder_genl_send_report() - send one binder genl report
 * @report:	the binder genl report to send
 * @len:	the length of the report data
 *
 * Packs the report data into a BINDER_GENL_ATTR_REPORT packet and sends them
 * to the previously registered process.
 */
void binder_genl_send_report(struct binder_report *report, int len)
{
	struct sk_buff *skb;
	void *hdr;

	trace_binder_send_report(report, len);

	skb = genlmsg_new(len, GFP_KERNEL);
	if (!skb) {
		pr_err("Failed to alloc binder genl message\n");
		return;
	}

	hdr = genlmsg_put(skb, binder_report_pid, 0, &binder_gnl_family, 0,
			  BINDER_GENL_CMD_REPORT);
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

	if (genlmsg_unicast(&init_net, skb, binder_report_pid))
		pr_err("Failed to send binder genl message\n");
}
