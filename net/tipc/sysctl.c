// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * net/tipc/sysctl.c: sysctl interface to TIPC subsystem
 *
 * Copyright (c) 2013, Wind River Systems
 * All rights reserved.
 */

#include "core.h"
#include "trace.h"
#include "crypto.h"
#include "bcast.h"
#include <linux/sysctl.h>

static struct ctl_table_header *tipc_ctl_hdr;

static struct ctl_table tipc_table[] = {
	{
		.procname	= "tipc_rmem",
		.data		= &sysctl_tipc_rmem,
		.maxlen		= sizeof(sysctl_tipc_rmem),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1         = SYSCTL_ONE,
	},
	{
		.procname	= "named_timeout",
		.data		= &sysctl_tipc_named_timeout,
		.maxlen		= sizeof(sysctl_tipc_named_timeout),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1         = SYSCTL_ZERO,
	},
	{
		.procname       = "sk_filter",
		.data           = &sysctl_tipc_sk_filter,
		.maxlen         = sizeof(sysctl_tipc_sk_filter),
		.mode           = 0644,
		.proc_handler   = proc_doulongvec_minmax,
	},
#ifdef CONFIG_TIPC_CRYPTO
	{
		.procname	= "max_tfms",
		.data		= &sysctl_tipc_max_tfms,
		.maxlen		= sizeof(sysctl_tipc_max_tfms),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1         = SYSCTL_ONE,
	},
	{
		.procname	= "key_exchange_enabled",
		.data		= &sysctl_tipc_key_exchange_enabled,
		.maxlen		= sizeof(sysctl_tipc_key_exchange_enabled),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1         = SYSCTL_ZERO,
		.extra2         = SYSCTL_ONE,
	},
#endif
	{
		.procname	= "bc_retruni",
		.data		= &sysctl_tipc_bc_retruni,
		.maxlen		= sizeof(sysctl_tipc_bc_retruni),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
};

int tipc_register_sysctl(void)
{
	tipc_ctl_hdr = register_net_sysctl(&init_net, "net/tipc", tipc_table);
	if (tipc_ctl_hdr == NULL)
		return -ENOMEM;
	return 0;
}

void tipc_unregister_sysctl(void)
{
	unregister_net_sysctl_table(tipc_ctl_hdr);
}
