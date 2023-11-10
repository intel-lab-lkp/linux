/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __NET_TC_BLOCKCAST_H
#define __NET_TC_BLOCKCAST_H

#include <net/act_api.h>
#include <linux/tc_act/tc_blockcast.h>

struct tcf_blockcast_act {
	struct tc_action common;
	u32 blockid;
	enum tc_blockcast_tx_type tx_type;
};

#define to_blockcast_act(a) ((struct tcf_blockcast_act *)a)

#endif /* __NET_TC_BLOCKCAST_H */
