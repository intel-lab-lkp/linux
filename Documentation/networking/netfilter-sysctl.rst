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

nf_tables_jumps_max_netns - INTEGER (count)
	default 256000

	This is the maximum number of jumps/gotos that a netns can have
	across its tables. This limit prevents packet path soft lockups
	caused by rulesets with too many jumps. This limit does not
	represent the net count of jumps in your ruleset; rather, it
	represents the number of jumps that can be reached when traversing
	the ruleset via a depth-first search (DFS). Note that IPv4 and IPv6
	tables are mutually excluded in this jump count. This limit is
	determined in the control plane, where evaluating the rule
	selectors is not possible; therefore, it represents the
	hypothetical worst case. This limit only applies to non-init_net
	namespaces and can be read for non-init_user_ns namespaces. Meeting
	or exceeding this value will prevent additional rules from being
	added and will return an EMLINK error to the user. Verdict map
	lookups only count as a single jump, therefore, assuming you do not
	use iptables-nft your system, then a good alternative default is 256.
