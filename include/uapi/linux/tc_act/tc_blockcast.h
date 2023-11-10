/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __LINUX_TC_BLOCKCAST_H
#define __LINUX_TC_BLOCKCAST_H

#include <linux/types.h>
#include <linux/pkt_cls.h>

struct tc_blockcast {
	tc_gen;
	__u32                   blockid;  /* block ID to which we'll blockcast */
};

enum {
	TCA_BLOCKCAST_UNSPEC,
	TCA_BLOCKCAST_TM,
	TCA_BLOCKCAST_PARMS,
	TCA_BLOCKCAST_TX_TYPE,
	TCA_BLOCKCAST_PAD,
	__TCA_BLOCKCAST_MAX
};

#define TCA_BLOCKCAST_MAX (__TCA_BLOCKCAST_MAX - 1)

enum tc_blockcast_tx_type {
	TCA_BLOCKCAST_TX_TYPE_BROADCAST,
	TCA_BLOCKCAST_TX_TYPE_ALL,
	__TCA_BLOCKCAST_TX_TYPE_MAX,
};

#define TCA_BLOCKCAST_TX_TYPE_MAX (__TCA_BLOCKCAST_TX_TYPE_MAX - 1)

#endif
