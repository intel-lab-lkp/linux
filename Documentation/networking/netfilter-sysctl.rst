.. SPDX-License-Identifier: GPL-2.0

=========================
Netfilter Sysfs variables
=========================

/proc/sys/net/netfilter/* Variables:
====================================

nf_log_all_netns - BOOLEAN
	- 0 - disabled (default)
	- not 0 - enabled

	By default, only init_net namespace can log packets into kernel log
	with LOG target; this aims to prevent containers from flooding host
	kernel log. If enabled, this target also works in other network
	namespaces. This variable is only accessible from init_net.

nf_max_table_jumps - INTEGER (count)
	default 8192

	The maximum numbers of jumps a table can contain.  Meeting or exceeding
	this value will cause additional rules to not be added with
	EMLINK being return to the user. This variable is only accessible from
	init_net.
