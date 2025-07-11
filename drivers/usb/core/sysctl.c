// SPDX-License-Identifier: GPL-2.0
/*
 * SPDX-FileCopyrightText: (C) 2025 ANSSI
 *
 * USB Authentication netlink interface
 *
 * Author: Luc Bonnafoux <luc.bonnafoux@ssi.gouv.fr>
 * Author: Nicolas Bouchinet <nicolas.bouchinet@ssi.gouv.fr>
 *
 */

#include <linux/sysctl.h>
#include <linux/usb.h>
#include "authent.h"

static const unsigned long max_ms = 3600;

static const struct ctl_table usb_sysctls[] = {
#ifdef	CONFIG_USB_AUTHENTICATION
	{
		.procname	= "authent_engine_register_timeout",
		.data		= &usb_auth_wait_userspace_timeout,
		.maxlen		= sizeof(usb_auth_wait_userspace_timeout),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= (void*)&max_ms,
	},
	{
		.procname	= "authent_engine_response_timeout",
		.data		= &usb_auth_wait_response_timeout,
		.maxlen		= sizeof(usb_auth_wait_response_timeout),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= (void*)&max_ms,
	},
#endif
};

static struct ctl_table_header *usb_sysctl_table;

int __init usb_register_sysctl(void)
{
	usb_sysctl_table = register_sysctl("kernel/usb", usb_sysctls);
	if (!usb_sysctl_table)
		return -ENOMEM;
	return 0;
}

void usb_unregister_sysctl(void)
{
	unregister_sysctl_table(usb_sysctl_table);
	usb_sysctl_table = NULL;
}
