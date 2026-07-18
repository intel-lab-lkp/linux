// SPDX-License-Identifier: GPL-2.0
/*
 * sysctl_net_gn.c: sysctl interface to net GeoNetworking subsystem
 */

#include <linux/sysctl.h>
#include <net/sock.h>
#include <linux/gn.h>

static int dummy_var = 1234;

static struct ctl_table gn_table[] = { {
	.procname = "gn-dummy-var",
	.data = &dummy_var,
	.maxlen = sizeof(int),
	.mode = 0644,
	.proc_handler = proc_dointvec,
} };

static struct ctl_table_header *gn_table_header;

int __init gn_register_sysctl(void)
{
	gn_table_header = register_net_sysctl(&init_net, "net/gn", gn_table);
	if (!gn_table_header)
		return -ENOMEM;
	return 0;
}

void gn_unregister_sysctl(void)
{
	unregister_net_sysctl_table(gn_table_header);
}
