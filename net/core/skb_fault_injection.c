// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fault-inject.h>
#include <linux/skbuff.h>

static DECLARE_FAULT_ATTR(fail_net_force_skb_realloc);

void skb_might_realloc(struct sk_buff *skb)
{
	if (should_fail(&fail_net_force_skb_realloc, 1))
		pskb_expand_head(skb, 0, 0, GFP_ATOMIC);
}
EXPORT_SYMBOL(skb_might_realloc);

static int __init fail_net_force_skb_realloc_setup(char *str)
{
	return setup_fault_attr(&fail_net_force_skb_realloc, str);
}
__setup("fail_net_force_skb_realloc=", fail_net_force_skb_realloc_setup);

static int __init fail_net_force_skb_realloc_debugfs(void)
{
	struct dentry *dir;

	dir = fault_create_debugfs_attr("fail_net_force_skb_realloc", NULL,
					&fail_net_force_skb_realloc);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	return 0;
}

late_initcall(fail_net_force_skb_realloc_debugfs);
